# Telemetry and dashboards

The runtime can write device signals and health to InfluxDB 2.x. Grafana
dashboards bundled with the runtime read them back.

## On an installed machine

`install.sh --with-observability` installs InfluxDB and Grafana on the target
as native packages, creates the `anolis` bucket with scoped tokens, puts the
runtime's write token in `/etc/anolis/runtime.env` as `INFLUXDB_TOKEN`, and
provisions the dashboards. The runtime writes telemetry only if the installed
variant's config has `telemetry.enabled: true`, for example
`--variant telemetry` in the bioreactor project. See
[Deploy with `install.sh`](deploy/install-sh.md) for the flag and its
behaviour offline.

InfluxDB and Grafana bind `127.0.0.1`. Reach Grafana from another computer
over an SSH tunnel and browse to `http://localhost:3000`:

```bash
ssh -L 3000:127.0.0.1:3000 <user>@<host>
```

`--with-grafana-network` opens Grafana to the network instead. The Grafana
admin password is `GRAFANA_ADMIN_PASSWORD` in `/etc/anolis/observability.env`
on the target, readable by root only.

## For development, or on a separate host

The release asset `anolis-<version>-observability.tar.gz` holds a Docker
Compose stack with InfluxDB, Grafana and the same dashboards. Its
[README](https://github.com/anolishq/anolis/blob/main/observability/README.md)
covers starting it, its default credentials and ports, and data retention.
Those defaults are for local development only.

## Runtime settings

<!-- validate: runtime-config fragment -->

```yaml
telemetry:
  enabled: true
  influxdb:
    url: http://localhost:8086
    org: anolis
    bucket: anolis
    token: <write token>    # or leave unset and set INFLUXDB_TOKEN
```

The other telemetry keys, such as batching and the health interval, are in
[Configuration schema](configuration-schema.md).

## What is written

- `anolis_signal`: a row each time a signal's value or quality changes, not
  on every poll.
- `anolis_device_health` and `anolis_provider_health`: periodic health
  snapshots, every `telemetry.health_interval_ms` (15 s by default; 0 turns
  them off).

The record formats are fixed by the
[telemetry contracts](contracts/README.md).

## Dashboards

The bundled dashboards include **Signal History** and **Device Health**. Each
has a Runtime filter, so several runtimes can share one bucket. The README
linked above explains how the panels read change-only data.

To export telemetry for analysis, use
[anolis-telemetry-export](https://github.com/anolishq/anolis-telemetry-export).
