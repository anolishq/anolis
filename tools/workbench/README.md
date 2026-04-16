# Anolis Workbench (Phase 06)

Unified commissioning shell with three workspaces:

1. Compose
2. Commission
3. Operate

This tool is implemented as a new project under `tools/workbench/` and reuses the
existing composer backend modules for project CRUD, render/preflight, launch
ownership, and log streaming.

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

Standalone tools remain supported:

1. `tools/system-composer/`
2. `tools/operator-ui/`
