# RFC: provider-declared safe state

**Status:** accepted with revisions; not implemented. ADPP has no safe-state
request, and the runtime's safe state is still the ladder declared in
`safety.safe_state`. The zero rung, and the install's check that a safe state
can be planned, belong to that ladder, not to this proposal. Depends on the stop
categories in [safety.md](../safety.md#stop-categories-what-software-can-and-cannot-be);
tracked under anolishq/anolis#283.

## The problem

A safe state is a list of device calls written in YAML:

<!-- validate: runtime-config fragment -->

```yaml
safety:
  safe_state:
    hooks:
      - device_handle: bread0/dcmt0
        function_name: set_brake
        args: { motor1_brake: true, motor2_brake: true }
```

The runtime has an actuation latch, a safe-state ladder (hooks, else setpoints
that cover every actuating output, else zero when `zero_is_safe`), and
`safety.stop_chain`, which observes the emergency stop. Apart from the zero
rung, whose calls the runtime builds itself, the safe state is still calls an
operator wrote by hand. The rest of this section follows from that.

There are several stop paths, each with its own calls:

- `safety.safe_state` runs on `POST /v0/estop`, at startup unless
  `safety.on_startup: leave`, and for a device that answers again after it was
  lost.
- A mode-transition hook on `AUTO -> FAULT` runs on an autonomous fault. It is
  the only declaration the gate into AUTO checks.
- Other mode-transition hooks, such as `AUTO -> MANUAL`, carry their own call
  lists.
- Device firmware may stop on its own command watchdog.

These paths disagree. Six issues came from that: #246, #251, #253, #255, #256
and #261. All are closed, each fixed on its own, and the shape that produces
them is unchanged.

A call list cannot say what a safe state needs:

- Sequencing is left to the operator.
- Nothing confirms the result. A bread call succeeds when the device
  acknowledges the command (CRUMBS 1600 or later) or, on older firmware, when
  the frame is written. An EZO call succeeds when the frame is written. No call
  reads the resulting state back.
- Argument types are limited to what YAML and the runtime's parser accept.
  Until #252 was fixed, a uint64 actuator had no safe state that could be
  written.

## The proposal

The device declares its own safe state. The runtime has one authority that
invokes it, and every stop trigger goes through that authority.

Add an ADPP request:

```proto
message EnterSafeStateRequest {
  string device_id = 1;   // empty = every device this provider owns
  string reason    = 2;
}
```

The response carries an outcome per device. Each device declares whether it
supports the request, so the runtime knows where it can be used.

### Why a request rather than a declared call list

A device could instead declare a list of calls for the runtime to run. That
keeps the current design and moves the YAML into protobuf. A request lets the
provider do what a call list cannot:

- Sequence its own calls, so the operator does not encode driver behaviour.
- Read the state back, and report *applied* separately from *verified*.
- Retry and escalate when a step fails (#256).
- Use argument types the config cannot express.

## Required revisions before implementation

Each of these is the kind of gap that produced the issues above.

1. **Latch and ordering.** The authority must engage the actuation latch
   before it sends anything, and take the same per-provider lock the call
   router uses. Otherwise a queued call can land after the safe state is
   confirmed. For each trigger, the config must say whether it latches, who
   may clear the latch, and who may resume.
2. **Provider crash is not fixed by this.** A crashed provider cannot receive
   a request. A crash can trigger the safe state on the providers that are
   still running, which the runtime does not do today. The crashed provider's
   devices still rely on their firmware watchdog alone.
3. **The safe state belongs to the machine, not the device.** Two devices of
   the same type, firmware and provider can need opposite safe states,
   depending on what they drive. A provider can declare what it is able to
   do. It cannot know what is safe for the machine.
4. **Report only what was checked.** Name the outcome for what was verified,
   a read-back that matches, never for mechanical effect. Add an explicit
   unreachable outcome. Under a Category 0 stop there is nothing to read back.
5. **Coverage stays per function.** The runtime measures safe-state coverage
   per actuating function. A per-device flag would go stale without notice
   when a provider adds an actuating function.
6. **Keep the config ladder as a fallback.** Devices that never implement the
   request still need a safe state. Without the fallback, machine safety
   depends on the least conformant provider on the bus.
7. **Bound it.** The request carries a deadline and has its own timeout. A
   safe-state timeout must not mark the provider session unhealthy or restart
   it.
8. **Build the test seam first.** The unit tests' mock provider returns
   scripted results and holds no device state, so a read-back there returns
   what the test scripted, not the effect of the call. Without a seam, the
   read-back path, which is the reason for the request, would first run on
   hardware.

## What it does not solve

This is a Category 2 mechanism. It commands devices that are still powered,
so it is a protective stop and cannot be an emergency stop; see
[safety.md](../safety.md#stop-categories-what-software-can-and-cannot-be). On
a machine whose emergency stop removes power, the devices are off before any
of this could run.

It can make the software stop verifiable. It does not make it the machine's
emergency stop.

Two related gaps are handled elsewhere. `safety.stop_chain` tells an expected
device loss from a fleet fault (#284; see [stop-chain.md](stop-chain.md)). A
device that is lost and returns is latched against automation until an
operator re-enters AUTO (#285).

## Staging

1. Protocol, SDK and one provider. The authority handles `POST /v0/estop`
   only, and the config ladder stays as the fallback.
2. Route the other triggers, within the scope set by revision 2.
3. Make the config ladder a documented fallback.

Before stage 1: the test seam (revision 8), the latch contract (revision 1),
and conformance clauses that ship with the protocol change.
