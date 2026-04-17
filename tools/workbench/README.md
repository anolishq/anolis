# Anolis Workbench

Unified commissioning shell with three workspaces:

1. Compose
2. Commission
3. Operate

This tool is implemented as a new project under `tools/workbench/` and reuses the
existing composer backend modules for project CRUD, render/preflight, launch
ownership, and log streaming.

Phase 6 and 7 hardening currently includes:

1. URL-routed shell workspaces with unsaved-change and running-runtime guard prompts.
2. Launch hard-block guidance when another runtime is active.
3. Commission health sourced from runtime contracts (`/v0/runtime/status`, `/v0/providers/health`).
4. Operate contract state sourced from `/v0/*` and `/v0/events` through workbench pass-through.
5. Operate parity panels for automation status, runtime parameters, behavior tree, and event trace.
6. Stream-state badge semantics (`connected`, `reconnecting`, `disconnected`, `stale`) and telemetry parity path with embedded Grafana plus explicit external fallback.

Phase 8 export pipeline currently includes:

1. Deterministic handoff package export from Commission workspace (`.anpkg` zip).
2. Shared pure export core: `tools/workbench/backend/exporter.py`.
3. HTTP wrapper endpoint: `POST /api/projects/<name>/export`.
4. CLI wrapper: `python -m anolis_workbench_backend.package_cli <project-name> [output.anpkg]`.
5. Package validation command: `python tools/contracts/validate-handoff-packages.py`.

Install Python dependencies first from repo root:

```sh
python -m pip install -r requirements.txt
```

## Quick Start (Linux / macOS)

```sh
./tools/workbench/start.sh
```

## Quick Start (Windows)

Double-click `tools/workbench/start.cmd`.

## Environment Overrides

1. `ANOLIS_WORKBENCH_HOST` (default: `127.0.0.1`)
2. `ANOLIS_WORKBENCH_PORT` (default: `3010`)
3. `ANOLIS_WORKBENCH_OPEN_BROWSER` (`1` or `0`, default: `1`)
4. `ANOLIS_OPERATOR_UI_BASE` (default: `http://localhost:3000`)
5. `ANOLIS_DATA_DIR` (project storage root, default: `~/.anolis/systems`; legacy fallback: repo `systems/` when present)

## Route Model

1. `/`
2. `/projects/<name>/compose`
3. `/projects/<name>/commission`
4. `/projects/<name>/operate`

`/projects/<name>` redirects to compose in the frontend router.

## API Contract Notes

Composer control endpoints are preserved under workbench:

1. `/api/status`
2. `/api/projects/*`
3. `/api/projects/*/logs` (SSE)
4. `/api/projects/*/preflight|launch|stop|restart`

Operate workspace consumes runtime contract endpoints through workbench
pass-through routes under `/v0/*` and `/v0/events`.

Workbench-specific export route:

1. `POST /api/projects/<name>/export` returns deterministic `.anpkg` bytes
   as `application/zip`.

## Phase 08 Docs

1. `docs/workbench/handoff-package-v1.md`
2. `docs/workbench/commissioning-handoff-runbook.md`

Standalone tools remain supported:

1. `tools/system-composer/`
2. `tools/operator-ui/`

## Phase 07 Closeout Artifacts

1. `docs/workbench/operate-parity-matrix.md`
2. `docs/workbench/operator-ui-to-workbench-migration-runbook.md`
3. `docs/workbench/operator-ui-retirement-go-no-go.md`
