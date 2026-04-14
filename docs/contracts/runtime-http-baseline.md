# Runtime HTTP Baseline (Pre-OpenAPI)

Status: Baseline snapshot for `contracts-02-runtime-http.md` execution.

Purpose:

1. Lock current implemented `/v0` behavior before OpenAPI authoring.
2. Make implementation-vs-doc drift explicit.
3. Keep the contract-02 work additive and predictable.

## Implemented Route Inventory

Source of truth for this snapshot is route registration in `core/http/server.cpp`.

1. `GET /v0/devices`
2. `GET /v0/devices/{provider_id}/{device_id}/capabilities`
3. `GET /v0/state`
4. `GET /v0/state/{provider_id}/{device_id}`
5. `POST /v0/call`
6. `GET /v0/runtime/status`
7. `GET /v0/mode`
8. `POST /v0/mode`
9. `GET /v0/parameters`
10. `POST /v0/parameters`
11. `GET /v0/automation/tree`
12. `GET /v0/automation/status`
13. `GET /v0/providers/health`
14. `GET /v0/events` (SSE)
15. `OPTIONS /v0/call`
16. `OPTIONS /v0/.*`

## Current Payload Notes (Implementation)

1. `GET /v0/mode` currently returns `status` + `mode` only.
2. `POST /v0/call` currently requires `function_id` (transport decode does not accept `function_name`).
3. `GET /v0/state` currently emits only devices that have cached state entries.
4. `GET /v0/state/{provider_id}/{device_id}` supports repeated `signal_id` query params for filtering.
5. `GET /v0/providers/health` emits `supervision` as an object for every provider (never `null`).
6. `GET /v0/events` emits these event names:
   - `state_update`
   - `quality_change`
   - `device_availability`
   - `mode_change`
   - `parameter_change`
   - `bt_error`
   - `provider_health_change`

## Documented Drift to Resolve in Contract-02

1. `/v0/automation/status` and `/v0/events` are implemented, but not covered with the same depth as core REST endpoints in current docs.

## Contract-02 Readiness Checklist

Ready to start when these are executed in order:

1. Freeze this baseline as the implementation anchor.
2. Decide per drift item: implementation change vs documentation correction.
3. Author `schemas/http/runtime-http.openapi.v0.yaml` against the decided baseline.
4. Add spec lint + structural validation in CI.
5. Add example payload validation and runtime conformance smoke checks.

## Slice 1 Status

Implemented for initial contracts-02 execution:

1. `schemas/http/runtime-http.openapi.v0.yaml` created with current shipped endpoint coverage.
2. Structural OpenAPI validation script added at `tools/contracts/validate-runtime-http-openapi.py`.
3. CI structural gate wired in `.github/workflows/ci.yml` (Linux strict lane).
4. `/v0/automation/status` and `/v0/events` documentation coverage added in `docs/http-api.md`.

## Slice 2 Status

Implemented for contract drift hardening:

1. Example manifest and payload fixtures added under `tests/contracts/runtime-http/examples/`.
2. Example schema validator added at `tools/contracts/validate-runtime-http-examples.py`.
3. Live runtime conformance smoke validator added at `tools/contracts/validate-runtime-http-conformance.py`.
4. CI gates updated:
   - Linux strict lane validates OpenAPI examples.
   - Provider compatibility lane runs live conformance smoke.
5. Live conformance includes deterministic `400`, `404`, and `503` negative checks.
6. Live conformance supports optional response capture (`--capture-dir`) for example refresh.
