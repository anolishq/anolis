# Schemas

Machine-readable contract artifacts for Anolis.

## Runtime Config Schema

File:

1. `runtime-config.schema.json`

Scope:

1. Runtime YAML consumed by `anolis-runtime --config` and `--check-config`.
2. Targets `anolis-runtime*.yaml` files under `config/`.
3. Excludes provider-local YAML and telemetry-export YAML.

Compatibility notes for current wave:

1. Unknown keys are intentionally allowed by schema to match runtime loader behavior (warn-and-ignore).
2. Deprecated keys are still accepted:
   - `automation.behavior_tree_path`
   - flat `telemetry.influx_*`, `telemetry.batch_size`, `telemetry.flush_interval_ms`
3. Use runtime semantic validation (`anolis-runtime --check-config`) alongside schema validation.
4. Runtime parser behavior (`yaml-cpp`) is authoritative for config semantics;
   schema tooling must not assume Python YAML parsing is equivalent for edge cases (for example duplicate keys and ambiguous scalar resolution).
5. Contract tooling enforces schema draft lock and meta-validation:
   - schema must declare Draft-07
   - schema must pass JSON Schema meta-validation before instance checks

## Machine Profile Schema

File:

1. `machine-profile.schema.json`

Scope:

1. Machine profile manifests for machine realizations.
2. Declares package entrypoints, provider config references, optional behavior assets,
   validation metadata, and compatibility metadata.
3. Works with `tests/contracts/machine-profile/validate-machine-profiles.py` to enforce schema validity,
   reference integrity, and runtime-profile schema compatibility.

## Runtime HTTP OpenAPI Contract

File:

1. `http/runtime-http.openapi.v0.yaml`

Scope:

1. Canonical machine-validated OpenAPI contract for runtime `/v0` HTTP operations.
2. Source is OpenAPI 3.x.
3. Validated by:
   - `tests/contracts/runtime-http/validate-runtime-http-openapi.py`
   - `tests/contracts/runtime-http/validate-runtime-http-examples.py`
   - `tests/contracts/runtime-http/validate-runtime-http-conformance.py`
4. Human-facing documentation:
   - `docs/http/README.md`
   - `docs/http-api.md`

## Commissioning Contracts

System Composer and Workbench contracts are maintained in the
[`anolis-workbench`](https://github.com/anolishq/anolis-workbench) repository.
