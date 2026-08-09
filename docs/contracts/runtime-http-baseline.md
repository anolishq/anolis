# Runtime HTTP Baseline

Status: Locked.

## Purpose

Freeze implemented `/v0` HTTP behavior so OpenAPI, examples, and conformance tests stay aligned with runtime handlers.

## Canonical Artifacts

1. Route registration: `core/http/server.cpp`
2. Handlers: `core/http/handlers/*.cpp`
3. OpenAPI contract: `schemas/http/runtime-http.openapi.v0.yaml`
4. Example fixtures: `tests/contracts/runtime-http/examples/`
5. Validators:
   - `tests/contracts/runtime-http/validate-runtime-http-openapi.py`
   - `tests/contracts/runtime-http/validate-runtime-http-examples.py`
   - `tests/contracts/runtime-http/validate-runtime-http-conformance.py`

## Locked Behavior Summary

### Route Inventory

1. `GET /v0/runtime/status`
2. `GET /v0/providers/health`
3. `GET /v0/devices`
4. `GET /v0/devices/{provider_id}/{device_id}/capabilities`
5. `GET /v0/state`
6. `GET /v0/state/{provider_id}/{device_id}`
7. `POST /v0/call`
8. `GET /v0/mode`
9. `POST /v0/mode`
10. `GET /v0/parameters`
11. `POST /v0/parameters`
12. `GET /v0/automation/tree`
13. `GET /v0/automation/status`
14. `GET /v0/events` (SSE)
15. `OPTIONS /v0/call`
16. `OPTIONS /v0/.*`

### Semantics Anchors

1. `/v0/call` requires `function_id` (no function-name dispatch in request body).
2. `/v0/state` returns only devices with cached state entries.
3. `/v0/state/{provider_id}/{device_id}` supports repeated `signal_id` filter params.
4. `/v0/providers/health` always returns a `supervision` object.
5. `/v0/providers/health` device entries always carry `type_version` and
   `descriptor_tags`, passed through verbatim from the provider's inventory
   descriptor — the runtime interprets no key in them. Both are present but
   empty (`""` / `{}`) when the provider declared no identity, and for devices
   the provider reports but the registry does not carry, since no descriptor
   exists for those. Bounded on ingest: at most 32 tags, and every key and value
   truncated to 128 characters.
6. `/v0/events` currently emits:
   - `state_update`, `quality_change`, `device_availability`, `mode_change`, `parameter_change`, `bt_error`, `provider_health_change`.

## Validation Gates

1. OpenAPI structural validation:
   - `python3 tests/contracts/runtime-http/validate-runtime-http-openapi.py`
2. Example payload validation:
   - `python3 tests/contracts/runtime-http/validate-runtime-http-examples.py`
3. Live runtime conformance smoke:
   - `python3 tests/contracts/runtime-http/validate-runtime-http-conformance.py --runtime-bin <...> --provider-bin <...>`

## Drift Notes and Change Rule

1. Runtime implementation is authoritative when docs/spec disagree.
2. Contract changes require synchronized updates to:
   - handler behavior
   - OpenAPI
   - examples/manifest
   - validators/tests
   - this baseline
3. Keep `/v0/events` schema depth conservative; tighten only with matching runtime evidence.
