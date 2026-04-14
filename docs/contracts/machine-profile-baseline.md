# Machine Profile Baseline

Status: locked baseline for `contracts-03-machine-profile-packaging`.

Purpose:

1. Define what counts as a machine package in this wave.
2. Lock manifest scope before validator/CI wiring.
3. Avoid layout churn while preserving current bioreactor workflows.

## Current Package Reality (Pre-Contract)

Source-controlled machine assets already exist under `config/`:

1. `config/bioreactor/` (canonical lab application profile)
2. `config/mixed-bus-providers/` (validation/scenario pack)

Current runtime profile entrypoints are descriptive and already used in docs/runbooks:

1. `anolis-runtime.bioreactor.manual.yaml`
2. `anolis-runtime.bioreactor.telemetry.yaml`
3. `anolis-runtime.bioreactor.automation.yaml`
4. `anolis-runtime.bioreactor.full.yaml`

## Package Boundary (Locked for This Wave)

1. Keep machine packages under existing `config/<machine_id>/` paths.
2. Do not migrate to `config/machines/<machine_id>/` in this wave.
3. Composer workspace output in `systems/<project>/` remains a separate concern.
4. Core runtime remains provider-agnostic.

## Manifest Contract (v1)

Each machine package must include `machine-profile.yaml` with:

1. identity:
   - `schema_version`
   - `machine_id`
   - `display_name`
2. package entrypoints:
   - `runtime_profiles.manual` (required)
   - optional `telemetry`, `automation`, `full`
3. provider config map:
   - provider ID -> config file path
4. optional behavior asset list
5. runtime contract references:
   - runtime config baseline doc path
   - runtime HTTP baseline doc path
6. compatibility metadata:
   - runtime contract versions/notes
   - provider compatibility policy per provider

## Validation Expectations

Contract tooling must validate:

1. manifest schema correctness,
2. reference integrity (all declared files exist),
3. referenced runtime profiles are valid against `schemas/runtime-config.schema.json`.

## Out of Scope (This Wave)

1. packaging repo split decisions,
2. runtime/provider version negotiation redesign,
3. cross-dialect shared schema fragment extraction.

## Next Step

Implement `schemas/machine-profile.schema.json`, `tools/contracts/validate-machine-profiles.py`,
fixtures, and CI/local gate wiring.
