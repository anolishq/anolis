# Anolis Observability Stack

This directory contains Docker configuration for the Anolis telemetry stack:

- **InfluxDB 2.9**: Time-series database for signal history
- **Grafana 13.1.0**: Visualization dashboards

## Quick Start

### 1. Start the Stack

```bash
# Create .env from template (optional - defaults work for local dev)
cp .env.example .env

# Start containers
docker-compose -f docker-compose.observability.yml up -d

# Verify containers are running
docker-compose -f docker-compose.observability.yml ps
```

### 2. Access Services

| Service  | URL                     | Credentials       |
| -------- | ----------------------- | ----------------- |
| InfluxDB | <http://localhost:8086> | admin / anolis123 |
| Grafana  | <http://localhost:3001> | admin / anolis123 |

### 3. Run Anolis with Telemetry

```bash
# From anolis root directory
./build/core/Release/anolis-runtime.exe anolis-runtime-telemetry.yaml
```

### 4. View Dashboards

1. Open <http://localhost:3001> (Grafana)
2. Login with admin / anolis123
3. Navigate to **Dashboards** → **Anolis**
4. Select **Signal History** or **Device Health**

## Configuration

### Environment Variables

Copy `.env.example` to `.env` and customize:

| Variable          | Default   | Description             |
| ----------------- | --------- | ----------------------- |
| INFLUXDB_USERNAME | admin     | InfluxDB admin user     |
| INFLUXDB_PASSWORD | anolis123 | InfluxDB admin password |
| INFLUXDB_ORG      | anolis    | InfluxDB organization   |
| INFLUXDB_BUCKET   | anolis    | InfluxDB bucket name    |
| INFLUXDB_TOKEN    | dev-token | API token for writes    |
| GRAFANA_PASSWORD  | anolis123 | Grafana admin password  |

### Runtime Config

In your `anolis-runtime.yaml`, enable telemetry:

```yaml
telemetry:
  enabled: true
  influxdb:
    url: http://localhost:8086
    org: anolis
    bucket: anolis
    token: dev-token # Or use INFLUXDB_TOKEN env var
    batch_size: 100
    flush_interval_ms: 1000
```

## Dashboards

All bundled dashboards query bucket `anolis` directly (matching the
provisioned default); if you change `INFLUXDB_BUCKET`, the datasource will
follow but the bundled dashboard queries will not — keep the bucket named
`anolis` unless you also edit the dashboards.

### Signal History

Time-series visualization of signal values:

- **Double values**: Temperature, pressure, duty cycles
- **Boolean values**: Switch states, enabled flags
- **Integer values**: RPM, counts

Supports filtering by:

- Provider
- Device
- Signal

### Device Health

Status overview of all devices:

- **Quality status**: OK / STALE / ERROR counts
- **Quality table**: Per-signal quality status
- **Staleness gauge**: Time since last update

### I/O & Watchdog Health

Transport and safety-layer health from the runtime's health ingestion
(`telemetry.health_interval_ms`, anolis 0.1.37+; the panels are empty on
older runtimes or with ingestion disabled):

- **Status table**: latest io counters, watchdog state, missing/excluded
  flags per device
- **I/O failure rate** and **retried-attempts slope** per device — the
  retried-attempts slope is the leading indicator for bus degradation
  (rising retries while `io_failed` stays flat means the retry budget is
  masking a failing bus)
- **Watchdog**: armed state, cumulative trip count, and per-trip
  annotations derived from `watchdog_trip_count` steps
- **Provider degraded / failed-device count** — the alerting primitive for
  silent device loss
- E-stop signatures: a power-cut e-stop shows as `io_failed` accrual and
  device blackout; a signal-wired e-stop keeps the bus alive (see the
  machine-profile `safety.estop_topology` docs)

## Data Retention

The compose stack creates the `anolis` bucket with **infinite retention**
(InfluxDB's default). The native co-located install (`install.sh
--with-observability`) defaults to **30 days** (`OBSERVABILITY_RETENTION`
env at install time). To change retention afterwards (the `update`
subcommand selects by `--id`; `--name` there means *rename*):

```bash
influx bucket update \
  --id "$(influx bucket list --name anolis --hide-headers | cut -f1)" \
  --retention 30d   # or 90d, 0 (infinite), ...
```

Raising retention is a deliberate trade-off, not a free knob:

- **Disk growth and (on a Pi) SD-card wear**: the TSM engine compacts
  continuously; more retained data means more storage and more write
  amplification on flash.
- **Query cost**: long-window Flux queries over months of data get slow on
  small hosts; dashboards default to bounded windows for a reason.
- **The real fix for long experiments**: if you need months of history,
  prefer the separate-host topology (run this stack on a monitoring host
  and point the runtime's `telemetry.influxdb.url` at it) or add a
  downsampling task, rather than growing the co-located Pi bucket
  unboundedly.

Shortening retention deletes data older than the new window at the next
retention enforcement pass — export anything you need first
(`anolis-telemetry-export`).

## Data Schema

Health measurements (`anolis_provider_health`, `anolis_device_health`) are
documented in `docs/contracts/telemetry-health-timeseries-baseline.md` in
the anolis repository (also shipped in the
`anolis-<version>-telemetry-schema.tar.gz` release artifact — not inside
this observability tarball); the signal measurement is below.

InfluxDB measurement: `anolis_signal`

**Tags**:

- `provider_id`: Provider identifier (e.g., "sim0")
- `device_id`: Device identifier (e.g., "tempctl-0")
- `signal_id`: Signal identifier (e.g., "temperature")

**Fields**:

- `value_double`: Double values
- `value_int`: Integer values
- `value_bool`: Boolean values
- `quality`: Signal quality ("OK", "STALE", etc.)

**Example line protocol**:

```flux
anolis_signal,provider_id=sim0,device_id=tempctl-0,signal_id=temperature value_double=23.5,quality="OK" 1706960096000
```

## Troubleshooting

### InfluxDB Not Starting

Check health status:

```bash
docker-compose -f docker-compose.observability.yml logs influxdb
```

### No Data in Grafana

1. Verify runtime is writing to InfluxDB:

   ```text
   [InfluxSink] Written 1000 events to InfluxDB
   ```

2. Check InfluxDB data explorer:
   - Open <http://localhost:8086>
   - Go to Data Explorer
   - Query `anolis_signal` measurement

3. Verify Grafana datasource connection:
   - Settings → Data Sources → InfluxDB
   - Click "Test" button

### Dashboards Not Appearing

Dashboards are auto-provisioned from `../grafana/dashboards/`.
If not appearing:

```bash
docker-compose -f docker-compose.observability.yml restart grafana
```

## Cleanup

```bash
# Stop containers
docker-compose -f docker-compose.observability.yml down

# Remove volumes (deletes all data)
docker-compose -f docker-compose.observability.yml down -v
```
