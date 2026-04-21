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
3. Inventory/capabilities: through `ProviderRegistry`.
4. Parameters: through `ParameterManager`.
5. No provider-specific behavior is encoded in the BT runtime engine.

## Runtime Modes

| Mode | Automation | Manual Calls | Control Behavior |
| --- | --- | --- | --- |
| `IDLE` | Stopped | Blocked | Safe startup |
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

## Runtime Parameters

Parameters are declared in runtime YAML and mutable via HTTP.

Example:

```yaml
automation:
  enabled: true
  behavior_tree: ./behaviors/demo.xml
  tick_rate_hz: 10
  manual_gating_policy: BLOCK
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

```bash
# Linux
./build/dev-release/core/anolis-runtime --config ./config/anolis-runtime.yaml

# Windows
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\config\anolis-runtime.yaml
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
