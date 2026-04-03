# Anolis System Composer

A graphical tool for composing, configuring, and launching Anolis systems —
no command-line knowledge required.

## Quick Start (Windows)

Double-click `start.cmd` from this directory. A browser window will open
automatically at `http://localhost:3002`.

## Quick Start (Linux / macOS)

```sh
./tools/system-composer/start.sh
```

## What it does

The composer lets you pick a starter template (sim, mixed-bus, bioreactor),
fill in device and provider settings through a form UI, save the system to a
named project, and launch the full Anolis runtime stack with one click.
Projects are stored in `systems/` at the repo root (gitignored).

## System Project Layout

Each project in `systems/` is a self-contained machine description:

```
systems/<project-name>/
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

The `behaviors/` subdirectory is not managed by the composer in v1. Place BT
XMLs there manually and reference them by path in the runtime config's
`automation.behavior_tree` field. The repo-level `behaviors/` directory is
reserved for generic and test BTs not associated with any specific system.
