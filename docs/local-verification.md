# Contract Validation

All contract checks run automatically in CI on every push and pull request.
To run any validator locally, invoke it directly from the repo root.

## Runtime config contracts

Requires a local runtime binary (`build/dev-release/core/anolis-runtime`):

```bash
python3 tests/contracts/runtime-config/validate-runtime-configs.py --runtime-bin build/dev-release/core/anolis-runtime
```

Enforces:

1. JSON Schema conformance across all tracked runtime YAML config files
2. Runtime loader acceptance via `anolis-runtime --check-config` on each file
3. Contract fixture sets (`valid`, `invalid/schema`, `invalid/runtime`)

## Machine profile contracts

```bash
python3 tests/contracts/machine-profile/validate-machine-profiles.py
```

Enforces:

1. Machine profile schema conformance
2. Referenced file existence
3. Referenced runtime profile compatibility

## Telemetry timeseries contracts

```bash
python3 tests/contracts/telemetry-timeseries/validate-telemetry-timeseries.py
```

Enforces:

1. `anolis_signal` telemetry row schema conformance
2. Fixture coverage for typed value fields and quality-only rows
3. Invalid fixture rejection

## Runtime HTTP contracts

```bash
# Structural + example checks (no binary needed)
python3 tests/contracts/runtime-http/validate-runtime-http-openapi.py
python3 tests/contracts/runtime-http/validate-runtime-http-examples.py
```

Enforces:

1. OpenAPI document shape and required `/v0` endpoint coverage
2. Internal `$ref` resolution
3. SSE media type on `/v0/events`
4. Example payload conformance from `tests/contracts/runtime-http/examples/manifest.yaml`

## Runtime HTTP live conformance

Requires both a runtime binary and a provider-sim binary:

```bash
python3 tests/contracts/runtime-http/validate-runtime-http-conformance.py \
  --runtime-bin build/dev-release/core/anolis-runtime \
  --provider-bin ../anolis-provider-sim/build/dev-release/anolis-provider-sim
```

Starts a live runtime fixture and validates responses against the OpenAPI schema,
including negative cases (400, 404, 503).

## Docs link check

Dead links in `docs/` and root markdown files are checked by the `docs.yml`
workflow via `lycheeverse/lychee-action` (offline mode). No local tooling needed.

## Focused C++ tests

```bash
ctest --test-dir build/dev-release --output-on-failure -R "ConfigTest|RuntimeOwnershipValidationTest"
```

Covers runtime YAML parsing, restart-policy validation, automation config
handling, and ownership-claim invariants.
