# Safety Operations Guide

This guide defines safe operating procedures for Anolis-managed hardware systems.

## Core Principles

1. Startup: with `automation.enabled` the runtime starts in `IDLE`, which blocks
   new control calls and does not by itself change any output; without
   automation there is no mode and manual calls are not mode-gated (the
   software-stop latch still refuses actuating calls). What happens to
   outputs depends on `safety.on_startup` (see Outputs at startup below): by
   default the runtime runs `safety.safe_state` once before it accepts requests.
2. Explicit control: on automation machines an operator changes mode with
   `POST /v0/mode`, and `POST /v0/estop` also enters `FAULT`; the runtime enters
   `FAULT` on its own only in the cases listed under Runtime Mode Safety
   Semantics.
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

At startup the runtime warns about each device that has ACTUATE functions
and whose provider reports the SDK health metric `watchdog_armed` as
`false`. A provider reports that key only when the device has a command
watchdog that is configured and was read live; a device without one, a
watchdog that was never configured, and a failed read all leave the key
out, and the runtime says nothing about them. No warning is therefore not
evidence that a watchdog is armed.

Authority matters: every layer above can fail, so a stop is only as reliable as
the lowest layer that can enforce it alone. The runtime is the weakest layer in
this table, which is why it must never be the sole stop path on a machine that
can hurt someone or destroy a batch.

### Consequences that surprise people

- **Under Category 0 there is nothing to read back.** Every device disappears at
  once. That is the *success* signal, not a fault. With `safety.stop_chain`
  configured, a device in its scope that goes unreachable while the stop is in
  force is reported as the stop (`stop_chain.expected_losses` in
  `GET /v0/runtime/status`); without it, the runtime reads the blackout as a
  fleet of device failures.
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
  back able to be started, not started. Today the runtime does this per device
  (anolishq/anolis#285, #299). A device that went unreachable stays
  latched, and the behaviour tree may not actuate it until an operator re-enters
  AUTO. When it answers again, the active safe-state rung is re-run for it (under
  rung `none` that does nothing), and
  `GET /v0/runtime/status` lists it under `device_loss_latched`. The latch does
  not hold manual calls or mode-transition hooks, and the runtime tells a stop
  from a fault only for a stop it observes (see the stop chain below).
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
2. It describes the whole machine with one value, but real machines are mixed,
   and the two values mix up *what* a stop de-energises (a board's actuators, or
   the whole board) with *how* (a contactor, or a board's stop input).

Device loss can be treated as an expected stop only when the stop is *observed*,
never on a declaration alone: a dead supply or an unplugged backplane looks the
same as a power-cut stop. The model in
[rfcs/stop-chain.md](rfcs/stop-chain.md) (anolishq/anolis#284) is the runtime
config `safety.stop_chain` (v0.1.53): an observed stop status and a per-device
scope. The machine profile's `estop_topology` field remains alongside it. With
`safety.stop_chain` configured, a stop status that reads `in_force` or
`unknown` holds the software-stop latch, and a device in its scope that goes
unreachable while the stop is in force is reported as the stop. Without it,
device loss is handled as described under Runtime Mode Safety Semantics,
whatever caused it.

Design work on this model is tracked in anolishq/anolis#283.

## Runtime Mode Safety Semantics

| Runtime Mode | Automation Loop | Control Operations (`POST /v0/call`) | Notes                                                         |
| ------------ | --------------- | ------------------------------------ | ------------------------------------------------------------- |
| `IDLE`       | Stopped         | Blocked                              | Startup/standby mode; outputs are not changed by the mode     |
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

The runtime does enter FAULT autonomously in three cases:

- **A behaviour-tree tick raises an exception** (#279). The control logic has reached a
  state its author did not model, so autonomous actuation is halted rather than continued.
  Note this is reachable from ordinary tree edits, not only from internal errors: BT nodes
  guard their inputs but not their outputs, and a precondition script referencing an
  undefined blackboard entry throws on the first tick of a tree that loaded cleanly.
- **A provider restart republishes an inventory that fails the refuse-hookless gate**
  while in AUTO (#233).
- **An observed stop chain changes to `in_force` or `unknown`** after startup, on
  an automation machine with HTTP enabled, while the software-stop latch is not
  already held (`safety.stop_chain`, #483). The latch is held without running the
  ladder. At startup the latch is held and the mode stays `IDLE`.

In each case the declared `*->FAULT` mode hooks run (in the third, the held
latch refuses their actuating calls), and recovery is the ordinary
`FAULT -> MANUAL` path. An operator who finds a machine in FAULT should check the runtime
log and `GET /v0/automation/status` (`execution_reason: terminal_failure` indicates the
engine faulted rather than an operator changing mode).

## Outputs at startup

`IDLE` refuses new control calls; it does not stop an output that was already
on. A restarted runtime finds the devices as the last runtime left them, so a
heater that was on in open loop is still on. `safety.on_startup` decides what
the runtime does about that:

| `safety.on_startup` | At startup |
| ------------------- | ---------- |
| `safe_state` (default) | Runs `safety.safe_state` once, after provider discovery and before the HTTP server opens. Same ladder and verdict as `POST /v0/estop`, without engaging the e-stop latch or changing mode. The log records the rung, the number of calls and `safe_state_achieved`. |
| `leave` | Leaves outputs as they were and logs that it did. |

With `safe_state` and no software safe state declared (`software_safe_state:
"none"`), there is nothing to run and the log says so. A run whose verdict is
not achieved is logged at ERROR, with one ERROR line per call that failed,
unless every call was skipped for an absent device (below); the runtime still
starts and does not latch. `GET /v0/runtime/status` reports the
outcome in `startup_safe_state` (`on_startup`, `ran`, `software_safe_state`,
`safe_state_achieved`, `failed_actions`, `skipped_actions`); `ran` is `false`
for `leave` and for no declared safe state. Each failed action carries a
`cause` (#616). `config` means the call could not be planned from the config on
these devices: the zero rung could not build its arguments, or the call was
refused before it reached its provider because its device or function does not
exist or an argument is invalid. It fails the same way on every run, and on
every `POST /v0/estop`, until `safety.safe_state` is fixed. `run` is anything
else, such as a provider that failed the call or a board that came up tripped. Set `leave` only on a machine
where a restart must not interrupt a running process, such as a culture that
must keep its heater on.

A device can be absent at startup, for instance on a bus that is powered off.
A provider that answers and lists no devices is up with none registered,
and logs a WARN; the runtime starts. A call to a device its provider reports
`UNREACHABLE`, and that is not registered, is skipped rather than failed: it
is listed in `skipped_actions` with the provider's reason and logged at WARN,
and preflight warns `absent at startup` for it. Any other call that cannot be
resolved still fails, at ERROR. `safe_state_achieved` is `true` only when at
least one call ran and every call that ran succeeded, so a boot with every
device absent reports `false` with no failed actions, logged at WARN. A
skipped device is not driven when it appears later.

`safety.on_startup` acts at runtime startup only. A supervised provider
restart (a provider crash, then the runtime starting it again) runs nothing:
the runtime does not run the safe state, and provider loss is not reported as
device loss, so the device-loss latch does not engage and nothing is
re-asserted when the devices come back. Outputs stay as the devices and the
restarted provider leave them. anolis-provider-bread from 0.10.0 drives its
outputs safe each time it starts (anolishq/anolis-provider-bread#195): DCMTs to
open loop at PWM 0, RLHTs to setpoints 0, open loop and duty 0. So a supervised
restart of bread turns those outputs off, unless its config sets
`provider.outputs_on_startup: leave`; set that with `safety.on_startup: leave`
on a machine where a restart must not interrupt a running process.

## Standard Startup Sequence

With `automation.enabled: true`:

1. Start runtime. It enters `IDLE`, and with the default `safety.on_startup` it
   runs the safe state before the HTTP server opens.
2. Verify provider/device availability and safe initial states.
3. Transition to `MANUAL`.
4. Run manual verification/calibration checks.
5. Transition to `AUTO` only after operator acceptance.

With automation disabled, the default, there is no mode to start in: `GET
/v0/mode` and `POST /v0/mode` answer `503`, and a manual `POST /v0/call` is
dispatched from the first request the HTTP server accepts. Nothing holds the
machine idle for you:

1. Start runtime. With the default `safety.on_startup` it runs the safe state
   before the HTTP server opens; `startup_safe_state.safe_state_achieved` in
   `GET /v0/runtime/status` says whether it took.
2. If no actuating call may run until an operator is ready, engage the software
   stop with `POST /v0/estop`. Its latch refuses actuating calls until
   `POST /v0/estop/clear`.
3. Verify provider/device availability and safe initial states before anyone
   sends a call.
4. Run manual verification/calibration checks, clearing the stop first if you
   engaged it.

On an automation machine, use this canonical transition command:

```bash
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}'
```

Use the same command shape for `AUTO`, `IDLE`, and `FAULT`. With auth on, as
`install.sh` sets it for a config that binds `127.0.0.1`, add the token
header; see [install-sh.md](deploy/install-sh.md#after-installing).

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
> leaves the software stop's ladder driving nothing. With no rung to run, the
> stop's own entry into FAULT runs the `-> FAULT` hooks that match the mode it
> was pressed in — in IDLE only the `after_transition` ones, because the IDLE
> gate refuses `before_transition` calls — and reports
> `safe_state_achieved: false`: the runtime cannot
> tell whether those hooks make the machine safe. When the ladder does run a
> rung (hooks, setpoints or zero), the latch refuses those hooks and the ladder
> alone drives the outputs. The refusal message says so when the ladder would
> drive nothing.

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
but its ladder performs no safe-state actuation at all. On an automation machine
the stop's entry into FAULT still runs the matching `-> FAULT` mode-transition
hooks; the stop does not count them as a safe state.

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

1. A runtime with automation enabled still starts in `IDLE`.
2. Faster workflows are acceptable after minimal safety verification.
3. `OVERRIDE` may be acceptable for controlled bench testing.

Production/hardware operation:

1. Keep the full startup checklist, with explicit IDLE verification on
   automation machines. Without automation, nothing refuses a manual call at
   startup: engage the software stop if the machine must not move until an
   operator is ready.
2. Use `manual_gating_policy: BLOCK` unless a written override policy exists.
3. Require documented recovery steps for FAULT entry and exit.

## Related Documentation

1. [Provider Safe Initialization Contract](providers.md#safe-initialization-contract)
2. [Automation Layer](automation.md)
3. [Runtime Configuration](configuration.md)
4. [HTTP API Reference](http-api.md)
5. [anolis-provider-sim fault injection reference](https://github.com/anolishq/anolis-provider-sim/blob/main/README.md#fault-injection-api)
6. [anolis-provider-sim safe initialization reference](https://github.com/anolishq/anolis-provider-sim/blob/main/README.md#safe-initialization-in-provider-sim)
