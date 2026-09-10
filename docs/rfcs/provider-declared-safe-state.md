# RFC: provider-declared safe state

**Status:** accepted with revisions, not implemented.
**Supersedes:** nothing. **Depends on:** the stop-category model in
[safety.md](../safety.md); tracked as anolishq/anolis#283.

## The problem

Safety is currently expressed as **raw device function calls in YAML**:

```yaml
safety:
  safe_state:
    hooks:
      - device_handle: bread0/dcmt0
        function_name: set_brake
        args: { motor1_brake: true, motor2_brake: true }
```

There is no concept of "stop" anywhere in the system — only a list of bus calls an
operator hand-wrote. Everything below follows from that.

**It produced several competing stop paths.** `safety.safe_state` fires only on
`POST /v0/estop`. A `*->FAULT` mode hook fires on an autonomous fault, and is the
only one the AUTO gate checks. `AUTO->MANUAL` and `MANUAL->IDLE` hooks each carry
their own stop-shaped call lists. The device firmware may self-safe on a command
watchdog. These disagree, and the disagreements are the open issues — six
symptoms of one defect (#251, #253, #255, #256, #246, #261). They have been fixed
individually and keep reopening because the shape is wrong, not the instances.

**Each declaration is written in a vocabulary that cannot express what is
needed.** Sequencing is the operator's problem. Verification is impossible —
a call is reported successful once the frame is written. And types are limited to
what YAML and the runtime's parser agree on, which is why #252 blocked a heater
from having any expressible safe state until argument coercion shipped in v0.1.41.

## The proposal

**The device declares its own safe state; the runtime has one authority that
invokes it; every stop trigger routes through that authority.**

Add an ADPP request:

```proto
message EnterSafeStateRequest {
  string device_id = 1;   // empty = every device this provider owns
  string reason    = 2;
}
```

with a per-device outcome in the response, and a per-device capability
declaration so the runtime knows which devices support it.

### Why imperative rather than declarative

The alternative — a device *declaring* a list of calls the runtime then executes —
keeps the present architecture and moves the YAML into a protobuf. The RPC is
better because the provider can do what a call list structurally cannot:

- **Sequence correctly** without an operator encoding driver semantics.
- **Read back and confirm.** Today a call reports success on bus acknowledgement alone; a provider can re-read and distinguish *applied* from *verified*.
- **Retry and escalate** — the missing backstop in #256.
- **Use types the config layer cannot express.**

## Required revisions before implementation

These came out of adversarial review and are not optional; each is the class of
omission that produced the issues above.

1. **State the latch and ordering contract.** The authority must engage the actuation latch *before* any fan-out and contend on the same per-provider lock the call router takes, or a queued call can land after a confirmed safe state. And `enter_safe_state_on` must say, per trigger, whether it latches, who may clear it, and who may resume.
2. **Do not claim this fixes provider crash.** A crashed provider cannot be sent anything. The trigger services *surviving* providers, which is real and new — nothing happens today — but the crashed provider's devices remain firmware-watchdog-only.
3. **The safe pole is machine policy, not device policy.** Two devices of the same type, same firmware, same provider, can need opposite safe states depending on what they drive. A provider can declare what it is *able* to do; it cannot know what is safe for a machine it knows nothing about.
4. **Confirmation must degrade honestly.** Name the outcome for what is verified — a read-back match — never for mechanical effect. Add an explicit unreachable state. Under a Category 0 stop there is nothing to read back at all.
5. **Coverage stays per-function.** A per-device boolean goes stale silently when a provider publishes a new actuating function, which is exactly what #253 asks the predicate to catch.
6. **Deleting the config-declared ladder is a demotion, not a removal.** Keep it as a documented fallback for devices that will never implement the RPC, or machine safety becomes a function of the least-conformant provider binary on the bus.
7. **Bound it.** Carry a deadline; give the call its own timeout; a safe-state timeout must not mark the session unhealthy or trigger a restart.
8. **Build the test seam first.** The provider mock currently answers control writes unconditionally, so a faithful read-back fails in every unit test. Without fixing that, the read-back path — the entire justification for the RPC — is first exercised on live hardware.

## What it does not solve

**This is a Category 2 mechanism.** It commands devices that remain powered, so it
is a *protective* stop and cannot be an emergency stop — see
[safety.md](../safety.md). On a machine whose emergency stop removes power, none
of this participates: the devices are unpowered before any of it could run.

That bounds the value precisely. This makes the software stop trustworthy and
verifiable. It does not make it the machine's emergency stop, and no amount of
further work on it would.

Two adjacent gaps are **not** addressed here and are tracked separately: the
runtime cannot distinguish expected device-loss from a fleet fault (#284), and it
has no reset-must-not-restart rule (#285).

## Staging

1. **Protocol, SDK and one provider**, with the authority wired to `POST /v0/estop` only and the existing ladder retained as fallback.
2. **Route the remaining triggers**, with the corrected scope from revision 2.
3. **Demote the ladder** to documented fallback.

Prerequisites to stage 1: the test seam (revision 8), the latch contract
(revision 1), and conformance clauses alongside the protocol change.
