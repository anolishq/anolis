# Runtime HTTP API (v0)

Human-oriented runtime API guide.

Machine contract authority:

1. OpenAPI:
   [`schemas/http/runtime-http.openapi.v0.yaml`](https://github.com/anolishq/anolis/blob/main/schemas/http/runtime-http.openapi.v0.yaml),
   also rendered on the docs site as the
   [Runtime HTTP API reference](https://anolishq.github.io/reference/runtime-http-api)
2. Contract examples:
   [`tests/contracts/runtime-http/examples/`](https://github.com/anolishq/anolis/tree/main/tests/contracts/runtime-http/examples)
3. Baseline: [runtime-http-baseline.md](contracts/runtime-http-baseline.md)

Use this file for operational semantics and practical examples.

## Transport

1. Base URL default: `http://127.0.0.1:8080`
2. JSON content type: `application/json`
3. API prefix: `/v0`
4. Authentication: bearer token, off by default (`http.auth_enabled`). With it
   on, every request needs `Authorization: Bearer <token>`, including one from
   this host unless `http.auth_exempt_loopback` is set. The check runs before
   routing, so it covers every endpoint and the `/v0/events` stream. Without a
   valid token the reply is `401` with
   `WWW-Authenticate: Bearer` and the body `{"error":"unauthorized"}`, not the
   envelope below. `install.sh` generates the token into
   `/etc/anolis/runtime.env`. See [security.md](security.md).

5. Request checks, before routing and before authentication (#540):
   - with auth off, the `Host` header must name this machine (`localhost`,
     `127.0.0.1`, `::1` or the bind address), or the reply is `403`
     `{"error":"host not allowed"}`. `http.allow_insecure_bind` turns this
     check off;
   - a `POST` to `/v0/` that carries an `Origin` header must come from an
     origin in `http.cors_allowed_origins`, or the reply is `403`
     `{"error":"origin not allowed"}`. Tools such as `curl` send no `Origin`;
   - a `POST` with a body must send `Content-Type: application/json`, or the
     reply is `415`. A bodyless `POST`, such as `/v0/estop`, needs none.

CORS response headers come from runtime config (`http.cors_*`).

## Common Response Envelope

All JSON responses include `status`:

<!-- validate: none -->

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
   `POST /v0/call` is permitted and no behaviour tree or mode machine drives
   outputs. The runtime still issues safe-state calls on its own: once at
   startup (`safety.on_startup: safe_state`, the default), and when a latched
   device returns, to re-assert its safe state.
2. To tell whether a mode machine exists, call `GET /v0/mode`. On an
   automation-disabled runtime it returns `503 UNAVAILABLE`
   (`Automation layer not enabled`), as do `POST /v0/mode`,
   `GET /v0/automation/tree` and `GET /v0/automation/status`.
   `GET /v0/parameters` and `POST /v0/parameters` return `503` too, with
   `Parameter system not enabled`: parameters are declared under
   `automation`. A `MANUAL` from `/v0/runtime/status` alone does not
   distinguish the two cases.

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
   `failed_actions` (same shape as the `/v0/estop` actions; a failed one
   carries `cause`, `config` or `run`, see
   [safety.md](safety.md#outputs-at-startup)), and
   `skipped_actions`: calls not issued, each with `device_handle`,
   `function` and `reason`. A call is skipped when its provider reported the
   device `UNREACHABLE` and it is not registered (`reason` is the provider's
   message, or `reported unreachable by its provider` when it gives none), or
   when the device is in `safety.stop_chain.devices` and the stop chain was
   `in_force` at startup (`reason`: `stop chain in force`). A device that meets
   both conditions carries the provider-derived `reason`. `ran` is
   `false` for `leave` and when no safe state is declared. Runtimes before
   0.1.51 do not send `skipped_actions`.
5. This is a software stop, not a substitute for the hardware emergency stop.
6. With `safety.stop_chain` (see [configuration-schema.md](configuration-schema.md)),
   `GET /v0/runtime/status` carries `stop_chain`: `state` (`in_force`, `clear`
   or `unknown`), `reason`, `signal`, `devices`, `expectations` (each
   device's declared `expect`, by handle), `expected_losses`, `mismatches`
   and `since_epoch_ms`. A move of the state is a `stop_chain_change` event on
   `/v0/events`. A `mismatches` entry (`device`, `expect`, `reason`,
   `since_epoch_ms`) is "Stop not reaching *device*": a declared `expect`
   contradicted while the stop is in force. Each one raised or resolved is a
   `stop_chain_mismatch` event (`device`, `expect`, `raised`, `reason`,
   `timestamp_ms`). While the state is `in_force` or `unknown`,
   `POST /v0/estop/clear` answers `409` FAILED_PRECONDITION with the reason in
   `status.message`, and the latch stays engaged.

### Device-loss latch

`GET /v0/runtime/status` carries `device_loss_latched`: an array of device
handles whose provider answered a read and reported the device itself
unreachable (`UNAVAILABLE` or `DEADLINE_EXCEEDED` for that device), and that
have not been re-armed since.

A provider that hangs, crashes or is restarted does not latch its devices:
provider loss is not device loss (see [safety.md](safety.md)). Its devices go
unavailable, with `device_availability` events, and come back without a latch.
Once the provider is back, a read it reports failed for a device latches that
device as usual.

Notes:

1. While a device is latched, behaviour-tree calls that actuate it are refused
   with `FAILED_PRECONDITION`. Manual `POST /v0/call`, mode-transition hooks and
   safe-state calls are unaffected — an operator commanding a device they can
   see is the deliberate action that "permit restarting" means.
2. Latches clear on a `MANUAL -> AUTO` transition, and on nothing else while
   the runtime runs. They are held in memory, so a restarted runtime starts
   with none, in `IDLE`, and automation still needs a deliberate
   `MANUAL -> AUTO`. Releasing
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

### Runs

A run is a recorded span of operation: an experiment, a batch. At most one is
open at a time, and its id is `<runtime.name>-<ULID>`. The runtime records its
own lifecycle events in it (`run_opened`, `run_closed`, `mode_change`,
`parameter_change`, `automation_fault`); operators add markers. Telemetry is
tied to a run by its time window.

1. `POST /v0/runs` - open a run. Optional body: `label`, `params` (any
   object), `tag_scope`. Returns `409` while another run is open. A run's
   record carries `label` and, through 0.9.x, the same value as the deprecated
   `experiment_label`, which a request may still send instead of `label`.
   Sent both with different values, the request is refused with `400`.
2. `GET /v0/runs` - list runs, newest first (`limit`, `offset`).
3. `GET /v0/runs/{run_id}` - one run.
4. `POST /v0/runs/{run_id}/close` - close a run; idempotent. Optional
   `close_reason`: `operator_stop`, `completed`, `failed` or `superseded`.
5. `GET /v0/runs/{run_id}/events` - the run's events and markers, oldest first
   (`limit`, `offset`).
6. `POST /v0/runs/{run_id}/events` - add an operator marker: `type` (required,
   your own vocabulary, such as `feed` or `sample`), optional `payload` object
   and `occurred_at_epoch_ms`. Returns `409` on a closed run.

Runs need a writable `runtime.data_dir`; without one every run endpoint
returns `503`.

### Telemetry

1. `GET /v0/telemetry/status` - the InfluxDB sink's health and throughput
   counters. See [Telemetry](observability.md).

### Streaming

1. `GET /v0/events` (SSE)

Optional query filters:

1. `provider_id`
2. `device_id`
3. `signal_id`

Current event names:

1. `state_update` (sent when a signal's value or quality changes; carries
   `quality`)
2. `device_availability`
3. `mode_change`
4. `parameter_change`
5. `automation_fault`
6. `automation_call_failed` (each device call automation made that was refused
   or failed: `device_handle`, `function`, `error`, `consecutive_failures`; not
   narrowed by the filters above)
7. `device_removed`
8. `stop_chain_change`
9. `stop_chain_mismatch`

`quality_change` is defined in the wire format, but the runtime does not emit
it.

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

These need `automation.enabled: true`; see [Automation](automation.md).

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

<!-- validate: none -->

```json
{
  "status": {
    "code": "NOT_FOUND",
    "message": "Device not found: sim0/nonexistent"
  }
}
```

## Related

1. [`docs/http/README.md`](https://github.com/anolishq/anolis/blob/main/docs/http/README.md) -
   HTTP contract validation workflow.
2. [`tests/contracts/runtime-http/examples/manifest.yaml`](https://github.com/anolishq/anolis/blob/main/tests/contracts/runtime-http/examples/manifest.yaml) -
   fixture inventory.
