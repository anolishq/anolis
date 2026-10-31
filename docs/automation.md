# Automation

Automation in Anolis is a behavior-tree layer above core runtime services.

## Contract and Build Context

1. Runtime config contract: `schemas/runtime/runtime-config.schema.json`
2. Runtime HTTP contract: `schemas/http/runtime-http.openapi.v0.yaml`
3. Runtime mode/parameter endpoints:
   - `GET/POST /v0/mode`
   - `GET/POST /v0/parameters`
   - `GET /v0/automation/status`
   - `GET /v0/automation/tree`

Build-time option:

```bash
cmake -DANOLIS_ENABLE_AUTOMATION=OFF ...
```

When compiled out, automation endpoints return `UNAVAILABLE`.

## Architecture Boundaries

Automation is a consumer of kernel services, not a replacement for them.

1. Reads: through `StateCache`.
2. Writes: through `CallRouter`.
3. Inventory/capabilities: not read by the BT; `CallRouter` validates each call
   against `DeviceRegistry` and dispatches it through `ProviderRegistry`.
4. Parameters: through `ParameterManager`.
5. No provider-specific behavior is encoded in the BT runtime engine.

## Runtime Modes

| Mode | Automation | Manual Calls | Control Behavior |
| --- | --- | --- | --- |
| `IDLE` | Stopped | Blocked | Startup mode; outputs set by `safety.on_startup` |
| `MANUAL` | Stopped | Allowed | Operator control |
| `AUTO` | Running | Policy-gated | Automated control |
| `FAULT` | Stopped | Allowed | Manual recovery state |

Transition rules:

1. `IDLE <-> MANUAL`
2. `MANUAL <-> AUTO`
3. `Any -> FAULT` (valid transition target)
4. `FAULT -> MANUAL`
5. `FAULT -> AUTO` is not allowed directly.
6. `AUTO -> IDLE` is not allowed directly.

Startup is always `IDLE`.

## Manual Gating Policy in AUTO

`automation.manual_gating_policy` controls manual calls while mode is `AUTO`.

1. `BLOCK` (default): manual `POST /v0/call` rejected.
2. `OVERRIDE`: manual calls allowed during AUTO.

Control-operation guardrails in current runtime:

1. `IDLE` blocks control operations at `CallRouter`.
2. `AUTO` applies manual-call gating policy (`BLOCK`/`OVERRIDE`).
3. `FAULT` is primarily a recovery mode; transition constraints apply, but control calls are not globally blocked by `CallRouter`.

## Provider-declared function policy

A provider's `FunctionPolicy` can restrict a function, and the runtime enforces
it in `CallRouter`, as the protocol requires (#566). `GET
/v0/devices/{provider_id}/{device_id}/capabilities` shows all three fields.

1. `allowed_modes`: a call in any other mode is refused with
   `FAILED_PRECONDITION`, naming the modes and the current one. This holds for
   manual calls, the behavior tree and mode-transition hooks alike. Empty means
   every mode. A runtime without automation has no mode and judges calls as
   `MANUAL`.
2. `min_interval_ms`: a call that arrives sooner than this after the last one
   dispatched to that function is refused with `FAILED_PRECONDITION` and the
   time remaining. It is refused, not deferred: retrying is the caller's
   decision.
3. `requires_lease`: protocol v1 leases are not implemented. Discovery logs a
   WARN naming each function that sets it, and the function is called without a
   lease.

Software-stop and startup safe-state calls are exempt from `allowed_modes` and
`min_interval_ms`, so the ladder can always drive outputs safe.

## Runtime Parameters

Parameters are declared in runtime YAML and mutable via HTTP.

Values set over HTTP are not persisted. Every start begins at the config
defaults, and the startup log lists each parameter with its default value.
`GET /v0/parameters` reports `source: default` until a `POST` changes a value,
then `source: operator`. The runtime always starts in `IDLE`, so a value that
reverted on restart takes effect only once an operator re-enters `AUTO`.

Example:

<!-- validate: runtime-config fragment -->

```yaml
automation:
  enabled: true
  behavior_tree: ./tests/integration/fixtures/behaviors/demo.xml
  parameters:
    - name: temp_setpoint
      type: double
      default: 25.0
      min: 10.0
      max: 50.0
    - name: control_enabled
      type: bool
      default: true
```

**Safe-state hooks (required to enter AUTO with actuators).** Automation that
drives actuating outputs must declare safe-state `mode_transition_hooks`; the
runtime refuses `MANUAL -> AUTO` (HTTP 409) without them — see
[safety.md](safety.md#manual-to-auto). Add a `*->FAULT` hook that drives actuators
to a safe value:

> This hook is **not** the software stop's safe state. `POST /v0/estop` runs
> `safety.safe_state`, which this gate does not check — declare both. A machine
> with the hook and no `safety.safe_state` passes the gate while its software stop
> has no safe state of its own: the stop runs this hook on its way into FAULT
> (except in IDLE, where the IDLE gate refuses `before_transition` calls) and
> reports `safe_state_achieved: false`. Once `safety.safe_state` declares a rung,
> the stop's latch refuses this hook and the rung alone drives the outputs.

<!-- validate: runtime-config fragment -->

```yaml
automation:
  mode_transition_hooks:
    before_transition:
      - to: FAULT
        fail_on_error: false
        calls:
          - device_handle: sim0/motorctl0
            function_name: set_motor_duty
            args: { motor_index: 1, duty: 0.0 }
```

`fail_on_error` (default `true`) makes a failed `before_transition` call veto
the transition: the runtime issues none of the remaining calls and stays in the
current mode. On entry to `FAULT` it has no effect. FAULT cannot be vetoed, so
every call in every matching hook is attempted, each failure is logged at error
level, and the mode still changes to FAULT.

Supported types:

1. `double`
2. `int64`
3. `bool`
4. `string`

Constraint rules:

1. `min/max` are numeric-only.
2. `allowed_values` is string-only.

## Custom BT Nodes (Runtime)

Core runtime nodes include:

1. `CallDevice`
2. `ReadSignal`
3. `CheckQuality`
4. `GetParameter`
5. `GetParameterBool`
6. `GetParameterInt64`
7. `CheckBool`
8. `PeriodicPulseWindow`

Use runtime capability metadata and parameter constraints to keep BT logic generic.

## Quick Walkthrough

### 1) Start runtime with automation enabled

`examples/anolis-runtime.yaml` ships with `automation.enabled: false`. Before
starting, set `automation.enabled: true` and `behavior_tree`, and add the
`parameters` and `mode_transition_hooks` blocks from
[Runtime Parameters](#runtime-parameters). Without the hook, step 4 returns
HTTP 409.

```bash
# Linux
./build/dev-release/core/anolis-runtime --config ./examples/anolis-runtime.yaml

# Windows
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\examples\anolis-runtime.yaml
```

### 2) Verify startup mode

```bash
curl -s http://127.0.0.1:8080/v0/mode | jq
```

Expected mode at startup: `IDLE`.

### 3) Inspect and update parameters

```bash
curl -s http://127.0.0.1:8080/v0/parameters | jq

curl -s -X POST http://127.0.0.1:8080/v0/parameters \
  -H "Content-Type: application/json" \
  -d '{"name":"temp_setpoint","value":30.0}' | jq
```

### 4) Move into AUTO

```bash
curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}' | jq

curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"AUTO"}' | jq
```

### 5) Check automation status

```bash
curl -s http://127.0.0.1:8080/v0/automation/status | jq
```

### 6) Optional: inspect loaded tree

```bash
curl -s http://127.0.0.1:8080/v0/automation/tree | jq
```

## Troubleshooting

1. `UNAVAILABLE` from automation endpoints:
   - automation disabled in config or compiled out.
2. BT load failure:
   - invalid tree path/XML or port type mismatch.
3. Manual calls blocked unexpectedly:
   - verify mode and `manual_gating_policy`.
4. Parameter update rejected:
   - check type/constraint mismatch against declared parameter.

## Safety Reminder

Automation provides orchestration policy, not safety certification.
External safety systems (E-stop/interlocks/watchdogs) remain required for real hardware deployments.
