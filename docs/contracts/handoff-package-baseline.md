# Handoff Package Baseline

Status: Locked.

## Purpose

Freeze Phase 08 commissioning handoff package behavior so `.anpkg` export and
validation remain explicit and test-backed.

## Canonical Artifacts

1. Export core: `tools/workbench/backend/exporter.py`
2. Package validator: `tools/workbench/backend/package_validator.py`
3. Validation command: `tools/contracts/validate-handoff-packages.py`
4. CLI wrapper: `tools/package.py`
5. Workbench HTTP export route: `POST /api/projects/{name}/export`
6. Unit/route tests:
   - `tools/workbench/tests/unit/test_exporter.py`
   - `tools/workbench/tests/unit/test_package_validator.py`
   - `tools/workbench/tests/unit/test_shell_route_support.py`

## Locked Behavior Summary

1. Package format is zip distribution (`.anpkg`) of canonical v1 structure:
   - `machine-profile.yaml`
   - `runtime/anolis-runtime.yaml`
   - `runtime/behaviors/*` (if referenced)
   - `providers/<provider-id>.yaml`
   - `meta/provenance.json`
   - `meta/checksums.sha256`
2. Export is deterministic:
   - stable file ordering
   - stable zip metadata
   - deterministic `meta/provenance.json.exported_at` derivation
3. Runtime provider config args are package-relative (`providers/<id>.yaml`).
4. Behavior references are package-relative under `runtime/behaviors/`.
5. Telemetry tokens are redacted from packaged runtime config content.
6. Validation enforces:
   - structure + checksums
   - package-root reference integrity
   - runtime schema compatibility
   - replay assumptions from clean extracted package root
   - optional runtime `--check-config` replay check when runtime binary is provided

## Validation Gates

1. `python3 tools/contracts/validate-handoff-packages.py`
2. `python3 tools/contracts/validate-handoff-packages.py --runtime-bin <runtime-binary>` (optional replay hardening)
3. `python3 -m pytest tools/workbench/tests -q`

## Drift Notes and Change Rule

1. Keep machine-profile schema reuse (`schema_version: 1`); do not fork a parallel manifest dialect.
2. Package layout/reference changes require synchronized updates to:
   - exporter
   - package validator
   - contract docs
   - tests
3. Secret handling rules are non-negotiable: token-like values must fail validation.
