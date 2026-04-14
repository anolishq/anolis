# Schemas

Machine-readable contract artifacts for Anolis.

## Runtime Config Schema

File:

1. `runtime-config.schema.json`

Scope:

1. Runtime YAML consumed by `anolis-runtime --config` and `--check-config`.
2. Targets `anolis-runtime*.yaml` files under `config/` and composer runtime outputs under `systems/`.
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

1. Machine package manifests at `config/**/machine-profile.yaml`.
2. Declares package entrypoints, provider config references, optional behavior assets,
   validation metadata, and compatibility metadata.
3. Works with `tools/contracts/validate-machine-profiles.py` to enforce schema validity,
   reference integrity, and runtime-profile schema compatibility.
