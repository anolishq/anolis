# Runtime Configuration

Anolis runtime is configured with YAML.

Contract authority is machine-validated:

1. `schemas/runtime/runtime-config.schema.json`
2. `tests/contracts/runtime-config/validate-runtime-configs.py`
3. `anolis-runtime --check-config` (semantic/load-time checks)

Use this document for authoring workflow and operational guidance.

## Where Runtime YAML Lives

1. Checked-in examples: `examples/anolis-runtime*.yaml`
2. Machine realization configs: `anolis-projects/projects/{name}/config/anolis-runtime*.yaml`
3. Commissioning-generated outputs are managed in
   [`anolis-workbench`](https://github.com/anolishq/anolis-workbench)
   and are intentionally not tracked in this repository.

Out of scope for this contract:

1. Provider-local YAML (`provider-*.yaml`)
2. Telemetry-export tool YAML (`telemetry-export*.yaml`)

## Authoring Workflow

1. Start from a known profile (`manual`, `telemetry`, `automation`, `full` variants when available).
2. Edit runtime concerns only in runtime YAML.
3. Validate schema fixtures:
   - `python3 tests/contracts/runtime-config/validate-runtime-configs.py`
4. Validate runtime semantics:
   - `anolis-runtime --check-config --config <path>`
5. Run runtime normally after both checks pass.

## Minimal Runtime Skeleton

```yaml
runtime:
  name: anolis-main

http:
  enabled: true
  bind: 127.0.0.1
  port: 8080

providers:
  - id: sim0
    command: ./build/dev-release/anolis-provider-sim

polling:
  interval_ms: 500

logging:
  level: info

telemetry:
  enabled: false

automation:
  enabled: false
```

## Operational Notes

### Runtime modes

1. Startup mode is always `IDLE` (not YAML-configurable).
2. Mode changes happen at runtime via `POST /v0/mode`.
3. `IDLE` blocks control operations; `AUTO` applies manual-call gating policy.
4. `FAULT` is a recovery mode with constrained transitions (`FAULT -> MANUAL` only).
5. See [automation.md](automation.md) for full mode semantics.

### HTTP and CORS

1. `http.cors_allowed_origins` should be explicit in non-dev environments.
2. Wildcard origin (`*`) cannot be combined with `cors_allow_credentials: true`.

### Providers and supervision

1. Provider IDs must be unique.
2. Provider command is required.
3. Restart policy is optional but recommended for hardware processes.

### Telemetry

1. Telemetry is off by default.
2. Use nested `telemetry.influxdb.*` settings.
3. Flat `telemetry.influx_*` keys remain accepted for compatibility, but are deprecated.
4. When telemetry is enabled, provider/device health is also ingested every
   `telemetry.health_interval_ms` (default 15000; `0` disables).

### Health staleness

Device-health liveness (`OK`/`WARNING`/`STALE` in `/v0/providers/health` and the
`anolis_device_health` timeseries) is derived from *time since the last successful
poll*. The `WARNING`/`STALE` thresholds are **cadence-derived** by default so a
healthy but serialized bus does not false-flap:

- `warn_after_ms  = max(3 x polling.interval_ms x device_count, 2000)`
- `stale_after_ms = max(8 x polling.interval_ms x device_count, 5000)`

At the default `interval_ms: 500` with a single device this is exactly `2000` /
`5000` (unchanged from prior releases). A multi-device serialized bus (e.g. an EZO
chain where a full poll cycle exceeds 2s) gets proportionally looser bounds and
stops flapping. Supply absolute overrides only when the heuristic does not fit:

```yaml
health:
  staleness:
    warn_after_ms: 0    # 0 (default) => derive from cadence; a positive value overrides
    stale_after_ms: 0   # 0 (default) => derive; when both set, must be > warn_after_ms
```

Beyond liveness, device health also folds in **per-signal quality and freshness**:
a device serving a cached `QUALITY_FAULT` sample reads `FAULT`, and a signal of
degraded quality (`QUALITY_STALE`/`QUALITY_UNKNOWN`) reads `STALE` — even when the
poll itself is fresh (this is the false-green the hardcoded liveness check missed).
The per-signal *age* check is deliberately conservative: it uses
`max(stale_after_ms, <liveness stale bound>)`, so it never flags `STALE` before the
cadence-derived liveness bound and therefore cannot re-introduce the false-STALE
storm — a provider's declared `stale_after_ms` (from its capability descriptor) only
*extends* trust, and an unset value simply inherits the liveness bound. The age path
adds `STALE` only for a genuinely stuck/old cached sample (signal age far exceeding
poll age). Per-device precedence is `UNAVAILABLE > FAULT > STALE > WARNING > OK`, and
the `stale_signal_count` / `fault_signal_count` fields on `/v0/providers/health`
explain the result.

### Compatibility behavior

1. Unknown keys are warn-and-ignore for forward compatibility.
2. `automation.behavior_tree_path` is an accepted alias for `automation.behavior_tree`.
3. `runtime.mode` in YAML is rejected.

## Related References

1. [configuration-schema.md](configuration-schema.md) - compact section-by-section schema narrative.
2. [docs/contracts/runtime-config-baseline.md](contracts/runtime-config-baseline.md) - locked behavior baseline.
3. [schemas/README.md](https://github.com/anolishq/anolis/blob/main/schemas/README.md) - canonical schema and validator map.
