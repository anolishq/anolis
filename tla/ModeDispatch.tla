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
    autoWhileLatched  \* history: AUTO was committed while the stop latch held

vars == <<mode, output, latched, laddered, hookless, pc, from, target, prev,
          tree, faultLost, autoWhileLatched>>

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

OrdinaryMayCommit(c) == mode = from[c] /\ ~FaultPending /\ AutoGate(c)
\* #763: an ordinary transition could commit during a pending FAULT.
OrdinaryIgnoresFault(c) == mode = from[c] /\ AutoGate(c)

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
    /\ autoWhileLatched = FALSE

Begin(c, t) ==
    /\ pc[c] = "idle"
    /\ pc' = [pc EXCEPT ![c] = "before"]
    /\ from' = [from EXCEPT ![c] = mode]
    /\ target' = [target EXCEPT ![c] = t]
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, prev, tree, faultLost, autoWhileLatched>>

\* Refused at the request while a FAULT is pending; one already in flight when
\* the FAULT starts is refused at its commit instead.
RequestOrdinary(c) ==
    /\ ~FaultPending
    /\ \E t \in Modes : <<mode, t>> \in Ordinary /\ Begin(c, t)

\* set_mode(FAULT) in FAULT is a no-op that returns at once.
RequestFault ==
    /\ mode # "FAULT"
    /\ Begin(Fault, "FAULT")

\* A before-hook is chosen by the snapshot taken at the request.
RunBefore(c) ==
    /\ pc[c] = "before"
    /\ pc' = [pc EXCEPT ![c] = "commit"]
    /\ output' = IF HookAt = "before" /\ from[c] = "AUTO" /\ target[c] # "AUTO"
                 THEN "safe" ELSE output
    /\ UNCHANGED <<mode, latched, laddered, hookless, from, target, prev, tree, faultLost, autoWhileLatched>>

CommitOrdinary(c) ==
    /\ pc[c] = "commit"
    /\ IF OrdinaryMayCommit(c)
       THEN /\ mode' = target[c]
            /\ prev' = [prev EXCEPT ![c] = mode]
            /\ pc' = [pc EXCEPT ![c] = "after"]
            /\ autoWhileLatched' = (autoWhileLatched \/ (target[c] = "AUTO" /\ latched))
       ELSE /\ pc' = [pc EXCEPT ![c] = "idle"]
            /\ UNCHANGED <<mode, prev, autoWhileLatched>>
    /\ UNCHANGED <<output, latched, laddered, hookless, from, target, tree, faultLost>>

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
    /\ UNCHANGED <<output, latched, laddered, hookless, from, target, tree, autoWhileLatched>>

\* An after-hook is chosen by the mode actually committed from.
RunAfter(c) ==
    /\ pc[c] = "after"
    /\ pc' = [pc EXCEPT ![c] = "idle"]
    /\ output' = IF HookAt = "after" /\ prev[c] = "AUTO" /\ target[c] # "AUTO"
                 THEN "safe" ELSE output
    /\ UNCHANGED <<mode, latched, laddered, hookless, from, target, prev, tree, faultLost, autoWhileLatched>>

\* The tick reads the mode once, then runs its nodes with no further check.
TickStart ==
    /\ tree = "idle"
    /\ mode = "AUTO"
    /\ tree' = "node"
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, pc, from, target, prev, faultLost, autoWhileLatched>>

\* A node has entered the router and now waits for the provider lock.
NodeWaits ==
    /\ tree = "node"
    /\ tree' = "wait"
    /\ UNCHANGED <<mode, output, latched, laddered, hookless, pc, from, target, prev, faultLost, autoWhileLatched>>

\* The provider lock serializes the check, the send and every hook call, so
\* one step can stand for them: a hook's own call is ordered after this one.
\* The tick then has another node to run, or it ends.
Dispatch ==
    /\ tree = "wait"
    /\ output' = IF TreeMayDispatch THEN "driven" ELSE output
    /\ tree' \in {"node", "idle"}
    /\ UNCHANGED <<mode, latched, laddered, hookless, pc, from, target, prev, faultLost, autoWhileLatched>>

\* A restart that leaves AUTO uncovered. The branch that does this during AUTO
\* faults the runtime and is not modelled.
RestartDropsHook ==
    /\ ~hookless
    /\ mode # "AUTO"
    /\ hookless' = TRUE
    /\ UNCHANGED <<mode, output, latched, laddered, pc, from, target, prev, tree, faultLost, autoWhileLatched>>

EngageStop ==
    /\ ~latched
    /\ latched' = TRUE
    /\ UNCHANGED <<mode, output, laddered, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched>>

RunLadder ==
    /\ latched /\ ~laddered
    /\ laddered' = TRUE
    /\ output' = "safe"
    /\ UNCHANGED <<mode, latched, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched>>

ClearStop ==
    /\ latched /\ laddered
    /\ latched' = FALSE
    /\ laddered' = FALSE
    /\ UNCHANGED <<mode, output, hookless, pc, from, target, prev, tree, faultLost, autoWhileLatched>>

Next ==
    \/ \E c \in Ops : RequestOrdinary(c) \/ CommitOrdinary(c)
    \/ RequestFault
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

\* A FAULT request always ends in FAULT (#528).
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
