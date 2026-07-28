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

### Safe-state (software e-stop)

1. `POST /v0/estop` - engage the latching software safe-state.
2. `POST /v0/estop/clear` - release the latch.

Notes:

1. `/v0/estop` runs the declared safe-state ladder (hooks -> per-actuator
   setpoints, only when they cover every actuating output -> zero, only when
   `safety.safe_state.zero_is_safe` is set -> otherwise an honest
   `software_safe_state: "none"`) and works whether or not automation is
   enabled. On an automation machine it also drives `FAULT`.
2. While latched, actuating `POST /v0/call` requests are refused with `409`
   (fail-closed: a function without an explicit read-only category counts as
   actuating). Read-only calls are unaffected.
3. Latch and planned safe-state are surfaced under `estop` in
   `GET /v0/runtime/status`.
4. This is a software stop, not a substitute for the hardware backplane cut.

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
6. `bt_error`
7. `provider_health_change`

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

### Software safe-state (e-stop)

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
