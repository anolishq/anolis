# Anolis System Composer

A graphical tool for composing, configuring, and launching Anolis systems —
no command-line knowledge required.

## Quick Start (Windows)

Install Python dependencies first from repo root:

```sh
python -m pip install -r requirements.txt
```

Double-click `start.cmd` from this directory. A browser window will open
automatically at `http://localhost:3002`.

## Quick Start (Linux / macOS)

```sh
./tools/system-composer/start.sh
```

Optional environment overrides:

1. `ANOLIS_COMPOSER_HOST` (default: `127.0.0.1`)
2. `ANOLIS_COMPOSER_PORT` (default: `3002`)
3. `ANOLIS_OPERATOR_UI_BASE` (default: `http://localhost:3000`)
4. `ANOLIS_COMPOSER_OPEN_BROWSER` (`1` or `0`, default: `1`)
5. `ANOLIS_DATA_DIR` (project storage root, default: `~/.anolis/systems`; legacy fallback: repo `systems/` when present)

## What it does

The composer lets you pick a starter template (sim, mixed-bus, bioreactor),
fill in device and provider settings through a form UI, save the system to a
named project, and launch the full Anolis runtime stack with one click.
Projects are stored under the configured systems root (`ANOLIS_DATA_DIR`).

When a runtime is healthy, the launch panel's "Open in Operator UI" link is
resolved in this order:

1. `topology.runtime.operator_ui_base` (if present in `system.json`)
2. first entry in `topology.runtime.cors_origins`
3. backend environment setting `ANOLIS_OPERATOR_UI_BASE`
4. fallback `http://localhost:3000`

## Save and Validation Semantics

Save is backend-authoritative.

1. The frontend submits the full `system.json` payload.
2. The backend validates schema first (`tools/system-composer/schema/system.schema.json`).
3. The backend then runs semantic validation (`tools/system-composer/backend/validator.py`).
4. If either stage fails, save is rejected with HTTP 400 and structured error details.
5. YAML render output is written only after validation passes.

This avoids frontend/backend drift and guarantees generated runtime/provider YAMLs
always come from a validated source document.

## Custom Providers in Composer v1

Composer contract v1 does not support `custom` providers.

1. The UI does not offer custom provider creation.
2. Legacy projects that contain custom providers are shown as unsupported.
3. Save is rejected server-side for any `kind: custom` provider entries.

Custom provider pass-through can be added in a later contract version, but it is
intentionally disabled in v1 to avoid partial/ambiguous behavior.

## System Project Layout

Each project in `<systems-root>/` is a self-contained machine description:

```
<systems-root>/<project-name>/
  system.json             Composer source (topology + paths)
  anolis-runtime.yaml     Generated runtime config
  providers/
    <id>.yaml             Generated per-provider config
  behaviors/              Hand-authored BT XMLs for this system (optional, v1+)
    main.xml
  logs/
    latest.log            Ephemeral launch log (gitignored)
  running.json            Ephemeral launch state (gitignored)
```

Contract notes:

- `system.json` is the composer source of truth. Generated YAML files are derived output.
- `topology.runtime.behavior_tree_path` is the composer-facing BT field; generated runtime YAML uses the canonical runtime key `automation.behavior_tree`.
- `topology.runtime.telemetry` mirrors the runtime's nested `telemetry.*` structure. The composer keeps telemetry settings in `system.json` even when telemetry is disabled, but the generated runtime YAML only emits the nested `telemetry.influxdb` block when telemetry is enabled.

Contract dependencies:

1. Runtime config schema: `schemas/runtime-config.schema.json`
2. Runtime HTTP OpenAPI: `schemas/http/runtime-http.openapi.v0.yaml`
3. Runtime HTTP fixtures (shared with Operator UI tests): `tests/contracts/runtime-http/examples/`
4. Composer control API baseline: `docs/contracts/composer-control-baseline.md`
5. Composer control OpenAPI: `schemas/tools/composer-control.openapi.v1.yaml`

The `behaviors/` subdirectory is not managed by the composer. Place BT XMLs
there manually and reference them by path in `behavior_tree_path`. The repo-level
`behaviors/` directory is reserved for generic and test BTs not associated with
any specific system.

## Out of Scope

**Behavior tree authoring is not part of this tool and never will be.**
Use [Groot2](https://www.behaviortree.dev/groot/) to create and edit BT XMLs —
it is the purpose-built editor for BehaviorTree.CPP trees, with a graphical
canvas, node palette, and live monitoring. The composer's only role with respect
to BTs is storing the path to an XML file and passing it to the runtime at launch.
