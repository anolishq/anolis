# Anolis Observability Stack

This directory contains Docker configuration for the Anolis telemetry stack:

- **InfluxDB 2.9**: Time-series database for signal history
- **Grafana 13.2.4**: Visualization dashboards

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

All bundled dashboards read the datasource's default bucket
(`v.defaultBucket`, provisioned from `INFLUX_BUCKET`), and every dashboard has
a **Runtime** filter (`runtime_name`), so two runtimes writing the same
provider, device and signal ids into one bucket stay apart.

### How the panels read change-driven data

The runtime writes an `anolis_signal` row only when a signal's value or
quality **changes**, not on every poll. The panels are built around that:

- **Latest-state panels** (status tables, quality counts, quality pie) show
  each signal's state **as of the end of the selected range**, looking back
  30 days for its last row. A steady signal that last changed before the
  range is still shown. One that last changed more than 30 days before the
  end of the range is not.
- **Only a running runtime's state is shown.** Nothing is written when a
  runtime stops, so its last rows would otherwise stand. The latest-state
  panels, and the seeds of the history panels, show a runtime's signals only
  if it wrote an `anolis_provider_health` row in the selected range. They are
  empty with health ingestion off (`telemetry.health_interval_ms: 0`) or on a
  runtime before 0.1.37, and can be for a range shorter than the health
  interval (15 s by default).
- **History panels** plot the **last value in each window** as steps, seeded
  with each signal's last value before the range. A window's value is where
  the signal ended up, not an average of its changes, which would weight each
  change rather than each second. A window with no change holds the value
  before it, so a steady signal is a flat line across the range.
- **Rates** ("changes/sec") count change rows: they measure how often values
  change, not how often the runtime polls.
- **Freshness** comes from the runtime's periodic health snapshots
  (`anolis_device_health.staleness_ms`), not from the age of a signal's last
  change. An unchanged, healthy signal has an old last change and is fresh.
- **"Seen in range" counts** count distinct ids with at least one row in the
  selected range. They are not a live inventory.

### Signal History

Signal values over time, one panel per value type:

- **Double**, **Boolean** (as 0/1), and **Integer / Unsigned** values, drawn
  as steps
- **String** values: each string signal's latest value, as of the end of the
  range

Supports filtering by runtime, provider, device and signal.

### Device Health

Per-signal quality against the locked contract (`OK`, `STALE`,
`UNAVAILABLE`, `FAULT`):

- **Signals OK / STALE / UNAVAILABLE / FAULT**: signals whose latest quality
  is each value
- **Device Signal Status**: each signal's latest quality and last change
- **Quality Distribution Over Time**: signals in each quality, carried forward
  between changes
- **Device Staleness**: the latest `staleness_ms` per device

### System Overview

- **Providers / Devices / Signals seen in range**
- **Signal changes/sec**, in the last minute and per provider over time
- **Signal Status** table and **Quality Distribution** pie, as of the end of
  the range

### Annotations

Mode and parameter changes are drawn as annotations, filtered by the
**Runtime** selection. Rows written by runtimes that predate the
`runtime_name` tag on these measurements have no runtime and are shown under
every selection.

### Testing the dashboards

`just test-observability` (needs Docker) starts a throwaway InfluxDB and
Grafana, writes fixture telemetry, and runs every panel, annotation and
variable query through Grafana, asserting what each panel shows. CI runs the
same suite in the `observability` job. Template variables (`${provider_id}`
and so on) are substituted by the suite the way Grafana's browser client
does, since Grafana's query API does not substitute them.

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

- `runtime_name`: Runtime instance (e.g., "default")
- `provider_id`: Provider identifier (e.g., "sim0")
- `device_id`: Device identifier (e.g., "tempctl-0")
- `signal_id`: Signal identifier (e.g., "temperature")

**Fields** (at most one typed value per row; none for a non-finite double):

- `value_double`, `value_int`, `value_uint`, `value_bool`, `value_string`
- `quality`: `OK`, `STALE`, `UNAVAILABLE` or `FAULT`, on every row

**Example line protocol**:

```flux
anolis_signal,runtime_name=default,provider_id=sim0,device_id=tempctl-0,signal_id=temperature value_double=23.5,quality="OK" 1706960096000
```

The locked contract is `docs/contracts/telemetry-timeseries-baseline.md`.

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
