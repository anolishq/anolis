# Runtime HTTP Contract

Human-facing guide for the runtime `/v0` HTTP contract.
Canonical machine-validated artifacts live under `schemas/` and `tests/contracts/`.

## Source of Truth

1. OpenAPI spec: [`../../schemas/http/runtime-http.openapi.v0.yaml`](../../schemas/http/runtime-http.openapi.v0.yaml)
2. Example manifest: [`../../tests/contracts/runtime-http/examples/manifest.yaml`](../../tests/contracts/runtime-http/examples/manifest.yaml)
3. Implementation baseline: [`../contracts/runtime-http-baseline.md`](../contracts/runtime-http-baseline.md)
4. Runtime handlers/routes:
   - `core/http/server.cpp`
   - `core/http/handlers/*.cpp`

## Contract Policy

1. Config schema and HTTP contract are separate layers.
2. Shared concepts (mode enums, status code semantics, parameter value shapes) are parity-checked.
3. Shared cross-contract schema fragments are deferred to a later follow-up after HTTP contract stabilization.

## Validation

Run structural OpenAPI validation:

```bash
python3 tools/contracts/validate-runtime-http-openapi.py
```

The validator checks:

1. OpenAPI top-level structure.
2. Required endpoint/method coverage.
3. Response presence for required operations.
4. Internal `$ref` resolution.
5. SSE media type contract for `/v0/events`.

Run example payload validation:

```bash
python3 tools/contracts/validate-runtime-http-examples.py
```

Run live runtime conformance smoke validation (runtime + provider-sim required):

```bash
python3 tools/contracts/validate-runtime-http-conformance.py \
  --runtime-bin <path-to-anolis-runtime> \
  --provider-bin <path-to-anolis-provider-sim> \
  --capture-dir tests/contracts/runtime-http/examples/_captures
```

Live conformance checks:

1. Starts runtime with provider-sim via fixture process management.
2. Exercises all required `/v0` operations.
3. Validates each observed response against the OpenAPI schema declared for that status code.
4. Enforces deterministic non-200 checks for `400`, `404`, and `503` responses.
5. Optionally captures live response payloads to support example refresh workflows.

## Notes

1. `/v0/events` is intentionally documented with provisional schema depth in this initial contract wave.
2. Runtime behavior remains implementation-authoritative for semantics not yet captured as strict OpenAPI constraints.
