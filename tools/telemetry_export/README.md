# Telemetry Export Service (MVP)

External data-plane service for telemetry export.

This service queries InfluxDB telemetry and provides a guarded, authenticated API
for `signals` exports. It intentionally does not add export routes to
`anolis-runtime`.

## Endpoints

1. `GET /v1/health`
2. `POST /v1/exports/signals:query`
3. `GET /v1/exports/manifests/{export_id}`

## Auth

Use bearer token auth on every export request:

- Header: `Authorization: Bearer <token>`

This applies to both query and manifest routes.

Token comes from service config (`server.auth_token`).
Token precedence:

1. `ANOLIS_EXPORT_AUTH_TOKEN` (env, if set)
2. `server.auth_token` (config)

Influx token precedence:

1. `ANOLIS_EXPORT_INFLUX_TOKEN` (env, if set)
2. `influxdb.token` (config)

## Request Tracing

Optional headers for traceability:

1. `X-Request-Id`
2. `X-Requester-Id`

If missing, the service generates `X-Request-Id` and defaults requester to
`anonymous`. Successful responses include `X-Request-Id`.

Export responses also include:

1. `X-Export-Id`
2. `X-Export-Manifest-Hash` (`sha256:...`)

The full manifest is always available in JSON bodies and via
`GET /v1/exports/manifests/{export_id}`.

## Optional Scope Enforcement

Config section `authorization` can enforce selector allowlists.

- `authorization.enforce_selector_scope: true`
- Non-empty `allowed_runtime_names` / `allowed_provider_ids` /
  `allowed_device_ids` / `allowed_signal_ids` become hard allowlists.
- Violations return `403 permission_denied`.

## Runtime Disambiguation (v1)

Telemetry points include `runtime_name` tag, and exports support:

- `selector.runtime_names` (optional list filter)
- `runtime_name` output column (included by default)

If `selector.runtime_names` is omitted, results include all matching runtimes in
the bucket.

## Downsample Aggregation Matrix (v1)

For `resolution.mode=downsampled`:

1. Numeric fields (`value_double`, `value_int`, `value_uint`) use requested aggregation (`last|mean|min|max|count`).
2. Non-numeric fields (`value_bool`, `value_string`, `quality`) always use `last`.
3. Requests that combine non-numeric typed output columns (`value_bool`, `value_string`) with non-`last` aggregation are rejected with `400 invalid_argument`.

## Timezone Behavior

`timezone` request input is not supported in v1.
All timestamps are exported in UTC (`RFC3339 Z`).
Supplying `timezone` returns `400 invalid_argument`.

## Response Formats

`format` supports:

1. `json`
2. `csv`
3. `ndjson`

All formats use a bounded-memory spool-to-file pipeline before response
streaming to avoid assembling full exports in RAM.

## Run

```bash
cd /path/to/anolis
python -m tools.telemetry_export.export_service --config config/bioreactor/telemetry-export.bioreactor.yaml
```

## Example Query (JSON Response)

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

## Example Query (CSV Response)

`format=csv` returns a `text/csv` body. The service writes CSV to a temporary
spool file first (bounded memory path), enforces row/size limits, then streams
the file to the response.

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

Fetch manifest metadata for the CSV response:

```bash
export EXPORT_ID=$(grep -i '^X-Export-Id:' /tmp/export.headers | awk '{print $2}' | tr -d '\r')
curl -sS "http://127.0.0.1:8091/v1/exports/manifests/${EXPORT_ID}" \
  -H "Authorization: Bearer export-dev-token"
```

## Example Query (NDJSON Response)

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

## Programmatic Example

```bash
python tools/telemetry_export/examples/query_signals.py \
  --start 2026-04-01T00:00:00Z \
  --end 2026-04-01T00:30:00Z \
  --provider bread0 \
  --provider ezo0 \
  --format json
```

## Notes

1. MVP scope is `signals` only.
2. Completeness is best-effort under current telemetry overflow behavior.
3. `bytes` vs `string` fidelity remains a documented MVP limitation.
4. Config validates field ranges and semantics (for example: `server.port`
   bounds and positive limit values).
5. `authorization.enforce_selector_scope=true` requires at least one non-empty
   `allowed_*` allowlist.
