# Commissioning Handoff Runbook

Status: Active.
Audience: Machine integrators commissioning in Workbench and handing off to
headless runtime deployment.

## 1) Preconditions

1. Project validates and launches cleanly in Commission workspace.
2. Operate checks are complete for target behavior.
3. Runtime contracts and Workbench tests are green locally.

## 2) Export Package

Workbench path:

1. Open project in `Commission`.
2. Click `Export Package…`.
3. Save generated `.anpkg` artifact.

CLI path:

```bash
python3 tools/package.py <project-name> [output.anpkg]
```

## 3) Validate Package

Static package validation:

```bash
python3 tools/contracts/validate-handoff-packages.py --package <path-to-package.anpkg>
```

Replay hardening with runtime binary:

```bash
python3 tools/contracts/validate-handoff-packages.py \
  --package <path-to-package.anpkg> \
  --runtime-bin <path-to-anolis-runtime>
```

## 4) Deploy and Replay from Clean Directory

1. Create fresh deployment directory on target host.
2. Copy package archive.
3. Extract package:

```bash
unzip <machine>.anpkg -d /opt/anolis/<machine>
```

4. Configure deploy-time secret(s), minimally:

```bash
export INFLUXDB_TOKEN='<token-value>'
```

5. Start runtime from extracted package root:

```bash
cd /opt/anolis/<machine>
<path-to-anolis-runtime> --config runtime/anolis-runtime.yaml
```

Expected: runtime loads config and providers without dependency on
`systems/<project>/`.

## 5) Operator Handoff Package Contents

Deliver all of:

1. `.anpkg` artifact
2. Runtime/provider binary provenance (build IDs/refs)
3. Validation output (`validate-handoff-packages.py` logs)
4. Mode/operation checklist used during commissioning
5. Deployment env requirements (`INFLUXDB_TOKEN` policy)

## 6) Rollback Guidance

1. Keep prior known-good `.anpkg` artifact in deployment storage.
2. If new package replay fails:
   - stop runtime
   - redeploy previous package archive
   - restart runtime with previous package config
3. Record failure cause and attach validation output before next rollout attempt.

## 7) Troubleshooting

1. Checksum mismatch:
   - Artifact tampered/corrupted after export.
   - Re-export and re-validate package.
2. Secret leakage validation failure:
   - Token-like value exists in package content.
   - Remove embedded token and re-export.
3. Path escape/reference failure:
   - Runtime/provider/behavior path is not package-relative.
   - Fix project config, re-export, and validate.
4. Replay `--check-config` failure:
   - Review runtime error output and runtime schema expectations.
   - Re-run static validator first, then replay validator.
