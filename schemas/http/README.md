# Runtime HTTP OpenAPI Schema

Canonical machine-validated OpenAPI contract for the runtime HTTP `/v0` surface.

Files:

1. `runtime-http.openapi.v0.yaml` - OpenAPI 3.x source of truth for `/v0`.

Validation:

1. `python3 tools/contracts/validate-runtime-http-openapi.py`
2. `python3 tools/contracts/validate-runtime-http-examples.py`
3. `python3 tools/contracts/validate-runtime-http-conformance.py`

Human-facing documentation:

1. `docs/http/README.md`
2. `docs/http-api.md`
