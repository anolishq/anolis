# Anolis Observability Stack

A Docker Compose stack of InfluxDB 2.9 and Grafana 13.3.0 with the Anolis
dashboards provisioned. Use it for development, or on a separate monitoring
host. It ships as the release asset `anolis-<version>-observability.tar.gz`
and lives in `observability/` in the anolis repository.

The default credentials are for local development only. Grafana also lets
anyone who can reach it view the dashboards without logging in (anonymous
Viewer role).

On a runtime host, `install.sh --with-observability` installs InfluxDB and
Grafana as native packages, with the same dashboards, instead. See
[Deploy with `install.sh`](https://github.com/anolishq/anolis/blob/main/docs/deploy/install-sh.md).

## Start the stack

Run these from the directory that holds `docker-compose.observability.yml`:

```bash
cp .env.example .env    # optional: change the defaults below
docker compose -f docker-compose.observability.yml up -d
docker compose -f docker-compose.observability.yml ps
```

Grafana waits until InfluxDB passes its health check.

| Service  | URL                     | Login             |
| -------- | ----------------------- | ----------------- |
| InfluxDB | <http://localhost:8086> | admin / anolis123 |
| Grafana  | <http://localhost:3001> | admin / anolis123 |

The dashboards are in the **Anolis** folder in Grafana. Signal History is the
home dashboard.

## Settings

`.env` sets these variables. Each one defaults to the value shown.

| Variable            | Default     | Used for                                     |
| ------------------- | ----------- | -------------------------------------------- |
| `INFLUXDB_USERNAME` | `admin`     | InfluxDB admin user                          |
| `INFLUXDB_PASSWORD` | `anolis123` | InfluxDB admin password                      |
| `INFLUXDB_ORG`      | `anolis`    | InfluxDB organization                        |
| `INFLUXDB_BUCKET`   | `anolis`    | Bucket the runtime writes and Grafana reads  |
| `INFLUXDB_TOKEN`    | `dev-token` | Admin token, used by the runtime and Grafana |
| `GRAFANA_PASSWORD`  | `anolis123` | Grafana admin password                       |

InfluxDB applies its values only on first start, when the `influxdb-data`
volume is empty. To change them later, remove the volumes (see
[Stop and clean up](#stop-and-clean-up)) and start again.

## Point the runtime at it

Set `telemetry.enabled: true` and point `telemetry.influxdb` at
`http://localhost:8086` with the org, bucket and token above.
[`examples/anolis-runtime-telemetry.yaml`](https://github.com/anolishq/anolis/blob/main/examples/anolis-runtime-telemetry.yaml)
already matches the defaults. The other telemetry keys, including
`health_interval_ms`, are in the
[configuration schema](https://github.com/anolishq/anolis/blob/main/docs/configuration-schema.md#telemetry).

With the runtime on another host, use this host's address in place of
`localhost`.

## How the panels read change-only data

Every dashboard reads the datasource's default bucket and has a **Runtime**
filter, so several runtimes can write the same provider, device and signal
ids into one bucket.

The runtime writes an `anolis_signal` row only when a signal's value or
quality changes, not on every poll. The panels are built around that:

- Latest-state panels (status tables, quality counts, the quality pie) show
  each signal's state as of the end of the selected range. They look back 30
  days for its last row, so a steady signal that last changed before the range
  is still shown. One that last changed more than 30 days before the end of
  the range is not.
- Nothing is written when a runtime stops, so its last rows would otherwise
  stand. Latest-state panels, and the starting values of the history panels,
  show a runtime only if it wrote an `anolis_provider_health` row in the
  range. They are empty when health snapshots are off
  (`telemetry.health_interval_ms: 0`), and can be empty for a range shorter
  than the health interval (15 s by default).
- History panels start each series at its last value before the range, then
  plot the last value in each window as steps. A window with no change holds
  the previous value, so a steady signal is a flat line.
- A runtime that stops inside the range still reads as running. Its last
  value and quality are held to the end of the range, and the latest-state
  panels still show it, until the stop falls before the range
  (anolishq/anolis#497).
- Rates ("changes/sec") count change rows. They show how often values change,
  not how often the runtime polls.
- Freshness comes from the health snapshots
  (`anolis_device_health.staleness_ms`), not from the age of a signal's last
  change. An unchanged, healthy signal is fresh.
- "Seen in range" counts are distinct ids with at least one row in the range,
  not a live inventory.

## Dashboards

- **Signal History**: double, boolean (as 0/1) and integer/unsigned values as
  steps, and a table of each string signal's latest value. Filters by
  runtime, provider, device and signal.
- **Device Health**: counts of signals whose latest quality is `OK`, `STALE`,
  `UNAVAILABLE` or `FAULT`, a per-signal status table, quality distribution
  over time, and the latest `staleness_ms` per device.
- **System Overview**: providers, devices and signals seen in range, signal
  changes per second, and a status table and quality pie as of the end of the
  range.
- **I/O & Watchdog Health**: per-device I/O counters, watchdog state and
  missing or excluded flags; I/O failure rate; retried-attempts slope, which
  rises before `io_failed` does when retries are hiding a failing bus;
  watchdog trip count, with an annotation at each health snapshot where a
  device's trip count rose; and provider degraded and failed-device counts.
  These panels need health snapshots. For how each kind of e-stop shows up
  here, see
  [Safety](https://github.com/anolishq/anolis/blob/main/docs/safety.md#the-machine-profile-already-names-this).

Mode changes are drawn as annotations on every dashboard, and parameter
changes on all but I/O & Watchdog Health, which draws watchdog trips instead.
Annotations follow the Runtime selection.

## Data

The runtime writes these measurements:

- `anolis_signal`, tagged `runtime_name`, `provider_id`, `device_id` and
  `signal_id`. Each row has a `quality` field and at most one typed value
  field (`value_double`, `value_int`, `value_uint`, `value_bool` or
  `value_string`).
- `anolis_device_health` and `anolis_provider_health`, the periodic health
  snapshots.
- `mode_change` and `parameter_change`, tagged `runtime_name`, which the
  annotations read.

The formats are fixed by the
[telemetry contracts](https://github.com/anolishq/anolis/blob/main/docs/contracts/README.md).

## Data retention

The compose stack creates its bucket with InfluxDB's default retention, which
keeps data forever. `install.sh --with-observability` sets 30 days instead,
from `OBSERVABILITY_RETENTION` at install time.

To change retention on an existing bucket, select it by id. In
`influx bucket update`, `--name` renames the bucket.

```bash
influx bucket update \
  --id "$(influx bucket list --name anolis --hide-headers | cut -f1)" \
  --retention 30d
```

`--retention 0` keeps data forever.

Longer retention costs disk and, on a Raspberry Pi, SD-card wear, and long
queries get slow on a small host. For months of history, run this stack on a
separate monitoring host and point the runtime's `telemetry.influxdb.url` at
it, or downsample, rather than growing a bucket on the device.

Shortening retention deletes data older than the new window. Export what you
need first with
[anolis-telemetry-export](https://github.com/anolishq/anolis-telemetry-export).

## Troubleshooting

If InfluxDB does not start, read its log:

```bash
docker compose -f docker-compose.observability.yml logs influxdb
```

If Grafana shows no data:

1. Check that the runtime logs
   `[InfluxSink] Started, writing to <url>/<bucket>`, then look for
   `[InfluxSink] HTTP error` or `[InfluxSink] Connection error`.
   The `Written <n> events` line appears only when the running total is an
   exact multiple of 1000, so its absence does not mean writes are failing.
2. In InfluxDB's Data Explorer (<http://localhost:8086>), query the
   `anolis_signal` measurement in the bucket.
3. In Grafana, open the InfluxDB data source and run its test.

Grafana loads dashboards from `grafana/dashboards/` and rescans it every 30
seconds. If they still do not appear, restart Grafana:

```bash
docker compose -f docker-compose.observability.yml restart grafana
```

## Stop and clean up

```bash
docker compose -f docker-compose.observability.yml down     # keep data
docker compose -f docker-compose.observability.yml down -v  # delete data
```

## Testing the dashboards

In the anolis repository, `just test-observability` (needs Docker) starts a
throwaway InfluxDB and Grafana, writes fixture telemetry, and runs every
panel, annotation and variable query through Grafana, failing on any query
error. It also checks the values most panels show; on I/O & Watchdog Health it
checks only the missing or excluded count. CI runs it in the `observability`
job.
