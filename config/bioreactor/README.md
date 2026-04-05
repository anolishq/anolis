# Bioreactor Validation Profiles

This directory is the canonical application runtime profile for the current
bioreactor lab stack.

Scope:

1. Manual/open-loop validation only.
2. No automation in either profile.
3. Two runtime variants:
   - `manual` with telemetry disabled.
   - `telemetry` with InfluxDB sink enabled.

Device map:

1. `bread0`:
   - `rlht0` at `0x0A`
   - `dcmt0` at `0x14`
   - `dcmt1` at `0x15`
2. `ezo0`:
   - `ph0` at `0x63`
   - `do0` at `0x61`

## Files

1. `anolis-runtime.bioreactor.manual.yaml`
2. `anolis-runtime.bioreactor.telemetry.yaml`
3. `provider-bread.bioreactor.yaml`
4. `provider-ezo.bioreactor.yaml`

## Build Prerequisites

Build all three repos before validation so runtime config executable paths exist.

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

## Linux Hardware Baseline With Telemetry

Start observability stack:

```bash
cd /path/to/anolis/tools/docker
cp .env.example .env
docker compose -f docker-compose.observability.yml up -d
```

Token alignment requirement:

1. The runtime telemetry profile uses Influx token `dev-token`.
2. Ensure `tools/docker/.env` keeps `INFLUXDB_TOKEN=dev-token`, or update
   `anolis-runtime.bioreactor.telemetry.yaml` to match your `.env` token.

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

```bash
curl -sS --request POST "http://localhost:8086/api/v2/query?org=anolis" \
  --header "Authorization: Token dev-token" \
  --header "Accept: application/csv" \
  --header "Content-type: application/vnd.flux" \
  --data 'from(bucket:"anolis") |> range(start: -5m) |> filter(fn:(r) => r._measurement == "anolis_signal") |> limit(n: 1)'
```

Expected: output includes at least one data row (not only CSV headers).

## Operator UI Manual Workflow

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
