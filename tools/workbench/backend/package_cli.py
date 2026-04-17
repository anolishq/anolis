"""CLI to build deterministic commissioning handoff packages."""

from __future__ import annotations

import argparse
import pathlib
import sys

from anolis_composer_backend import paths as paths_module
from anolis_workbench_backend import exporter


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build an Anolis handoff package (.anpkg) from a project.")
    parser.add_argument("project_name", help="Project name under the configured systems root.")
    parser.add_argument(
        "output",
        nargs="?",
        help="Output file path (default: ./<project_name>.anpkg).",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    project_name = str(args.project_name).strip()
    if project_name == "":
        print("ERROR: project_name is required", file=sys.stderr)
        return 2

    project_dir = paths_module.SYSTEMS_ROOT / project_name
    output = pathlib.Path(args.output).expanduser() if args.output else pathlib.Path(f"{project_name}.anpkg")
    out_path = output.resolve()

    try:
        exporter.build_package(project_dir=project_dir, out_path=out_path)
    except exporter.ExportError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
