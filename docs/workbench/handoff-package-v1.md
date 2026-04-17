# Handoff Package v1

Status: Active baseline for Phase 08.

## Purpose

Define the deterministic package format produced by commissioning for
headless runtime deployment.

## Distribution Form

1. Archive format: zip
2. File extension: `.anpkg`
3. Canonical internal layout:
   - `machine-profile.yaml`
   - `runtime/anolis-runtime.yaml`
   - `runtime/behaviors/*` (if referenced)
   - `providers/<provider-id>.yaml`
   - `meta/provenance.json`
   - `meta/checksums.sha256`

## Manifest Lock

`machine-profile.yaml` remains the package manifest root (`schema_version: 1`).
No competing manifest dialect is introduced.

Path rewrite contract at export:

1. `runtime_profiles.*` -> package-relative runtime paths.
2. `providers.<id>.config` -> `providers/<id>.yaml`.
3. `behaviors[]` -> `runtime/behaviors/<file>.xml`.
4. Runtime provider `--config` args -> `providers/<id>.yaml`.

## Determinism Lock

For identical project input state, exported bytes are identical across wrappers
(Workbench HTTP export and `tools/package.py` CLI):

1. Stable file ordering in archive.
2. Stable zip metadata.
3. Deterministic `meta/provenance.json.exported_at`:
   - derived from `system.json.meta.created` (UTC second precision), or
   - fallback `1970-01-01T00:00:00Z` when absent/unparseable.

## Security and Secret Policy

1. `telemetry.influxdb.token` is stripped from packaged runtime config content.
2. Deploy-time token injection uses `INFLUXDB_TOKEN`.
3. Validator fails if token-like secrets remain in package files.
4. `meta/provenance.json` records redaction policy metadata.

## Integrity Policy

`meta/checksums.sha256` contains sha256 lines (`<digest>  <relative-path>`) for
every package file except `meta/checksums.sha256` itself.

## Validation Contract

Use:

```bash
python3 tools/contracts/validate-handoff-packages.py
```

Optional runtime replay hardening:

```bash
python3 tools/contracts/validate-handoff-packages.py --runtime-bin <runtime-binary>
```

Validation includes:

1. structure + checksums
2. machine-profile schema and package-context references
3. runtime schema compatibility
4. static replay assumptions from clean package root
5. optional runtime `--check-config` replay check

## Related Artifacts

1. `tools/workbench/backend/exporter.py`
2. `tools/workbench/backend/package_validator.py`
3. `tools/package.py`
4. `docs/contracts/handoff-package-baseline.md`
