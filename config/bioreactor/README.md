# Bioreactor Validation Profiles

This directory is the canonical application runtime profile for the current
bioreactor lab stack.

Scope:

1. Manual/open-loop and telemetry validation profiles.
2. Optional automation profile for stir + feed scheduling.
3. Three runtime variants:
   - `manual` with telemetry disabled.
   - `telemetry` with InfluxDB sink enabled.
   - `automation` for stir + feed BT control.
   - `full` for combined automation + telemetry.

Device map:

1. `bread0`:
   - `rlht0` at `0x0A`
   - `dcmt0` at `0x14`
   - `dcmt1` at `0x15`
2. `ezo0`:
   - `ph0` at `0x63`
   - `do0` at `0x61`

DCMT channel intent map:

1. `dcmt0` (`0x14` / decimal 20):
   - channel 1: sample pump motor
   - channel 2: impeller stirring motor
2. `dcmt1` (`0x15` / decimal 21):
   - channel 1: dosing pump 1
   - channel 2: dosing pump 2

RLHT usage note:

1. `rlht0` (`0x0A`) is currently used for vessel/culture temperature sensing only.
2. Relay channels are not used in the current bioreactor setup.

## Files

1. `anolis-runtime.bioreactor.manual.yaml`
2. `anolis-runtime.bioreactor.telemetry.yaml`
3. `anolis-runtime.bioreactor.automation.yaml`
4. `anolis-runtime.bioreactor.full.yaml`
5. `provider-bread.bioreactor.yaml`
6. `provider-ezo.bioreactor.yaml`
7. `telemetry-export.bioreactor.yaml`
8. `../../behaviors/bioreactor_stir_dual_dosing.xml`

Automation naming convention:

1. Use descriptive profile names by capability, not rollout step labels.
2. Current pattern is `anolis-runtime.bioreactor.automation.yaml`.
3. Future variants should follow explicit capability naming (for example `automation.ph-dosing`).

## Build Prerequisites

Build all three repos before validation so runtime config executable paths exist.
If you need first-time host setup (Ninja, vcpkg, compiler toolchain), follow
`docs/getting-started.md` in each repo first.

Linux packages required by this runbook:

```bash
sudo apt-get update
sudo apt-get install -y curl jq
```

```bash
cd /path/to/anolis
cmake --preset dev-release
cmake --build --preset dev-release

cd /path/to/anolis-provider-bread
cmake --preset dev-linux-hardware-release
cmake --build --preset dev-linux-hardware-release

cd /path/to/anolis-provider-ezo
cmake --preset dev-linux-hardware-release
cmake --build --preset dev-linux-hardware-release
```

## Linux Hardware Baseline

Start runtime:

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config ./config/bioreactor/anolis-runtime.bioreactor.manual.yaml
```

Capture baseline artifacts:

```bash
cd /path/to/anolis
./config/mixed-bus-providers/check_mixed_bus_http.sh \
  --base-url http://127.0.0.1:8080 \
  --expect-providers bread0,ezo0 \
  --min-device-count 5 \
  --capture-dir artifacts/mixed-bus-validation/bioreactor
```

Acceptance:

1. `bread0` and `ezo0` are `AVAILABLE`.
2. Runtime reports 5 devices.
3. Health is stable/OK for `rlht0`, `dcmt0`, `dcmt1`, `ph0`, and `do0`.
4. Artifacts exist in `artifacts/mixed-bus-validation/bioreactor`.

## Automation (Stir + Feed)

This profile enables Behavior Tree automation for:

1. `dcmt0` channel 2 impeller command.
2. `dcmt1` channel 1 independent dosing schedule (`dose1_*` parameters).
3. `dcmt1` channel 2 independent dosing schedule (`dose2_*` parameters).
4. `dcmt0` channel 1 sample pump held at `0` in automation mode (manual-only for now).
5. Safe mode-transition handoff writes (`AUTO->MANUAL`, `MANUAL->IDLE`, `*->FAULT`).

PWM parameter guardrails:

1. `impeller_pwm`, `dose1_pwm`, and `dose2_pwm` use Nano-compatible open-loop bounds: `0..255`.
2. Bioreactor automation intentionally keeps these non-negative (forward-only dosing/stir).

Safety behavior note:

1. Transition hooks targeting `FAULT` run best-effort; hook failures are logged.
2. `FAULT` entry is not blocked by hook failures.

Start runtime:

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config ./config/bioreactor/anolis-runtime.bioreactor.automation.yaml
```

Recommended first validation:

1. Keep defaults (`impeller_enable=false`, `dose1_enable=false`, `dose2_enable=false`).
2. Set `impeller_enable=true` and `impeller_pwm` via `POST /v0/parameters`.
3. Confirm `dcmt0` channel 2 responds and channel 1 remains off.
4. Set `dose1_enable=true` and tune `dose1_interval_s` / `dose1_pulse_s`.
5. Confirm `dcmt1` channel 1 pulses on schedule.
6. Optionally set `dose2_enable=true` and tune `dose2_interval_s` / `dose2_pulse_s`.
7. Confirm `dcmt1` channel 2 pulses independently from channel 1.

Capture automation API artifacts:

```bash
cd /path/to/anolis
./config/mixed-bus-providers/check_mixed_bus_http.sh \
  --base-url http://127.0.0.1:8080 \
  --expect-providers bread0,ezo0 \
  --min-device-count 5 \
  --capture-dir artifacts/mixed-bus-validation/bioreactor-automation
```

Automation acceptance:

1. Runtime and providers remain `AVAILABLE`.
2. Inventory remains 5 devices while automation runs.
3. Artifact capture passes during active stir/feed control.

## Full Runtime (Automation + Telemetry)

This is the canonical end-state profile combining:

1. bioreactor provider map (`bread0` + `ezo0`)
2. telemetry sink (`InfluxDB`)
3. stir/feed behavior tree automation

Start runtime with combined profile:

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config ./config/bioreactor/anolis-runtime.bioreactor.full.yaml
```

Acceptance:

1. Runtime starts with both telemetry and automation enabled.
2. Provider inventory remains stable at 5 devices.
3. Stir/feed control behaves as expected while telemetry points are written.

Capture API artifacts for full profile:

```bash
cd /path/to/anolis
./config/mixed-bus-providers/check_mixed_bus_http.sh \
  --base-url http://127.0.0.1:8080 \
  --expect-providers bread0,ezo0 \
  --min-device-count 5 \
  --capture-dir artifacts/mixed-bus-validation/bioreactor-full
```

Optional Influx sanity query for the full profile runtime name:

```bash
curl -sS --request POST "http://localhost:8086/api/v2/query?org=anolis" \
  --header "Authorization: Token dev-token" \
  --header "Accept: application/csv" \
  --header "Content-type: application/vnd.flux" \
  --data 'from(bucket:"anolis") |> range(start: -5m) |> filter(fn:(r) => r._measurement == "anolis_signal") |> filter(fn:(r) => r.runtime_name == "bioreactor-full") |> limit(n: 1)'
```

## Linux Hardware Baseline With Telemetry

Run the manual baseline above first, then layer the observability stack on top.

On Raspberry Pi OS / Debian, the simplest working path is the distro packages:

```bash
sudo apt-get update
sudo apt-get install -y docker.io docker-compose
docker --version
docker-compose --version
sudo systemctl status docker --no-pager || sudo systemctl start docker
sudo docker run hello-world
```

If your user is not yet configured for Docker access, prefix the `docker`
commands below with `sudo`.

Start observability stack:

```bash
cd /path/to/anolis/tools/docker
[ -f .env ] || cp .env.example .env
docker-compose -f docker-compose.observability.yml up -d
docker-compose -f docker-compose.observability.yml ps
```

Token alignment requirement:

1. The runtime telemetry profile uses Influx token `dev-token`.
2. The default stack also expects `INFLUXDB_ORG=anolis`,
   `INFLUXDB_BUCKET=anolis`, and `INFLUXDB_TOKEN=dev-token`.
3. If `tools/docker/.env` already exists, verify those values before startup
   instead of overwriting the file.
4. If you change the token, update
   `anolis-runtime.bioreactor.telemetry.yaml` to match.

Service endpoints:

1. InfluxDB: `http://localhost:8086`
2. Grafana: `http://localhost:3001`
3. Grafana login: `admin / anolis123`
4. Port `3000` remains reserved for the operator UI, not Grafana.

Stop the manual-profile runtime before starting the telemetry-profile runtime.
Both profiles bind the runtime HTTP server to `127.0.0.1:8080`.

Start runtime with telemetry profile:

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config ./config/bioreactor/anolis-runtime.bioreactor.telemetry.yaml
```

Capture API baseline artifacts:

```bash
cd /path/to/anolis
./config/mixed-bus-providers/check_mixed_bus_http.sh \
  --base-url http://127.0.0.1:8080 \
  --expect-providers bread0,ezo0 \
  --min-device-count 5 \
  --capture-dir artifacts/mixed-bus-validation/bioreactor-telemetry
```

Telemetry acceptance:

1. Runtime and providers remain `AVAILABLE` during telemetry writes.
2. `check_mixed_bus_http.sh` passes and captures artifacts.
3. InfluxDB is reachable at `http://localhost:8086`.
4. Grafana is reachable at `http://localhost:3001`.
5. Telemetry points are ingested in InfluxDB:

If you changed the Influx token in `tools/docker/.env` and
`anolis-runtime.bioreactor.telemetry.yaml`, replace `dev-token` below with the
same token value.

```bash
curl -sS --request POST "http://localhost:8086/api/v2/query?org=anolis" \
  --header "Authorization: Token dev-token" \
  --header "Accept: application/csv" \
  --header "Content-type: application/vnd.flux" \
  --data 'from(bucket:"anolis") |> range(start: -5m) |> filter(fn:(r) => r._measurement == "anolis_signal") |> filter(fn:(r) => r.runtime_name == "bioreactor-telemetry") |> limit(n: 1)'
```

Expected: output includes at least one data row (not only CSV headers).

## Telemetry Export MVP Service

This service is the production-MVP export path. It is intentionally external to
`anolis-runtime` control-plane APIs.

Start export service:

```bash
cd /path/to/anolis
# Optional secret overrides (preferred in shared/prod-like environments):
# export ANOLIS_EXPORT_AUTH_TOKEN='...'
# export ANOLIS_EXPORT_INFLUX_TOKEN='...'
python3 -m tools.telemetry_export.export_service --config config/bioreactor/telemetry-export.bioreactor.yaml
```

Health check:

```bash
curl -sS http://127.0.0.1:8091/v1/health
```

Raw-event query (`json` response):

```bash
curl -sS -X POST "http://127.0.0.1:8091/v1/exports/signals:query" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer export-dev-token" \
  -H "X-Requester-Id: operator-ui" \
  -d '{
    "time_range": {
      "start": "2026-04-01T00:00:00Z",
      "end": "2026-04-01T00:30:00Z"
    },
    "selector": {
      "runtime_names": ["bioreactor-telemetry"],
      "provider_ids": ["bread0", "ezo0"],
      "device_ids": ["rlht0", "dcmt0", "dcmt1", "ph0", "do0"]
    },
    "resolution": {
      "mode": "raw_event"
    },
    "format": "json"
  }'
```

Downsampled query (`csv` response):

```bash
curl -sS -D /tmp/export.headers \
  -o /tmp/export.csv \
  -X POST "http://127.0.0.1:8091/v1/exports/signals:query" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer export-dev-token" \
  -d '{
    "time_range": {
      "start": "2026-04-01T00:00:00Z",
      "end": "2026-04-01T00:30:00Z"
    },
    "resolution": {
      "mode": "downsampled",
      "interval": "10s",
      "aggregation": "last"
    },
    "format": "csv"
  }'
```

Fetch the CSV manifest by export id:

```bash
export EXPORT_ID=$(grep -i '^X-Export-Id:' /tmp/export.headers | awk '{print $2}' | tr -d '\r')
curl -sS "http://127.0.0.1:8091/v1/exports/manifests/${EXPORT_ID}" \
  -H "Authorization: Bearer export-dev-token"
```

Raw-event query (`ndjson` response):

```bash
curl -sS -X POST "http://127.0.0.1:8091/v1/exports/signals:query" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer export-dev-token" \
  -d '{
    "time_range": {
      "start": "2026-04-01T00:00:00Z",
      "end": "2026-04-01T00:30:00Z"
    },
    "resolution": {
      "mode": "raw_event"
    },
    "format": "ndjson"
  }'
```

Programmatic example:

```bash
python3 tools/telemetry_export/examples/query_signals.py \
  --start 2026-04-01T00:00:00Z \
  --end 2026-04-01T00:30:00Z \
  --provider bread0 \
  --provider ezo0 \
  --format json
```

MVP constraints:

1. Dataset support is `signals` only.
2. Auth is mandatory (`Authorization: Bearer ...`).
3. Completeness is best-effort under current telemetry overflow behavior.
4. `bytes` vs `string` fidelity remains a known MVP limitation.
5. `X-Request-Id` is always emitted; `X-Requester-Id` is optional.
6. Export responses include `X-Export-Id` and `X-Export-Manifest-Hash`; full
   manifest payload is available in JSON responses and at
   `GET /v1/exports/manifests/{export_id}`.
7. Optional selector scope enforcement can return `403 permission_denied`.
8. Selector supports optional `runtime_names` for multi-runtime disambiguation.
9. `timezone` request input is not supported in v1 (timestamps are always UTC).
10. In downsample mode:

- numeric fields use requested aggregation (`last|mean|min|max|count`);
- `value_bool`, `value_string`, and `quality` use `last`;
- requesting `value_bool`/`value_string` columns with non-`last` aggregation returns `400 invalid_argument`.

11. `json`, `csv`, and `ndjson` all use a bounded-memory spool-to-file path
    before response streaming.
12. `limits.max_response_bytes` applies to `format=json`; streamed `csv` and
    `ndjson` are controlled by `max_rows` and request limits.

Stop observability stack when finished:

```bash
cd /path/to/anolis/tools/docker
docker-compose -f docker-compose.observability.yml down
```

## Operator UI Manual Workflow

Requires Python 3 on the lab machine.

Start UI server:

```bash
cd /path/to/anolis/tools/operator-ui
./serve.sh
```

Open `http://127.0.0.1:3000` and validate:

1. All 5 devices are visible.
2. Per-device state updates continuously.
3. Function calls can be issued manually.
4. Automation panel remains graceful while automation is disabled.

## Notes

1. Bosch sensor checks remain outside provider mixed-bus scope.
2. Ownership-conflict negative testing lives in `config/mixed-bus-providers/`.
3. `anolis-runtime.bioreactor.telemetry.yaml` uses `token: dev-token` by default (same as `tools/docker/.env.example`).
