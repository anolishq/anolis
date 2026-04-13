# Anolis Automation Layer

Automation system for orchestrating device control using behavior trees.

## Overview

The automation layer adds configurable machine behavior on top of Anolis's stable IO primitives. It provides:

- **Behavior Trees** — Composable, reactive control logic that orchestrates device calls
- **Runtime Modes** — State machine governing when automation runs and how manual control interacts
- **Parameter System** — Config-tunable setpoints and limits

## Build-Time Flag

BehaviorTree automation can be compiled out with:

```bash
cmake -DANOLIS_ENABLE_AUTOMATION=OFF ...
```

When compiled out, BT-specific components are not built (`anolis_automation`, `bt_nodes_sanity`), and BT endpoints
return `UNAVAILABLE` with a build-time-disabled message.

## Architecture Constraints

The automation layer is a **consumer of kernel services**, NOT a replacement for core IO:

| Constraint                                | Implementation                                                   |
| ----------------------------------------- | ---------------------------------------------------------------- |
| **BT nodes read via StateCache**          | No direct provider access; kernel remains single source of truth |
| **BT nodes act via CallRouter**           | All device calls go through validated control path               |
| **No new provider protocol features**     | Automation uses existing ADPP v1 capabilities                    |
| **No device-specific logic in BT engine** | BT runtime is capability-agnostic                                |

The BT engine sits **above** the kernel, not beneath it.

### BT Service Context Contract

Custom BT nodes consume a typed blackboard payload populated by `BTRuntime`:

- BlackBoard key: `anolis.bt_service_context`
- Payload type: `BTServiceContext`

`BTServiceContext` fields:

- `state_cache` (required)
- `call_router` (required)
- `provider_registry` (required)
- `parameter_manager` (optional)

`BTRuntime` refreshes this context before each `tick()` (including direct/test `tick()` calls),
so threaded and direct tick paths use the same node-service contract.

---

## Runtime Modes

### Mode State Machine

Anolis supports four runtime modes:

| Mode       | Description               | BT State | Manual Calls    | Control Operations |
| ---------- | ------------------------- | -------- | --------------- | ------------------ |
| **IDLE**   | Safe startup/standby mode | Stopped  | ❌ Blocked      | Safe default state |
| **MANUAL** | Operator control active   | Stopped  | ✅ Allowed      | Testing/manual ops |
| **AUTO**   | Automated control active  | Running  | Gated by policy | Normal operation   |
| **FAULT**  | Error recovery state      | Stopped  | ❌ Blocked      | Requires recovery  |

**Operation Types:**

- **Control Operations**: `CallDevice` (actuates hardware)
- **Read-Only Operations**: Health, status, `ReadSignals`, discovery

**IDLE vs MANUAL:**

- **IDLE**: System is safe but inactive (default at startup). Control operations blocked. Read-only queries allowed.
  Operator must explicitly transition to MANUAL to enable control.
- **MANUAL**: System ready for operator control. All manual operations allowed. Common mode for testing and calibration.

### Mode Transitions

Valid transitions:

```text
           ┌──────────┐
    ┌──────┤  MANUAL  ├──────┐
    │      └────┬─────┘      │
    │           │            │
    ▼           ▼            ▼
┌──────┐    ┌──────┐     ┌──────┐
│ IDLE │    │ AUTO │     │FAULT │
└──┬───┘    └──────┘     └───┬──┘
   │                          │
   └──────────────────────────┘
      (Any → FAULT allowed)
```

**Valid transitions:**

- ✅ IDLE ↔ MANUAL
- ✅ MANUAL ↔ AUTO
- ✅ Any → FAULT
- ✅ FAULT → MANUAL (recovery path)

**Invalid transitions:**

- ❌ FAULT → AUTO — Must recover through MANUAL first
- ❌ FAULT → IDLE — Must recover through MANUAL first
- ❌ AUTO → IDLE — Must go through MANUAL first (prevents automation bypass)

**Rationale:**

- **Fault recovery requires explicit operator acknowledgment** before resuming automation
- **IDLE enforces safe startup** by requiring explicit transition through MANUAL to AUTO
- **No shortcuts from AUTO to IDLE** prevents operators from accidentally killing automation without proper shutdown

### Default Mode

- **Startup:** IDLE (safe default, control operations blocked)
- **After fault:** FAULT (requires explicit recovery)
- **Runtime mode:** System starts in IDLE (not configurable). Use `POST /v0/mode` to transition modes

**Safe Startup Sequence:**

1. Runtime starts in IDLE (control blocked)
2. Providers initialize with safe device defaults
3. Operator verifies state using read-only queries
4. Operator transitions to MANUAL (enables control)
5. Operator performs verification/calibration
6. Operator transitions to AUTO (enables automation)

---

## Manual/Auto Contention Policy

When in AUTO mode, manual device calls are gated by the configured policy:

### BLOCK Policy (Default)

Manual calls are **rejected** when automation is active.

```yaml
automation:
  manual_gating_policy: BLOCK
```

**Behavior:**

- POST /v0/call returns `FAILED_PRECONDITION` error
- Logged as warning: "Manual call blocked in AUTO mode"
- BT continues running uninterrupted

**Use case:** Prevent accidental operator interference during automated sequences.

### OVERRIDE Policy

Manual calls are **allowed** when automation is active.

```yaml
automation:
  manual_gating_policy: OVERRIDE
```

**Behavior:**

- POST /v0/call succeeds normally
- Logged as info: "Manual call overriding AUTO mode"
- BT continues running (may conflict with manual action)

**Use case:** Allow expert operators to override automation when needed.

---

## HTTP API for Mode Control

### GET /v0/mode

Get current automation mode.

**Request:**

```http
GET /v0/mode HTTP/1.1
```

**Response (automation enabled):**

```json
{
  "status": { "code": "OK", "message": "ok" },
  "mode": "MANUAL"
}
```

**Response (automation disabled):**

```json
{
  "status": {
    "code": "UNAVAILABLE",
    "message": "Automation layer not enabled"
  }
}
```

### POST /v0/mode

Set automation mode.

**Request:**

```http
POST /v0/mode HTTP/1.1
Content-Type: application/json

{
  "mode": "AUTO"
}
```

**Response (success):**

```json
{
  "status": { "code": "OK", "message": "ok" },
  "mode": "AUTO"
}
```

**Response (invalid transition):**

```http
HTTP/1.1 412 Precondition Failed
Content-Type: application/json

{
  "status": {
    "code": "FAILED_PRECONDITION",
    "message": "Invalid mode transition: FAULT -> AUTO"
  }
}
```

**Response (invalid mode string):**

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json

{
  "status": {
    "code": "INVALID_ARGUMENT",
    "message": "Invalid mode: 'BADMODE' (must be MANUAL, AUTO, IDLE, or FAULT)"
  }
}
```

---

## Mode Change Events

Mode transitions emit telemetry events for observability.

**Event Type:** `mode_change`

**Fields:**

- `previous_mode` (string): Mode before transition
- `new_mode` (string): Mode after transition
- `timestamp_ms` (int64): Unix timestamp in milliseconds

**InfluxDB Line Protocol:**

```sh
mode_change previous_mode="MANUAL",new_mode="AUTO" 1706918400000
```

**Grafana Query (Flux):**

```flux
from(bucket: "anolis")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "mode_change")
  |> yield(name: "mode_transitions")
```

---

## Parameters

Anolis supports **runtime parameters** that can be declared in YAML and updated at runtime via HTTP.
Parameters are read-only from Behavior Trees (exposed via the blackboard) and can be validated with min/max ranges or enum allowed values.

### YAML Declaration

Example:

```yaml
automation:
  parameters:
    - name: temp_setpoint
      type: double
      default: 25.0
      min: 10.0
      max: 50.0

    - name: control_enabled
      type: bool
      default: true

    - name: operating_mode
      type: string
      default: "normal"
      allowed_values: ["normal", "test", "emergency"]
```

Notes:

- Supported types: `double`, `int64`, `bool`, `string`.
- `min`/`max` apply to numeric types only.
- `allowed_values` applies to string enums.
- **Persistence** (writing changes back to YAML) is **not implemented yet**.
- The intent is to keep this opt-in behind a future `automation.persist_parameters` flag.

### HTTP API for Parameters

GET /v0/parameters

- Returns the list of declared parameters with current values and constraints.

POST /v0/parameters

- Update a parameter at runtime.
- Body: `{"name": "temp_setpoint", "value": 30.0}`
- Server validates type and constraints and replies with 200 OK on success or 400/INVALID_ARGUMENT on validation failure.

Example (update):

```http
POST /v0/parameters HTTP/1.1
Content-Type: application/json

{"name": "temp_setpoint", "value": 30.0}
```

Example (response):

```json
{
  "status": { "code": "OK", "message": "ok" },
  "parameter": { "name": "temp_setpoint", "value": 30.0 }
}
```

### BT Access

Behavior Trees can access parameters via the `GetParameter` node.
The node is available in the default node registry and reads a parameter by name from the blackboard using the `param` input port.
It returns SUCCESS with the value available on the `value` output port **only for numeric parameter types** (`double`, `int64`).
For `bool` parameters, use `GetParameterBool`.
For strict `int64` typed flows, use `GetParameterInt64`.
For `string` parameters, `GetParameter` returns `FAILURE` by design (no string node yet).

Example:

```xml
<GetParameter param="temp_setpoint" value="{target_temp}"/>
```

---

## BT Node Reference

Anolis provides the following custom BehaviorTree.CPP nodes for interacting with devices and runtime parameters.

### CallDevice

Invokes a device function through the CallRouter with validated arguments.

**Ports:**

| Port          | Type   | Direction | Description                                     |
| ------------- | ------ | --------- | ----------------------------------------------- |
| device_handle | string | input     | Device handle (format: `provider_id/device_id`) |
| function_name | string | input     | Function identifier                             |
| args          | string | input     | Arguments as JSON object (default: `{}`)        |
| success       | bool   | output    | Call result (true/false)                        |
| error         | string | output    | Error message if call failed                    |

**Args JSON Format:**

The `args` port accepts a JSON object string containing function arguments:

```xml
<!-- Single argument -->
<CallDevice device_handle="sim0/tempctl0"
            function_name="set_target_temp"
            args='{"target":30.0}'/>

<!-- Multiple arguments -->
<CallDevice device_handle="sim0/motorctl0"
            function_name="set_motor"
            args='{"mode":"PWM","duty":75,"frequency":1000}'/>

<!-- No arguments -->
<CallDevice device_handle="sim0/relayio0"
            function_name="reset"/>
```

**Supported JSON Types:**

| JSON Type      | Protobuf Value Type | Example            |
| -------------- | ------------------- | ------------------ |
| number (float) | double_value        | `{"target":25.5}`  |
| number (int)   | int64_value         | `{"count":100}`    |
| boolean        | bool_value          | `{"enabled":true}` |
| string         | string_value        | `{"mode":"AUTO"}`  |

**Error Messages:**

- `"args must be a JSON object"` — args is not a valid JSON object
- `"JSON parse error: ..."` — Malformed JSON syntax
- `"Unsupported JSON type for arg 'X'"` — null, array, or nested object passed

**Validation:**

Arguments are validated by CallRouter using ArgSpec metadata from the device registry:

- Required arguments must be present
- Type must match expected type
- Numeric values must be within min/max range

### ReadSignal

Reads a signal value from the StateCache.

**Ports:**

| Port          | Type   | Direction | Description                                 |
| ------------- | ------ | --------- | ------------------------------------------- |
| device_handle | string | input     | Device handle (`provider_id/device_id`)     |
| signal_id     | string | input     | Signal identifier                           |
| value         | double | output    | Signal value (as double)                    |
| quality       | string | output    | Signal quality (OK/STALE/UNAVAILABLE/FAULT) |

**Example:**

```xml
<ReadSignal device_handle="sim0/tempctl0"
            signal_id="temperature"
            value="{current_temp}"
            quality="{temp_quality}"/>
```

### CheckQuality

Verifies that a signal's quality meets expectations (condition node).

**Ports:**

| Port             | Type   | Direction | Description                             |
| ---------------- | ------ | --------- | --------------------------------------- |
| device_handle    | string | input     | Device handle (`provider_id/device_id`) |
| signal_id        | string | input     | Signal identifier                       |
| expected_quality | string | input     | Expected quality (default: `OK`)        |

**Returns:**

- `SUCCESS` — Signal quality matches expected
- `FAILURE` — Quality mismatch or signal not found

**Example:**

```xml
<!-- Gate control on sensor availability -->
<Sequence>
    <CheckQuality device_handle="sim0/tempctl0"
                  signal_id="temperature"
                  expected_quality="OK"/>
    <CallDevice device_handle="sim0/tempctl0"
                function_name="set_target_temp"
                args='{"target":30.0}'/>
</Sequence>
```

### GetParameter

Reads a runtime parameter from the ParameterManager.

**Ports:**

| Port  | Type   | Direction | Description                    |
| ----- | ------ | --------- | ------------------------------ |
| param | string | input     | Parameter name                 |
| value | double | output    | Parameter value (numeric only) |

**Returns:**

- `SUCCESS` — Parameter read successfully
- `FAILURE` — Parameter not found, ParameterManager unavailable, or non-numeric parameter type (`bool`/`string`)

**Example:**

```xml
<GetParameter param="temp_setpoint" value="{target_temp}"/>
```

### GetParameterBool

Reads a boolean runtime parameter from the ParameterManager.

**Ports:**

| Port  | Type   | Direction | Description        |
| ----- | ------ | --------- | ------------------ |
| param | string | input     | Parameter name     |
| value | bool   | output    | Boolean value      |

**Returns:**

- `SUCCESS` — Parameter read successfully and is bool
- `FAILURE` — Parameter missing, manager unavailable, or non-bool type

### GetParameterInt64

Reads an `int64` runtime parameter from the ParameterManager.

**Ports:**

| Port  | Type   | Direction | Description        |
| ----- | ------ | --------- | ------------------ |
| param | string | input     | Parameter name     |
| value | int64  | output    | `int64` value      |

**Returns:**

- `SUCCESS` — Parameter read successfully and is `int64`
- `FAILURE` — Parameter missing, manager unavailable, or non-`int64` type

### CheckBool

Compares a boolean input against an expected value.

**Ports:**

| Port     | Type | Direction | Description |
| -------- | ---- | --------- | ----------- |
| value    | bool | input     | Value to compare |
| expected | bool | input     | Expected value (default `true`) |

**Returns:**

- `SUCCESS` — `value == expected`
- `FAILURE` — values do not match (or input missing)

### PeriodicPulseWindow

Computes a periodic active window with startup delay.

**Ports:**

| Port                 | Type  | Direction | Description |
| -------------------- | ----- | --------- | ----------- |
| enabled              | bool  | input     | Enable scheduler |
| startup_delay_s      | int64 | input     | Delay before first pulse |
| interval_s           | int64 | input     | Pulse interval |
| pulse_s              | int64 | input     | Pulse width |
| max_pulses_per_hour  | int64 | input     | Optional cap (0 disables cap) |
| now_ms               | int64 | input     | Optional deterministic time override |
| active               | bool  | output    | True when in pulse window |
| pulse_index          | int64 | output    | Pulse index after delay |
| elapsed_ms           | int64 | output    | Elapsed ms since enable |

### EmitOnChangeOrInterval

Gates whether a command should be emitted this tick.

**Ports:**

| Port            | Type   | Direction | Description |
| --------------- | ------ | --------- | ----------- |
| key             | string | input     | Deterministic command signature |
| keepalive_s     | int64  | input     | Keepalive interval |
| min_spacing_ms  | int64  | input     | Minimum spacing between emits |
| force           | bool   | input     | Force immediate emit |
| now_ms          | int64  | input     | Optional deterministic time override |
| emit            | bool   | output    | Emit decision |
| reason          | string | output    | Decision reason |

### BuildArgsJson

Builds JSON object arguments from up to six typed arg slots (`argN_*` ports).

**Output:**

- `json` — JSON object string suitable for `CallDevice.args`.

---

## Configuration

### Enabling Automation

```yaml
automation:
  enabled: true
  behavior_tree: ./behaviors/demo.xml
  tick_rate_hz: 10 # 1-1000 Hz
  manual_gating_policy: BLOCK # BLOCK or OVERRIDE
```

### Configuration Fields

| Field                  | Type   | Default  | Description                            |
| ---------------------- | ------ | -------- | -------------------------------------- |
| `enabled`              | bool   | false    | Enable/disable automation layer        |
| `behavior_tree`        | string | required | Path to BT XML file                    |
| `tick_rate_hz`         | int    | 10       | BT execution rate (1-1000)             |
| `manual_gating_policy` | string | BLOCK    | Manual call policy (BLOCK or OVERRIDE) |
| `parameters`           | list   | []       | Parameter definitions                  |
| `mode_transition_hooks`| map    | {}       | Generic before/after mode transition hook calls |

Mode-transition hook fail-safe rule:

- Transitions to `FAULT` are never vetoed by `before_transition` callback failures.
- Hook errors are logged and runtime still enters `FAULT`.

---

## BT Execution Gating

The BT tick loop only runs when mode == AUTO:

```cpp
while (running) {
  if (mode != AUTO) {
    sleep(tick_period);
    continue;  // Skip tick
  }

  tree.tick();
  sleep(tick_period);
}
```

**Behavior:**

- MANUAL/IDLE/FAULT → BT paused cleanly (no state corruption)
- AUTO → BT ticks normally
- Transitioning to AUTO restarts BT from root node

---

## Dependency Notes

BehaviorTree.CPP is pulled via vcpkg (`behaviortree-cpp`). If vcpkg lags, the fallback is to vendor a known-good release from GitHub.
Link it in `core/CMakeLists.txt` and keep the same include paths to avoid code changes.
Keep the same include paths to avoid code changes.

## Usage Examples

### Starting Automated Control

```bash
# 1. Verify current mode
curl http://localhost:8080/v0/mode

# 2. Switch to AUTO mode
curl -X POST http://localhost:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"AUTO"}'

# 3. Monitor mode transitions in telemetry
# (Check Grafana or query InfluxDB)
```

### Emergency Stop

```bash
# Put system in FAULT mode (stops automation)
curl -X POST http://localhost:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"FAULT"}'

# Manually control devices
curl -X POST http://localhost:8080/v0/call \
  -H "Content-Type: application/json" \
  -d '{
    "provider_id": "sim0",
    "device_id": "tempctl0",
    "function_name": "set_target_temp",
    "arguments": {"target": 20.0}
  }'
```

### Recovery from FAULT

```bash
# 1. Recover to MANUAL
curl -X POST http://localhost:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}'

# 2. Verify system state
curl http://localhost:8080/v0/state

# 3. Resume automation
curl -X POST http://localhost:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"AUTO"}'
```

---

## Operator UI Integration (Planned)

The Operator UI does not yet expose mode or parameter controls. When UI work begins, use these endpoints:

- `GET /v0/mode` and `POST /v0/mode` for mode control
- `GET /v0/parameters` and `POST /v0/parameters` for runtime parameter tuning

## Testing

Automated tests for automation features:

```bash
# Run automation tests
python -m pytest tests/integration/test_integration.py -k automation_suite

# Test with custom executable paths
python -m pytest tests/integration/test_integration.py -k automation_suite \
  --runtime path/to/anolis-runtime --provider path/to/anolis-provider-sim
```

**Test coverage:**

- Mode transitions (valid and invalid)
- Manual/auto contention policy
- BT execution gating
- Mode change events
- Automation disabled behavior

---

## Safety Disclaimer

⚠️ **Automation is a control policy layer, not a safety-rated system.**

External safety systems (e.g., E-stops, interlocks) are **still required** for real hardware.

- FAULT mode is _policy_, not a certified safety mechanism
- Mode transitions are not safety-rated
- Manual override capability must not replace proper safety interlocks

For production deployment:

1. Integrate with certified safety PLCs
2. Implement hardware E-stops independent of Anolis
3. Add watchdog timers for automation health
4. Design BTs with safe failure modes

---

## References

- [BehaviorTree.CPP Documentation](https://www.behaviortree.dev/)
- [HTTP API Documentation](./http-api.md)
- [Architecture Overview](./architecture.md)
