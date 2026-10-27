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
   - `anolis-runtime --check-config <path>`
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
    command: ../anolis-provider-sim/build/dev-release/anolis-provider-sim
    args: ["--config", "../anolis-provider-sim/config/minimal.yaml"]

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

1. With `automation.enabled: true`, startup mode is always `IDLE` (not YAML-configurable).
   With automation disabled (the default) there is no mode machine: manual
   control is permitted from startup, and `/v0/mode` returns `503`.
2. Mode changes happen at runtime via `POST /v0/mode`.
3. `IDLE` blocks control operations; `AUTO` applies manual-call gating policy.
   `IDLE` does not change outputs. At startup, `safety.on_startup` does:
   `safe_state` (default) runs `safety.safe_state` once after provider
   discovery and before the HTTP server opens; `leave` leaves outputs as
   they were. See [safety.md](safety.md#outputs-at-startup).
4. `FAULT` is a recovery mode with constrained transitions (`FAULT -> MANUAL` only).
5. See [automation.md](automation.md) for full mode semantics.

### HTTP and CORS

1. `http.cors_allowed_origins` lists the browser origins allowed to call the
   API. It defaults to none, and `[]` means none. `*` allows any origin and is
   an explicit opt-in. A POST to `/v0/` from a browser origin not listed is
   refused with `403` before it is routed.
2. Wildcard origin (`*`) cannot be combined with `cors_allow_credentials: true`.

### Providers and supervision

1. Provider IDs must be unique.
2. Provider command is required.
3. Restart policy is optional but recommended for hardware processes.

### Telemetry

1. Telemetry is off by default.
2. Use nested `telemetry.influxdb.*` settings.
3. Flat `telemetry.influx_*`, `telemetry.batch_size` and `telemetry.flush_interval_ms`
   keys load without error and log a deprecation warning, but their values are ignored.
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
`5000` (unchanged from prior releases). A multi-device serialized bus (e.g. a
chain of slow sensors where a full poll cycle exceeds 2s) gets proportionally
looser bounds and stops flapping. Supply absolute overrides only when the heuristic does not fit:

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

### Signals left out of device health

A device type has a fixed signal set, but a machine may not use all of it. An
output the machine leaves disabled can read `QUALITY_UNKNOWN`, and an input
channel with nothing connected can read `QUALITY_FAULT`, on a correctly built
rig. Both would mark the device `STALE` or `FAULT`. List such signals per device
under `health.excluded_signals`:

```yaml
health:
  excluded_signals:
    - device_handle: sim0/tempctl0   # provider_id/device_id
      signals: [relay2_state]
```

Excluded signals are still polled, served by `/v0/state` and sent to telemetry.
Only the device health string and its `stale_signal_count` /
`fault_signal_count` skip them. Each `device_handle` may appear once, and needs
at least one signal. Device health is display-only: nothing in the runtime acts
on it, so an exclusion changes what is reported, not what the runtime does.

After provider discovery the runtime logs a warning for each `device_handle`
that names no registered device, and for each signal id the device does not
declare, since such an entry excludes nothing.

### Compatibility behavior

1. Unknown keys are ignored for forward compatibility, and most sections log a warning.
   Unknown keys under `polling` and `logging` are ignored with no warning, and the
   JSON schema rejects unknown keys under `safety.stop_chain`.
2. `automation.behavior_tree_path` is an accepted alias for `automation.behavior_tree`.
3. `runtime.mode` in YAML is rejected.

## Related References

1. [configuration-schema.md](configuration-schema.md) - compact section-by-section schema narrative.
2. [docs/contracts/runtime-config-baseline.md](contracts/runtime-config-baseline.md) - locked behavior baseline.
3. [schemas/README.md](https://github.com/anolishq/anolis/blob/main/schemas/README.md) - canonical schema and validator map.
