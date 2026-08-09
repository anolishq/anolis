# Telemetry Health Timeseries Baseline

Status: Locked (`v1`), additive companion to the telemetry timeseries baseline.

## Purpose

Freeze the row contracts for runtime health ingestion into InfluxDB
(anolishq/anolis#203): the `anolis_provider_health` and `anolis_device_health`
measurements written by the runtime's health snapshot task. This is the
retained/dashboardable form of the same view `GET /v0/providers/health`
serves; both derive from one collector (`core/health/health_snapshot.cpp`)
so they cannot drift.

## Canonical Artifacts

1. Machine schema: `schemas/telemetry/telemetry-health-timeseries.schema.v1.json`
2. Contract fixtures: `tests/contracts/telemetry-health-timeseries/examples/`
3. Fixture manifest: `tests/contracts/telemetry-health-timeseries/examples/manifest.yaml`
4. Contract validator: `tests/contracts/telemetry-timeseries/validate-telemetry-timeseries.py`
   (shared; pass `--manifest tests/contracts/telemetry-health-timeseries/examples/manifest.yaml`)
5. Runtime emission implementation: `core/telemetry/health_line_protocol.hpp`,
   `core/telemetry/health_snapshot_task.cpp`
6. Runtime emission unit coverage: `tests/unit/health_line_protocol_test.cpp`

## Locked Contract Summary

### `anolis_provider_health`

1. Tags (exactly): `runtime_name`, `provider_id` — non-empty strings.
2. Required fields: `state` (`AVAILABLE|UNAVAILABLE`), `lifecycle_state`
   (`RUNNING|RECOVERING|RESTARTING|CIRCUIT_OPEN|DOWN`), `degraded` (bool),
   `failed_device_count`, `device_count`, `uptime_seconds`,
   `supervision_enabled`, `attempt_count`, `crash_detected`, `circuit_open`.
3. Optional fields (present when the provider reports them):
   `startup_configured_devices`, `startup_initialized_devices`,
   `startup_failed_devices`.

### `anolis_device_health`

1. Tags (exactly): `runtime_name`, `provider_id`, `device_id` — non-empty
   strings.
2. Required fields: `health` (`OK|WARNING|STALE|FAULT|UNAVAILABLE|UNKNOWN`),
   `registered` (bool).
3. `staleness_ms`, `stale_signal_count`, and `fault_signal_count` are present
   only once the device has actually been polled: a missing/never-polled device
   must not read as 0 ms fresh with zero degraded signals. The two counts
   (integers, ≥ 0) tally cached signals that are freshness-stale (degraded
   quality or older than their effective `stale_after_ms`) vs `QUALITY_FAULT`;
   a non-zero `fault_signal_count` is why a freshly-polled device reads `FAULT`
   (#220).
4. `type_version` (string, non-empty) is present when the provider declared one
   in its inventory descriptor, and omitted entirely otherwise — never written
   empty. It carries the device's type/firmware revision so a stored result can
   be attributed to the build that produced it (bread#126). It is a **field,
   not a tag**: as a tag it would fork the series on every firmware change,
   orphaning exactly the history it exists to connect. Only this first-class
   protocol field is written; the provider's wider descriptor tag map is not,
   because those keys are provider-chosen and unbounded (it is available in
   full on `GET /v0/providers/health`).
5. Optional typed metric fields, parsed from the provider's `metrics` map
   using a strict allowlist:
   - integers: `io_ok`, `io_failed`, `io_retried_attempts`,
     `watchdog_timeout_ms`, `watchdog_trip_count`, `sample_success_count`,
     `sample_failure_count`, `call_success_count`, `call_failure_count`
   - booleans: `watchdog_armed`, `watchdog_tripped`, `missing`, `excluded`

### Allowlist rule

Provider metrics arrive as an untyped string-to-string map. Only allowlisted
keys are written, parsed strictly to their declared type (base-10 integers;
exactly `true`/`false` booleans). Unknown keys and unparseable values are
skipped, never stringly dumped — schema and series cardinality stay honest.
The allowlist follows the reserved-key vocabulary in
`anolis-provider-sdk docs/metrics.md` plus the bread watchdog keys. The
counter semantics (cumulative, monotonic per provider-process lifetime,
magnitudes not comparable across providers) are defined there.

### Tag cardinality guard

The tag key-sets are closed (`additionalProperties: false`): adding any tag
(e.g. `run_id`) is a breaking change, mirroring the `anolis_signal` rule.

### Timestamp

One snapshot timestamp (epoch milliseconds) is applied to every row of a
snapshot.

### Cadence

Controlled by `telemetry.health_interval_ms` (default 15000, `0` disables).
Each snapshot is a live ADPP GetHealth round-trip per provider — bread
answers with a live GET_WATCHDOG bus transaction per armed device — so the
cadence is a deliberate cost decision, not a free parameter.

## Compatibility Rule

1. Additive optional fields are backward-compatible.
2. Removing required keys, changing key semantics, or widening the tag
   key-set is breaking.

## Validation Gate

```bash
python3 tests/contracts/telemetry-timeseries/validate-telemetry-timeseries.py \
  --manifest tests/contracts/telemetry-health-timeseries/examples/manifest.yaml
```

The gate must remain green in CI.

## Distribution Note

Ships inside the same release artifact as the signal contract
(`anolis-<version>-telemetry-schema.tar.gz`). The `anolis_signal` v1 schema
file is untouched by this baseline; consumers pinning it by checksum are
unaffected.

## Change Rule

Update this baseline only in the same change that updates:

1. machine schema,
2. fixtures/validator expectations,
3. runtime emission behavior (if behavior changed).
