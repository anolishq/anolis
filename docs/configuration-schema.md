# Runtime Configuration Schema (Narrative)

This is a compact human map of runtime YAML sections.

Authoritative contract behavior comes from:

1. `schemas/runtime/runtime-config.schema.json`
2. `tests/contracts/runtime-config/validate-runtime-configs.py`
3. `anolis-runtime --check-config`

## Top-Level Sections

Supported runtime YAML sections:

1. `runtime`
2. `http`
3. `providers`
4. `polling`
5. `telemetry`
6. `logging`
7. `automation`
8. `safety`
9. `health`

Unknown keys are compatibility-warned and ignored.

## `runtime`

Purpose: runtime identity and startup/shutdown timing.

Fields:

1. `name` (string, optional)
2. `shutdown_timeout_ms` (int, bounded)
3. `startup_timeout_ms` (int, bounded)

Notes:

1. Startup mode is always `IDLE` and not configurable via YAML.
2. Runtime mode changes are done through HTTP (`POST /v0/mode`).

## `http`

Purpose: API server binding and CORS policy.

Fields:

1. `enabled` (bool)
2. `bind` (string)
3. `port` (int)
4. `cors_allowed_origins` (string or string array)
5. `cors_allow_credentials` (bool)
6. `thread_pool_size` (int)

Key invariants:

1. Port must be valid TCP port range.
2. Origins list cannot be empty.
3. Wildcard origin (`*`) is invalid when credentials are enabled.

## `providers`

Purpose: runtime-managed provider process list.

Per-provider fields:

1. `id` (unique string)
2. `command` (required string)
3. `args` (string array)
4. `timeout_ms` (int)
5. `hello_timeout_ms` (int)
6. `ready_timeout_ms` (int)
7. `restart_policy` (optional object)

Restart policy fields:

1. `enabled` (bool)
2. `max_attempts` (int)
3. `backoff_ms` (int array)
4. `timeout_ms` (int)
5. `success_reset_ms` (int)

Key invariants:

1. `providers` must be non-empty.
2. `id` values must be unique.
3. `len(backoff_ms) == max_attempts` when restart policy is enabled.

## `polling`

Purpose: state-cache polling cadence.

Fields:

1. `interval_ms` (int, lower-bounded)

## `telemetry`

Purpose: optional runtime telemetry export configuration.

Fields:

1. `enabled` (bool)
2. `influxdb` object:
   - `url`, `org`, `bucket`, `token`
   - `batch_size`, `flush_interval_ms`
   - `max_retry_buffer_size`
3. `health_interval_ms` (int, default 15000, `0` disables): cadence of
   provider/device health snapshot ingestion into InfluxDB
   (`anolis_provider_health` / `anolis_device_health`). Each snapshot performs
   a live provider GetHealth round-trip (a provider may answer with a live
   device query per device), so raise/lower deliberately.

Compatibility aliases (deprecated but accepted):

1. Flat keys under `telemetry.*`:
   - `influx_url`, `influx_org`, `influx_bucket`, `influx_token`
   - `batch_size`, `flush_interval_ms`

## `health`

Purpose: device-health liveness staleness policy and signal exclusions.

Fields:

1. `staleness` object (all `0` = derive from cadence):
   - `warn_after_ms` (int, `>= 0`, default `0`): absolute age at which liveness
     health becomes `WARNING`. `0` derives `max(3 x polling.interval_ms x device_count, 2000)`.
   - `stale_after_ms` (int, `>= 0`, default `0`): absolute age at which liveness
     health becomes `STALE`. `0` derives `max(8 x polling.interval_ms x device_count, 5000)`.

   When both overrides are positive, `stale_after_ms` must be `> warn_after_ms`. The
   derivation keeps the historic `2000`/`5000` behavior at one device @ 500ms while
   letting a serialized multi-device bus loosen proportionally instead of false-flapping.

2. `excluded_signals` array (optional, default empty). Each entry:
   - `device_handle` (string, required): `provider_id/device_id`. Listed at most
     once.
   - `signals` (array of non-empty strings, required, at least one): signal ids
     left out of that device's health rollup. They are still polled, served and
     sent to telemetry.

   A handle or signal id that matches nothing after discovery is logged as a
   warning at startup.

## `logging`

Purpose: runtime log verbosity.

Fields:

1. `level` in `debug|info|warn|error`

## `automation`

Purpose: behavior-tree runtime wiring and parameter declaration.

Fields:

1. `enabled` (bool)
2. `behavior_tree` (string; required when enabled)
3. `tick_rate_hz` (int)
4. `manual_gating_policy` (`BLOCK|OVERRIDE`)
5. `parameters` (array)
6. `mode_transition_hooks` (optional object)

Parameter entry fields:

1. `name`
2. `type` (`double|int64|bool|string`)
3. `default`
4. `min` / `max` (numeric types only)
5. `allowed_values` (string type only)

`mode_transition_hooks` fields:

1. `before_transition` (array of hook objects)
2. `after_transition` (array of hook objects)

Compatibility alias:

1. `behavior_tree_path` (accepted alias for `behavior_tree`).

## `safety`

Declares the software safe-state consumed by `POST /v0/estop`. Parsed
independently of `automation`, so a pure-manual machine can declare one.

`safety.safe_state` fields (all optional):

1. `hooks` (array of call objects) - run first when present.
2. `setpoints` (array of call objects) - per-actuator safe setpoints; applied
   only when they cover every actuating output (fail closed).
3. `zero_is_safe` (boolean, default `false`) - permits zeroing all actuating
   outputs when neither hooks nor full-coverage setpoints apply. Only
   numeric required arguments are zeroed. A function with a required boolean,
   string or bytes argument is not called and is reported as not driven:
   the runtime cannot know which boolean value is safe.

`safety.on_startup` (`safe_state` or `leave`, default `safe_state`) - what
happens to outputs at startup. `safe_state` runs `safety.safe_state` once,
after provider discovery and before the HTTP server opens, with the same
ladder and verdict as `POST /v0/estop` but without engaging the latch.
`leave` leaves outputs as they were. Any other value fails config load.

`safety.stop_chain` - observing the emergency stop
([rfcs/stop-chain.md](rfcs/stop-chain.md)). The runtime only observes it, and
no stop's effect depends on it. Absent means no stop is observed.

```yaml
safety:
  stop_chain:
    status:
      signal: gpio0/estop/chain_open  # provider/device/signal, a bool
      active: true                    # the value meaning "stop in force" (default true)
    devices:                          # provider/device handles in scope
      - bread0/rlht0                  # held; no expectation
      - handle: bread0/dcmt0
        expect: reports_estop         # stays on the bus, reports estop=true
      - handle: relay0/heater0
        expect: power_loss            # its controller loses power: goes silent
```

1. The status reads `in_force`, `clear` or `unknown`. A signal that is stale
   (three poll intervals old, at least one second, or reported with non-OK
   quality), unregistered or not a bool is `unknown`, never `clear`.
2. On `in_force` or `unknown`, at startup after the first poll or at any
   time after, the software stop's latch engages without running its ladder,
   and an automation machine enters `FAULT`. No device calls are sent.
3. While `in_force` or `unknown`, `POST /v0/estop/clear` is refused. Releasing
   the stop never clears the latch: the operator clears it once it reads
   `clear`.
4. A device in `devices` that goes unreachable while the stop is in force is
   logged as the stop, not as a device fault, and listed under
   `stop_chain.expected_losses` in `GET /v0/runtime/status`.
5. `safety.on_startup: safe_state` with the stop in force at boot skips the
   calls to devices in `devices`, reported in `startup_safe_state.skipped_actions`
   with the reason `stop chain in force`, and still drives the devices outside
   the scope, which are powered. With the status `unknown` it runs in full: the
   status source may be down while the devices are powered.
6. At startup the runtime warns when the status device is itself in
   `devices`, is not registered, has actuating functions, or its signal has no
   poll hint (it is then never polled, and the status reads `unknown`).
7. A device's optional `expect` says what it does when the stop is in force.
   Use `power_loss` only when the stop removes power from the device's
   controller itself; a device whose controller stays powered while its
   output is cut is `reports_estop`, or has no `expect` if it has no `estop`
   signal. A bread DCMT or RLHT behind a stop that cuts only the motor and
   relay paths stays on the bus and reports `estop`.
   Once the stop has been in force for the status signal's stale bound, a
   `power_loss` device that still has a fresh reading of any signal, or a
   `reports_estop` device whose `estop` signal reads a fresh `false`, raises
   "Stop not reaching *device*": a WARN in the log, an entry in
   `stop_chain.mismatches` in `GET /v0/runtime/status`, and a
   `stop_chain_mismatch` event. It is resolved, with another event, when the
   device behaves as expected or the stop is released. The hold is the same
   either way. A `reports_estop` device with no fresh `estop` reading does not
   raise it: it may have lost power. A device with no `expect` never raises
   it. At startup the runtime warns when a registered `reports_estop` device
   has no polled bool `estop` signal, or a `power_loss` device has no polled
   signal: a mismatch is then never raised for it.

A `status` without `signal`, a handle of the wrong shape, a `devices` mapping
without `handle`, or an `expect` other than `power_loss` or `reports_estop`
fails config load.

Call objects use the same shape as `mode_transition_hooks` calls
(`device_handle`, `function_id` or `function_name`, `args`). Each call requires
`device_handle` and a function selector. When none of the three apply, the
runtime reports `software_safe_state: "none"` and latches without actuating.

## Validation Model

Use both layers together:

1. Schema-level structure validation.
2. Runtime load-time semantic validation.

Example commands:

```bash
python3 tests/contracts/runtime-config/validate-runtime-configs.py
anolis-runtime --check-config --config examples/anolis-runtime.yaml
```

## Change Rule

1. Runtime loader behavior (`yaml-cpp`) is authoritative when parser behavior differs across toolchains.
2. Contract/schema updates must stay synchronized with runtime semantics and tests.
