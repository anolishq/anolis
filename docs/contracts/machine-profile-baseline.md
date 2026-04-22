# Machine Profile Baseline

Status: Locked.

## Purpose

Define machine profile manifest structure and lock manifest behavior for validation tooling.

## Canonical Artifacts

1. Manifest schema: `schemas/machine/machine-profile.schema.json`
2. Validator: `tests/contracts/machine-profile/validate-machine-profiles.py`
3. Fixtures: `tests/contracts/machine-profile/`
4. Runtime config schema dependency: `schemas/runtime/runtime-config.schema.json`

## Locked Behavior Summary

1. Machine realizations live in external repos (e.g., `anolis-projects`).
2. Manifest file is `machine-profile.yaml`.
3. Manifest declares:
   - identity (`schema_version`, `machine_id`, `display_name`)
   - runtime profile entrypoints (`manual` required; optional `telemetry`, `automation`, `full`)
   - provider config references
   - optional behavior asset references
   - runtime contract references and compatibility notes
4. Validator enforces:
   - schema correctness
   - referenced file existence
   - referenced runtime profile schema compatibility

## Validation Gates

1. `python3 tests/contracts/machine-profile/validate-machine-profiles.py`
2. Runtime config contract validation remains in place for referenced profiles.

## Drift Notes and Change Rule

1. Keep machine profile schema additive when possible.
2. Path/layout changes require coordinated updates to manifest schema, validator, package docs, and fixtures.
3. Do not introduce packaging-repo split semantics in this baseline.
