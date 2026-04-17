#!/usr/bin/env python3
"""Compatibility wrapper for the packaged handoff export CLI."""

from __future__ import annotations

try:
    from anolis_workbench_backend import package_cli
except ModuleNotFoundError as exc:  # pragma: no cover
    if exc.name != "anolis_workbench_backend":
        raise
    raise SystemExit(
        "ERROR: Missing local package 'anolis_workbench_backend'. "
        "Run `python -m pip install -r requirements.txt` from repo root."
    ) from exc


if __name__ == "__main__":
    raise SystemExit(package_cli.main())
