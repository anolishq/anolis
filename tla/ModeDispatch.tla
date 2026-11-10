---------------------------- MODULE ModeDispatch ----------------------------
(***************************************************************************)
(* A bounded model of how mode transitions, behavior-tree dispatch, hooks  *)
(* and the software-stop latch interleave in the anolis runtime.           *)
(*                                                                         *)
(* It is a model of a design, not of the code, and a passing check says    *)
(* nothing about the C++. See README.md for what is and is not modelled.   *)
(***************************************************************************)
CONSTANTS
    Ops,      \* ordinary callers of set_mode
    HookAt    \* "before" or "after": where the leave-AUTO hook is declared

Modes == {"IDLE", "MANUAL", "AUTO", "FAULT"}
Fault == "f"
Callers == Ops \cup {Fault}

\* The ordinary transition table (mode_manager.cpp is_valid_transition).
Ordinary == {<<"MANUAL", "AUTO">>, <<"AUTO", "MANUAL">>,
             <<"MANUAL", "IDLE">>, <<"IDLE", "MANUAL">>,
             <<"FAULT", "MANUAL">>}

VARIABLES
    mode,        \* committed runtime mode
    output,      \* "safe" or "driven": the actuator the tree commands
    latched,     \* software-stop latch
    laddered,    \* the stop's ladder has run since it engaged
    hookless,    \* a restart left no leave-AUTO hook covering an output
    pc,          \* per caller: "idle", "before", "commit", "after"
    from,        \* per caller: the mode its before-hooks were chosen for
    target,      \* per caller: the requested mode
    prev,        \* per caller: the mode it actually committed from
    tree,        \* "idle", "node", "wait"
    faultLost,   \* history: a FAULT request ended without FAULT committing
    faultSeen,   \* per caller: a FAULT was requested, in FAULT, since this request began
    autoWhileLatched  \* history: AUTO was committed while the stop latch held

vars == <<mode, output, latched, laddered, hookless, pc, from, target, prev,
          tree, faultLost, faultSeen, autoWhileLatched>>

-----------------------------------------------------------------------------
(* The guards under test. Each is replaced in a regression configuration   *)
(* by the behaviour of a past or suspected bug.                            *)

\* A transition out of AUTO has started and not yet finished its commit.
LeavingAuto ==
    \E c \in Callers : pc[c] \in {"before", "commit"}
                       /\ from[c] = "AUTO" /\ target[c] # "AUTO"

FaultPending == pc[Fault] \in {"before", "commit"}

\* Checked under the provider lock, immediately before dispatch.
TreeMayDispatch == mode = "AUTO" /\ ~LeavingAuto /\ ~latched
\* #762: only the latch was re-checked after the provider lock.
TreeDispatchLatchOnly == ~latched
\* Without the e-stop re-check after the provider lock.
TreeDispatchNoLatch == mode = "AUTO" /\ ~LeavingAuto
\* Re-checks the mode but ignores a transition still in progress.
TreeDispatchModeOnly == mode = "AUTO" /\ ~latched

\* Gates a transition into AUTO re-judges at commit (#239).
AutoGate(c) == target[c] = "AUTO" => (~hookless /\ ~latched)
AutoGateAbsent(c) == TRUE
\* Without the e-stop half of the gate (install_estop_auto_gate).
AutoGateNoLatch(c) == target[c] = "AUTO" => ~hookless

\* A transition out of AUTO, to MANUAL or FAULT, is running its after-hooks.
LeaveAutoAfterRunning == \E c \in Callers : pc[c] = "after" /\ prev[c] = "AUTO"
\* Only an ordinary transition out of AUTO holds the window.
LeaveAutoAfterRunningOrdinaryOnly == \E c \in Ops : pc[c] = "after" /\ prev[c] = "AUTO"

\* #776: IDLE is not entered while an after-hook leaving AUTO has yet to run,
\* since the IDLE gate would refuse its call. Judged at the request and again
\* at the commit: the mode can go MANUAL -> AUTO -> MANUAL in between.
IdleMayStart(t) == t = "IDLE" => ~LeaveAutoAfterRunning
IdleMayCommit(t) == IdleMayStart(t)
\* #776 as found: nothing held IDLE back.
IdleIgnoresAfterHooks(t) == TRUE
\* A fix that missed AUTO -> FAULT.
IdleIgnoresFaultAfterHooks(t) == t = "IDLE" => ~LeaveAutoAfterRunningOrdinaryOnly

\* #774: a recovery overtaken by a FAULT request made in FAULT does not commit.
RecoveryMayCommit(c) == ~faultSeen[c]
\* #774 as found: the FAULT request was a no-op and the recovery committed.
RecoveryIgnoresFault(c) == TRUE

OrdinaryMayCommit(c) == mode = from[c] /\ ~FaultPending /\ AutoGate(c) /\ IdleMayCommit(target[c])
                        /\ RecoveryMayCommit(c)
\* #763: an ordinary transition could commit during a pending FAULT. Only
\* that guard is dropped, so the regression fails for #763 alone.
OrdinaryIgnoresFault(c) == mode = from[c] /\ AutoGate(c) /\ IdleMayCommit(target[c]) /\ RecoveryMayCommit(c)

FaultMayCommit(c) == TRUE
\* #528: a concurrent change made the FAULT refuse.
FaultRefusesConcurrent(c) == mode = from[c]

-----------------------------------------------------------------------------
Init ==
    /\ mode \in {"MANUAL", "AUTO"}
    /\ output = "safe"
    /\ latched = FALSE
    /\ laddered = FALSE
    /\ hookless = FALSE
    /\ pc = [c \in Callers |-> "idle"]
    /\ from = [c \in Callers |-> mode]
    /\ target = [c \in Callers |-> mode]
    /\ prev = [c \in Callers |-> mode]
    /\ tree = "idle"
    /\ faultLost = FALSE
    /\ faultSeen = [c \in Callers |-> FALSE]
    /\ autoWhileLatched = FALSE

Begin(c, t) ==
    /\ pc[c] = "idle"
    /\ pc' = [pc EXCEPT ![c] = "before"]
    /\ from' = [from EXCEPT ![c] = mode]
    /\ target' = [target EXCEPT ![c] = t]
    /\ faultSeen' = [faultSeen EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, prev, tree, faultLost, autoWhileLatched>>

\* Refused at the request while a FAULT is pending; one already in flight when
\* the FAULT starts is refused at its commit instead. This mirrors the code and
\* no invariant depends on it: hooks on entering AUTO do not actuate here, and
\* the hazard it guards against is theirs.
RequestOrdinary(c) ==
    /\ ~FaultPending
    /\ \E t \in Modes : <<mode, t>> \in Ordinary /\ IdleMayStart(t) /\ Begin(c, t)

\* set_mode(FAULT) in FAULT is a no-op that returns at once.
RequestFault ==
    /\ mode # "FAULT"
    /\ Begin(Fault, "FAULT")

\* In FAULT it still counts: a recovery under way when it is made sees it (#774).
RequestFaultInFault ==
    /\ mode = "FAULT"
    /\ pc[Fault] = "idle"
    /\ faultSeen' = [c \in Callers |-> faultSeen[c] \/ (pc[c] \in {"before", "commit"} /\ from[c] = "FAULT")]
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched>>

\* A before-hook is chosen by the snapshot taken at the request. Its call goes
\* through the router too, and meets the IDLE gate if another exit from AUTO
\* has since run and IDLE was entered.
RunBefore(c) ==
    /\ pc[c] = "before"
    /\ pc' = [pc EXCEPT ![c] = "commit"]
    /\ output' = IF HookAt = "before" /\ from[c] = "AUTO" /\ target[c] # "AUTO" /\ mode # "IDLE"
                 THEN "safe" ELSE output
    /\ UNCHANGED <<mode, latched, laddered, hookless, from, target, prev, tree, faultLost, autoWhileLatched, faultSeen>>

CommitOrdinary(c) ==
    /\ pc[c] = "commit"
    /\ IF OrdinaryMayCommit(c)
       THEN /\ mode' = target[c]
            /\ prev' = [prev EXCEPT ![c] = mode]
            /\ pc' = [pc EXCEPT ![c] = "after"]
            /\ autoWhileLatched' = (autoWhileLatched \/ (target[c] = "AUTO" /\ latched))
            /\ faultLost' = (faultLost \/ faultSeen[c])
       ELSE /\ pc' = [pc EXCEPT ![c] = "idle"]
            /\ UNCHANGED <<mode, prev, autoWhileLatched, faultLost>>
    /\ UNCHANGED <<output, latched, laddered, hookless, from, target, tree, faultSeen>>

CommitFault ==
    /\ pc[Fault] = "commit"
    /\ IF FaultMayCommit(Fault)
       THEN /\ mode' = "FAULT"
            /\ prev' = [prev EXCEPT ![Fault] = mode]
            /\ pc' = [pc EXCEPT ![Fault] = "after"]
            /\ UNCHANGED faultLost
       ELSE /\ pc' = [pc EXCEPT ![Fault] = "idle"]
            /\ faultLost' = TRUE
            /\ UNCHANGED <<mode, prev>>
    /\ UNCHANGED <<output, latched, laddered, hookless, from, target, tree, autoWhileLatched, faultSeen>>

\* An after-hook is chosen by the mode actually committed from. Its call goes
\* through the router, whose IDLE gate refuses it (#776).
RunAfter(c) ==
    /\ pc[c] = "after"
    /\ pc' = [pc EXCEPT ![c] = "idle"]
    /\ output' = IF HookAt = "after" /\ prev[c] = "AUTO" /\ target[c] # "AUTO" /\ mode # "IDLE"
                 THEN "safe" ELSE output
    /\ UNCHANGED <<mode, latched, laddered, hookless, from, target, prev, tree, faultLost, autoWhileLatched, faultSeen>>

\* The tick reads the mode once, then runs its nodes with no further check.
TickStart ==
    /\ tree = "idle"
    /\ mode = "AUTO"
    /\ tree' = "node"
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, pc, from, target, prev, faultLost, autoWhileLatched, faultSeen>>

\* A node has entered the router and now waits for the provider lock.
NodeWaits ==
    /\ tree = "node"
    /\ tree' = "wait"
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, pc, from, target, prev, faultLost, autoWhileLatched, faultSeen>>

\* The provider lock serializes the check, the send and every hook call, so
\* one step can stand for them: a hook's own call is ordered after this one.
\* The tick then has another node to run, or it ends.
Dispatch ==
    /\ tree = "wait"
    /\ output' = IF TreeMayDispatch THEN "driven" ELSE output
    /\ tree' \in {"node", "idle"}
    /\ UNCHANGED <<mode, latched, laddered, hookless, pc, from, target, prev, faultLost, autoWhileLatched, faultSeen>>

\* A restart that leaves AUTO uncovered. The branch that does this during AUTO
\* faults the runtime and is not modelled.
RestartDropsHook ==
    /\ ~hookless
    /\ mode # "AUTO"
    /\ hookless' = TRUE
    /\ UNCHANGED <<mode, output, latched, laddered, pc, from, target, prev, tree, faultLost, autoWhileLatched, faultSeen>>

EngageStop ==
    /\ ~latched
    /\ latched' = TRUE
    /\ UNCHANGED <<mode, output, laddered, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched, faultSeen>>

RunLadder ==
    /\ latched /\ ~laddered
    /\ laddered' = TRUE
    /\ output' = "safe"
    /\ UNCHANGED <<mode, latched, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched, faultSeen>>

ClearStop ==
    /\ latched /\ laddered
    /\ latched' = FALSE
    /\ laddered' = FALSE
    /\ UNCHANGED <<mode, output, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched, faultSeen>>

Next ==
    \/ \E c \in Ops : RequestOrdinary(c) \/ CommitOrdinary(c)
    \/ RequestFault \/ RequestFaultInFault
    \/ CommitFault
    \/ \E c \in Callers : RunBefore(c) \/ RunAfter(c)
    \/ TickStart \/ NodeWaits \/ Dispatch
    \/ RestartDropsHook
    \/ EngageStop \/ RunLadder \/ ClearStop

-----------------------------------------------------------------------------
TypeOK ==
    /\ mode \in Modes
    /\ output \in {"safe", "driven"}
    /\ tree \in {"idle", "node", "wait"}

Settled == tree = "idle" /\ \A c \in Callers : pc[c] = "idle"

\* Once everything has settled outside AUTO, the leave-AUTO hook has the last
\* word: no tree command landed after it (#762, #763).
SafeWhenSettledOutsideAuto == (Settled /\ mode # "AUTO") => output = "safe"

\* A FAULT request is never lost: one that reaches its commit is not refused
\* (#528), and one made in FAULT is not overtaken by a recovery (#774).
FaultAlwaysCommits == ~faultLost

\* AUTO is never entered without a hook covering it (#239).
NoAutoWhenHookless == ~(mode = "AUTO" /\ hookless)

\* AUTO is never entered while the stop latch holds.
NoAutoEntryWhileLatched == ~autoWhileLatched

\* After the stop's ladder has run, nothing drives the output until it clears.
SafeWhileStopHolds == (latched /\ laddered) => output = "safe"

-----------------------------------------------------------------------------
(* Reachability witnesses: each must be violated, or the model is vacuous. *)
NeverDriven == output # "driven"
NeverFaultFromAuto == ~(mode = "FAULT" /\ prev[Fault] = "AUTO" /\ pc[Fault] = "after")
NeverStopHolds == ~(latched /\ laddered)
NeverHookless == ~hookless

=============================================================================
