# Runtime Configuration Schema (Narrative)

This is a compact human map of runtime YAML sections.

Authoritative contract behavior comes from:

1. `schemas/runtime/runtime-config.schema.json`
2. `tools/contracts/validate-runtime-configs.py`
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

Compatibility aliases (deprecated but accepted):

1. Flat keys under `telemetry.*`:
   - `influx_url`, `influx_org`, `influx_bucket`, `influx_token`
   - `batch_size`, `flush_interval_ms`

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

## Validation Model

Use both layers together:

1. Schema-level structure validation.
2. Runtime load-time semantic validation.

Example commands:

```bash
python3 tools/contracts/validate-runtime-configs.py
anolis-runtime --check-config --config config/anolis-runtime.yaml
```

## Change Rule

1. Runtime loader behavior (`yaml-cpp`) is authoritative when parser behavior differs across toolchains.
2. Contract/schema updates must stay synchronized with runtime semantics and tests.
