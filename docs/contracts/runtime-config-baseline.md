# Runtime Config Baseline

Status: Locked.

## Purpose

Freeze runtime YAML behavior used by `anolis-runtime --config` and `--check-config` so schema and validator changes stay aligned with implementation.

## Canonical Artifacts

1. Runtime model/defaults: `core/runtime/config.hpp`
2. Runtime loader/validation: `core/runtime/config.cpp`
3. CLI semantic gate: `core/src/main.cpp` (`--check-config`)
4. Runtime schema: `schemas/runtime/runtime-config.schema.json`
5. Contract validator: `tools/contracts/validate-runtime-configs.py`
6. Contract fixtures: `tests/contracts/runtime-config/`
7. Unit coverage: `tests/unit/config_test.cpp`

## Locked Behavior Summary

### Scope

1. Applies to runtime YAML consumed by `anolis-runtime`.
2. Targets `anolis-runtime*.yaml` under `config/`.
3. Excludes provider-local YAML and telemetry-export YAML.

### Supported Top-Level Sections

1. `runtime`
2. `http`
3. `providers`
4. `polling`
5. `telemetry`
6. `logging`
7. `automation`

### Compatibility Commitments

1. Unknown keys are warn-and-ignore (forward compatibility).
2. Accepted deprecated aliases:
   - `automation.behavior_tree_path`
   - flat telemetry keys under `telemetry.*` (`influx_*`, `batch_size`, `flush_interval_ms`)
3. Rejected legacy key: `runtime.mode`.
4. Runtime parser behavior (`yaml-cpp`) is authoritative for semantic edge cases.

### Key Validation Invariants

1. Runtime timeout bounds:
   - `500 <= shutdown_timeout_ms <= 30000`
   - `5000 <= startup_timeout_ms <= 300000`
2. HTTP invariants:
   - valid port range, `thread_pool_size >= 1`
   - non-empty CORS origins
   - wildcard origin forbidden when credentials are enabled
3. Providers:
   - non-empty provider list, unique IDs, required command
   - timeout lower bounds (`timeout_ms`, `hello_timeout_ms`, `ready_timeout_ms`)
4. Restart policy:
   - `max_attempts >= 1`
   - `backoff_ms` length equals `max_attempts`
5. Polling/logging validity:
   - `polling.interval_ms >= 100`
   - logging level in `debug|info|warn|error`
6. Automation:
   - behavior tree path required when enabled
   - `tick_rate_hz` bounded
   - parameter type/default consistency
   - mode-transition-hook structure and enum validation

## Validation Gates

Run both layers:

1. Schema and fixture validation:
   - `python3 tools/contracts/validate-runtime-configs.py`
2. Runtime semantic validation:
   - `anolis-runtime --check-config --config <runtime-yaml>`

CI must keep these green together.

## Drift Notes and Change Rule

1. Do not change schema-only behavior without matching runtime loader semantics.
2. Do not change runtime loader semantics without updating schema/tests/baseline in the same change.
3. Additive compatibility is preferred; tightening validation requires explicit migration notes.
