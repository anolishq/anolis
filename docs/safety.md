# Safety Operations Guide

This guide defines safe operating procedures for Anolis-managed hardware systems.

## Core Principles

1. Safe startup: runtime always starts in `IDLE`.
2. Explicit control: mode transitions require operator action (`POST /v0/mode`).
3. Controlled automation: in `AUTO`, manual calls are policy-gated (`BLOCK` or `OVERRIDE`).
4. Visibility first: read-only diagnostics remain available across modes.
5. Hardware safety remains external: software controls do not replace interlocks
   or emergency-stop systems. The runtime can only command devices that remain
   powered — see Stop categories below.

## Stop categories — what software can and cannot be

Read this before any other section. Most confusion about this system's safety
behaviour comes from using one word, "e-stop", for three unrelated mechanisms.

IEC 60204-1:2005 §9.2.2 defines three stop categories. ISO 13850:2015 §4.1.3
requires an emergency stop to be Category 0 or 1 — never Category 2 — with the
choice between them determined by risk assessment:

| Category | Behaviour | Power to actuators |
| -------- | --------- | ------------------ |
| **0** | immediate power removal, uncontrolled stop | removed immediately |
| **1** | controlled stop, then power removal | available during the stop, then removed |
| **2** | controlled stop, power maintained | never removed |

**A bus command to a powered device is, at best, a Category 2 stop:** power is
never removed from the actuators. In practice it is weaker than that. It has no
determined performance level (ISO 13849-1) and no standstill monitoring, it is
not necessarily a *controlled* stop in the standard's sense — `set_open_loop{0,0}`
on a motor initiates a coast, not a controlled deceleration — and it does not
execute at all if the runtime, a provider, or the bus has failed.

Treat it as an operational stop with a latch, not as a rated safety function.
That is the whole of what this runtime can do on its own, and it means:

> `POST /v0/estop` is a **software stop**, not an emergency stop. It cannot
> remove power, and no amount of runtime work makes it compliant as one. The
> machine's emergency stop is external hardware, and it always is.

That is still a legitimate and useful thing — it is the right response to an
automation fault, a guard opening, or an operator deciding to halt a process that
is going wrong. It is just not the emergency stop, and it must never be presented
as one.

### The three mechanisms, kept apart

Stop categories classify a machine's stop **function as a whole**. The column
below is what each mechanism does to actuator power — an approximation for
orientation, not a compliance claim about any one layer.

| Mechanism | Effect on actuator power | Authority | Observable after? |
| --------- | ------------------------ | --------- | ----------------- |
| Button + contactor cutting device power | removed at the board | hardware | no — devices are gone |
| Hardware stop input to a device, device self-safes | removed at the output stage | firmware | yes |
| Firmware command watchdog (bus goes quiet) | removed at the output stage | firmware | yes |
| `POST /v0/estop` / `safety.safe_state` ladder | **maintained** — commands only | provider + runtime | yes |

Authority matters: every layer above can fail, so a stop is only as reliable as
the lowest layer that can enforce it alone. The runtime is the weakest layer in
this table, which is why it must never be the sole stop path on a machine that
can hurt someone or destroy a batch.

### Consequences that surprise people

- **Under Category 0 there is nothing to read back.** Every device disappears at
  once. That is the *success* signal, not a fault — but the runtime does not yet
  know that, and reads it as a fleet of device failures (anolishq/anolis#284).
- **Under Category 0 a motor coasts, by construction.** Power is gone, so an
  H-bridge cannot brake. If a coast is unacceptable for the hazard, the options
  are a power-off-engaged (spring-applied) mechanical brake, a Category 1 stop
  (controlled deceleration *then* power removal — a contactor **plus** a signal
  path, not one instead of the other), or a guard interlock timed to the run-down.
  Note the mechanical brake is the only one of the three that still works when
  the control system is the thing that failed. Which to use is a risk-assessment
  question, not a software one.
- **Releasing an emergency stop must never restart the machine.** ISO 13850:2015
  §4.1.4: disengagement "shall not restart the machinery but only permit
  restarting". The second half is the actionable part — the runtime must come
  back able to be started, not started. It does not model this yet
  (anolishq/anolis#285).
- **"Confirmed safe" is not always achievable.** Any reporting that claims it
  unconditionally is asserting something the system cannot observe.

### The machine profile already names this

`machine-profile.yaml` has carried a `safety.estop_topology` field since before
this section was written, with two values that are a useful proxy for this
distinction and an accurate description of each one's software signature:

- `power_cut` — the stop cuts actuator-board power. Signature: device blackout,
  I/O failures accruing, recovery on release.
- `signal` — the stop drives the boards' ESTOP inputs. Signature: `estop=true` in
  device state, bus stays alive.

Both are first-class supported wirings, and a machine that uses `power_cut` with
its signal inputs unwired is a deliberate, correct design — not every device has
a stop input, so the total-cut path has to exist.

Note what the field does **not** carry: it describes how the stop is *wired*, not
its stop category. A `signal` topology can be Category 0, 1, or 2 depending on
what the firmware does with the input. It is most of what the runtime needs to
predict a blackout, and it is not the model #283 asks for.

Two gaps, and they are the reason this distinction kept getting re-litigated:

1. The field is declared **informational**: "nothing may branch runtime behavior
   on these fields beyond presentation/interpretation". So the runtime knows the
   answer and is forbidden from acting on it.
2. Neither reference machine profile actually sets it.

Promoting it from informational to behavioural — so the runtime can tell an
expected `power_cut` blackout from a genuine fleet fault — is the substance of
anolishq/anolis#284, and the smallest useful piece of anolishq/anolis#283.

Promotion has to define what an **absent** declaration means, which today is
every machine, and it must not let a mis-declared `power_cut` silently reinterpret
a genuine fleet fault as an expected stop. Treating device-loss as expected should
be gated on a stop having actually been commanded or observed, not on the profile
field alone.

Design work on this model is tracked in anolishq/anolis#283.

## Runtime Mode Safety Semantics

| Runtime Mode | Automation Loop | Control Operations (`POST /v0/call`) | Notes                                                         |
| ------------ | --------------- | ------------------------------------ | ------------------------------------------------------------- |
| `IDLE`       | Stopped         | Blocked                              | Safe startup/standby mode                                     |
| `MANUAL`     | Stopped         | Allowed                              | Operator-driven commissioning/control                         |
| `AUTO`       | Running         | Manual calls policy-gated            | Automation calls continue normally                            |
| `FAULT`      | Stopped         | Allowed                              | Recovery/diagnostic mode; transition restrictions still apply |

Transition rules:

1. `IDLE <-> MANUAL`
2. `MANUAL <-> AUTO`
3. `Any -> FAULT` (valid transition target)
4. `FAULT -> MANUAL`
5. `FAULT -> AUTO` is invalid
6. `AUTO -> IDLE` is invalid

Note: FAULT is not globally auto-entered for every error condition in the current runtime;
it is a defined mode and transition target with strict recovery pathing.

The runtime does enter FAULT autonomously in two cases:

- **A behaviour-tree tick raises an exception** (#279). The control logic has reached a
  state its author did not model, so autonomous actuation is halted rather than continued.
  Note this is reachable from ordinary tree edits, not only from internal errors: BT nodes
  guard their inputs but not their outputs, and a precondition script referencing an
  undefined blackboard entry throws on the first tick of a tree that loaded cleanly.
- **A provider restart republishes an inventory that fails the refuse-hookless gate**
  while in AUTO (#233).

In both cases the declared `*->FAULT` mode hooks run, and recovery is the ordinary
`FAULT -> MANUAL` path. An operator who finds a machine in FAULT should check the runtime
log and `GET /v0/automation/status` (`execution_reason: terminal_failure` indicates the
engine faulted rather than an operator changing mode).

## Standard Startup Sequence

1. Start runtime (enters `IDLE`).
2. Verify provider/device availability and safe initial states.
3. Transition to `MANUAL`.
4. Run manual verification/calibration checks.
5. Transition to `AUTO` only after operator acceptance.

Use this canonical transition command:

```bash
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}'
```

Use the same command shape for `AUTO`, `IDLE`, and `FAULT`.

## Mode Procedure Checklist

### IDLE -> MANUAL

1. Confirm providers are `AVAILABLE`.
2. Confirm actuator signals are in safe states.
3. Transition to `MANUAL`.

### MANUAL -> AUTO

1. Validate behavior tree configuration and parameters.
2. Confirm manual verification is complete.
3. Transition to `AUTO`.
4. Monitor `/v0/automation/status` and `/v0/providers/health`.

> The runtime refuses `MANUAL -> AUTO` (returns `FAILED_PRECONDITION`) when the
> config has actuating outputs but no declared `mode_transition_hooks` entry
> covers the `AUTO -> FAULT` transition — autonomous actuation (which only runs
> in AUTO) must have a declared safe-state path that actually fires when a fault
> trips from AUTO. Declare a hook matching `AUTO -> FAULT` (`from: AUTO`/`"*"`/
> omitted **and** `to: FAULT`/`"*"`/omitted) that drives actuators to a safe
> value; a hook covering only another transition (e.g. `AUTO -> MANUAL`, or an
> `IDLE -> FAULT` hook that never fires from AUTO) does not satisfy the gate.
> Manual control and the software stop remain available in the refused state.
>
> **Satisfying this gate does not give you a software stop.** The gate asks
> only for the autonomous path; `POST /v0/estop` runs `safety.safe_state`, which
> is a separate declaration the gate never checks. Declaring only the hook
> leaves the software stop driving nothing *and* — because the latch engages before
> FAULT is entered — suppresses the hook that any other route into FAULT would
> have run. The refusal message says so when the ladder would drive nothing.

### AUTO -> MANUAL

1. Transition to `MANUAL` for planned operator takeover.
2. Verify actuators and signals settle to expected state.

### MANUAL -> IDLE

1. Command actuators to safe outputs.
2. Verify safe state via `/v0/state`.
3. Transition to `IDLE`.

### Any -> FAULT, then FAULT -> MANUAL

1. Enter `FAULT` when explicit recovery isolation is needed.
2. Diagnose and resolve root cause.
3. Transition `FAULT -> MANUAL`.
4. Re-verify before returning to `AUTO`.

## Emergency Response

**If there is a physical hazard — a person at risk, fire, a machine damaging
itself — hit the hardware emergency stop. First, not third.** It is the only
control that removes power, and it is the only one that works when the software
is the thing that has failed.

1. **Physical hazard → hardware emergency stop / power isolation.** This is the
   machine's emergency stop — Category 0 or 1 depending on how it is wired (see
   Stop categories above). No software involvement; works regardless of runtime
   or provider state.
2. **Process or control problem, no physical hazard →** `POST /v0/estop`. A
   software stop: it runs the declared safe-state ladder and
   refuses further actuating calls until `POST /v0/estop/clear`. It works whether
   or not automation is enabled; on an automation machine it also drives `FAULT`.
   It requires the runtime, the providers, and the bus to be working.
3. **Runtime misbehaving and unresponsive →** terminate the runtime process. On
   machines with a firmware command watchdog this also causes devices to self-safe
   once bus traffic stops; on machines without one it leaves outputs latched, so
   prefer step 1.
4. **After any incident:** inspect hardware, collect logs, and restart from the
   full startup checklist.

**If you are not certain whether step 1 or step 2 applies, use step 1.**

Per principle 5, the software stop does not replace the hardware emergency stop
or interlocks. A machine that declares no software safe-state
(`safety.safe_state`) reports `software_safe_state: "none"` and still latches,
but performs no safe-state actuation at all.

## Common Risks and Mitigations

1. Skipping IDLE checks:
   - Always validate safe initial state before `MANUAL`.
2. Unsafe power-on assumptions:
   - Providers must actively drive safe defaults on startup.
3. Invalid FAULT recovery path:
   - Never attempt `FAULT -> AUTO`; recover through `MANUAL`.
4. Manual interference in AUTO:
   - Prefer `manual_gating_policy: BLOCK` for production.
5. Ignored provider health degradation:
   - Monitor `/v0/providers/health` and supervision fields continuously.

## Hardware Integration Safety Checklist

### Provider safety

1. Provider startup sets actuators to safe defaults.
2. Communication failures surface as degraded quality/error status.
3. Restart behavior does not create unsafe transient outputs.

### Capability and limits

1. Function argument constraints match physical limits.
2. Signals represent actionable safety state.
3. Device fault conditions are exposed clearly.

### Operational readiness

1. Startup/recovery SOP exists and is tested.
2. Emergency stop SOP exists and is tested.
3. Site-specific hazard analysis is complete.

## Development vs Production Policy

Development/testing:

1. Runtime still starts in `IDLE`.
2. Faster workflows are acceptable after minimal safety verification.
3. `OVERRIDE` may be acceptable for controlled bench testing.

Production/hardware operation:

1. Keep explicit IDLE verification and full startup checklist.
2. Use `manual_gating_policy: BLOCK` unless a written override policy exists.
3. Require documented recovery steps for FAULT entry and exit.

## Related Documentation

1. [Provider Safe Initialization Contract](providers.md#safe-initialization-contract)
2. [Automation Layer](automation.md)
3. [Runtime Configuration](configuration.md)
4. [HTTP API Reference](http-api.md)
5. [anolis-provider-sim fault injection reference](https://github.com/anolishq/anolis-provider-sim/blob/main/README.md#fault-injection-api)
6. [anolis-provider-sim safe initialization reference](https://github.com/anolishq/anolis-provider-sim/blob/main/README.md#safe-initialization-in-provider-sim)
