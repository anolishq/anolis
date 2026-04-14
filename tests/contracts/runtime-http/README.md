# Runtime HTTP Contract Fixtures

Captured fixture payloads for the `/v0` HTTP contract.

## Files

1. [`manifest.yaml`](manifest.yaml): maps each example file to `method + path + status + content_type`.
2. `*.json`: JSON response examples validated against OpenAPI response schemas.
3. `*.txt`: text payload examples (currently SSE for `/v0/events`).

## Validation

Run:

```bash
python3 tools/contracts/validate-runtime-http-examples.py
```

The validator checks:

1. Manifest covers all required runtime HTTP operations.
2. Each example maps to an existing OpenAPI operation/response/media type.
3. JSON examples satisfy the declared OpenAPI response schema.
4. Text examples satisfy the declared non-JSON schema type.

## Refresh Workflow

Examples remain curated contract anchors, but can be refreshed from live runtime captures:

```bash
python3 tools/contracts/validate-runtime-http-conformance.py \
  --runtime-bin <path-to-anolis-runtime> \
  --provider-bin <path-to-anolis-provider-sim> \
  --capture-dir tests/contracts/runtime-http/examples/_captures
```

The capture directory contains:

1. Per-check response body files.
2. A `manifest.json` index for request/response metadata.
