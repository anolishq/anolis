# Runtime HTTP API (v0)

Human-oriented runtime API guide.

Machine contract authority:

1. OpenAPI: `schemas/http/runtime-http.openapi.v0.yaml`
2. Contract examples: `tests/contracts/runtime-http/examples/`
3. Baseline: `docs/contracts/runtime-http-baseline.md`

Use this file for operational semantics and practical examples.

## Transport

1. Base URL default: `http://127.0.0.1:8080`
2. JSON content type: `application/json`
3. API prefix: `/v0`
4. No auth model in v0

CORS behavior comes from runtime config (`http.cors_*`).

## Common Response Envelope

All JSON responses include `status`:

```json
{
  "status": {
    "code": "OK",
    "message": "ok"
  }
}
```

Common status-code mapping:

| Code                  | HTTP |
| --------------------- | ---- |
| `OK`                  | 200  |
| `INVALID_ARGUMENT`    | 400  |
| `NOT_FOUND`           | 404  |
| `FAILED_PRECONDITION` | 409  |
| `UNAVAILABLE`         | 503  |
| `DEADLINE_EXCEEDED`   | 504  |
| `INTERNAL`            | 500  |

## Endpoint Map

### Runtime and provider status

1. `GET /v0/runtime/status` - high-level runtime/provider summary.
2. `GET /v0/providers/health` - per-provider lifecycle/supervision and device health.

Notes:

1. `mode` in `GET /v0/runtime/status` is the control regime in effect, not a
   probe for the mode machine. With automation enabled it is the mode
   machine's current mode. With `automation.enabled: false` there is no mode
   machine and `mode` is always `MANUAL`, which is accurate: manual
   `POST /v0/call` is permitted and nothing actuates on its own.
2. To tell whether a mode machine exists, call `GET /v0/mode`. On an
   automation-disabled runtime it returns `503 UNAVAILABLE`
   (`Automation layer not enabled`), as do `POST /v0/mode`,
   `GET /v0/automation/tree` and `GET /v0/automation/status`. A `MANUAL`
   from `/v0/runtime/status` alone does not distinguish the two cases.

### Discovery and capabilities

1. `GET /v0/devices`
2. `GET /v0/devices/{provider_id}/{device_id}/capabilities`

### State reads

1. `GET /v0/state`
2. `GET /v0/state/{provider_id}/{device_id}` (supports repeated `signal_id` filters)

### Control

1. `POST /v0/call`

Notes:

1. Request currently requires `function_id`.
2. Args use typed ADPP value encoding.

### Safe-state (software stop)

1. `POST /v0/estop` - engage the latching software safe-state.
2. `POST /v0/estop/clear` - release the latch.

Notes:

1. `/v0/estop` runs the declared safe-state ladder (hooks -> per-actuator
   setpoints, only when they cover every actuating output -> zero, only when
   `safety.safe_state.zero_is_safe` is set -> otherwise an honest
   `software_safe_state: "none"`) and works whether or not automation is
   enabled. On an automation machine it also drives `FAULT`. The response
   carries `safe_state_achieved`: `true` only when the rung issued at least
   one call and every call succeeded, `false` when any call failed, when the
   rung issued no call, or when no rung applies. It covers only the calls
   the rung issued: under `hooks` it can be `true` while actuating functions
   no hook drives remain (`uncovered_actuating_functions` in
   `GET /v0/runtime/status`). A call succeeds when its provider reports it
   OK; that confirms the command was accepted, not that the device was seen
   in the commanded state. The same verdict is logged when a returning
   device's safe state is re-asserted.
2. While latched, actuating `POST /v0/call` requests are refused with `409`
   (fail-closed: a function without an explicit read-only category counts as
   actuating). Read-only calls are unaffected.
3. Latch and planned safe-state are surfaced under `estop` in
   `GET /v0/runtime/status`.
4. What `safety.on_startup` did at startup is surfaced under
   `startup_safe_state` in `GET /v0/runtime/status`: `on_startup`, `ran`,
   `software_safe_state`, `safe_state_achieved` (the same verdict) and
   `failed_actions` (same shape as the `/v0/estop` actions). `ran` is
   `false` for `leave` and when no safe state is declared.
5. This is a software stop, not a substitute for the hardware emergency stop.

### Device-loss latch

`GET /v0/runtime/status` carries `device_loss_latched`: an array of device
handles that went unreachable at the transport level and have not been re-armed.

Notes:

1. While a device is latched, behaviour-tree calls that actuate it are refused
   with `FAILED_PRECONDITION`. Manual `POST /v0/call`, mode-transition hooks and
   safe-state calls are unaffected — an operator commanding a device they can
   see is the deliberate action that "permit restarting" means.
2. Latches clear on a `MANUAL -> AUTO` transition, and on nothing else. Releasing
   an emergency stop must not restart the machine (ISO 13850 §4.1.4).
3. The array is always present and always an array, empty when nothing is
   latched.

### Automation and parameters

1. `GET /v0/mode`
2. `POST /v0/mode`
3. `GET /v0/parameters`
4. `POST /v0/parameters`
5. `GET /v0/automation/tree`
6. `GET /v0/automation/status`

### Streaming

1. `GET /v0/events` (SSE)

Optional query filters:

1. `provider_id`
2. `device_id`
3. `signal_id`

Current event names:

1. `state_update`
2. `quality_change`
3. `device_availability`
4. `mode_change`
5. `parameter_change`
6. `automation_fault`
7. `device_removed`

Provider health is not streamed. Read `GET /v0/providers/health` for it.

`device_availability` carries `provider_id`, `device_id`, `available` and
`timestamp_ms`. It reports edges, not state:

- `available: false` when a device becomes unreachable: a read of it fails,
  or its provider is found unavailable or missing (a crashed provider awaiting
  its supervised restart).
- `available: true` on the first successful read after that, sent before
  that read's `state_update` events.
- One event per transition. Repeated failed polls send nothing more.
- Nothing at startup or on the first poll: a device starts out available.
- No loss is sent once the runtime is shutting down, so a stop does not
  report its providers exiting as device losses.
- No replay. A subscriber that connects during an outage sees only the
  `available: true`. For the current state, read `GET /v0/state`, where an
  unavailable device has `quality: "UNAVAILABLE"`.

`device_removed` carries `provider_id`, `device_id` and `timestamp_ms`. It is
sent when a provider restart republishes an inventory without a device the
runtime was tracking, whether that device was available or not. The device has
no state after it and no `device_availability` follows, including for a device
last reported `available: false`. The `provider_id` and `device_id` filters
apply to it.

## Typed Value Encoding

ADPP value payload format:

| Type | Example |
| --- | --- |
| `double` | `{"type":"double","double":1.23}` |
| `int64` | `{"type":"int64","int64":-42}` |
| `uint64` | `{"type":"uint64","uint64":42}` |
| `bool` | `{"type":"bool","bool":true}` |
| `string` | `{"type":"string","string":"AUTO"}` |
| `bytes` | `{"type":"bytes","base64":"AAECAw=="}` |

## Quality Semantics

Signal quality values:

1. `OK`
2. `STALE`
3. `UNAVAILABLE`
4. `FAULT`

Device-level quality is computed as worst signal quality.

## Practical Examples

### Runtime status

```bash
curl -s http://127.0.0.1:8080/v0/runtime/status | jq
```

### Providers health

```bash
curl -s http://127.0.0.1:8080/v0/providers/health | jq
```

### Devices + capabilities

```bash
curl -s http://127.0.0.1:8080/v0/devices | jq
curl -s http://127.0.0.1:8080/v0/devices/sim0/motorctl0/capabilities | jq
```

### State

```bash
curl -s http://127.0.0.1:8080/v0/state | jq
curl -s "http://127.0.0.1:8080/v0/state/sim0/motorctl0?signal_id=motor1_duty" | jq
```

### Call device function

```bash
curl -s -X POST http://127.0.0.1:8080/v0/call \
  -H "Content-Type: application/json" \
  -d '{
    "provider_id": "sim0",
    "device_id": "motorctl0",
    "function_id": 1,
    "args": {
      "motor_index": {"type": "int64", "int64": 1},
      "duty": {"type": "double", "double": 0.75}
    }
  }' | jq
```

### Mode and parameters

```bash
curl -s http://127.0.0.1:8080/v0/mode | jq
curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}' | jq

curl -s http://127.0.0.1:8080/v0/parameters | jq
curl -s -X POST http://127.0.0.1:8080/v0/parameters \
  -H "Content-Type: application/json" \
  -d '{"name":"temp_setpoint","value":30.0}' | jq
```

### Software safe-state (software stop)

```bash
# Engage: runs the safe-state ladder and latches actuation.
curl -s -X POST http://127.0.0.1:8080/v0/estop | jq
# While latched, actuating /v0/call returns 409. Release the latch:
curl -s -X POST http://127.0.0.1:8080/v0/estop/clear | jq
```

### SSE stream

```bash
curl -N http://127.0.0.1:8080/v0/events
```

## Error Payload

Example:

```json
{
  "status": {
    "code": "NOT_FOUND",
    "message": "Device not found: sim0/nonexistent"
  }
}
```

## Related

1. `docs/http/README.md` - HTTP contract validation workflow.
2. `tests/contracts/runtime-http/examples/manifest.yaml` - fixture inventory.
