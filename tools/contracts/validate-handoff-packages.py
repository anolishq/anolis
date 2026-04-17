#!/usr/bin/env python3
"""Validate commissioning handoff package contracts and replay assumptions."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import tempfile
import zipfile
from typing import Any

import yaml


def _repo_root_from_script() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def _load_backend_modules(repo_root: pathlib.Path):
    import sys

    backend_dir = repo_root / "tools" / "workbench" / "backend"
    if str(backend_dir) not in sys.path:
        sys.path.insert(0, str(backend_dir))

    import exporter  # noqa: E402
    import package_validator  # noqa: E402

    return exporter, package_validator


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate .anpkg handoff package structure, integrity, and replay checks.")
    parser.add_argument(
        "--repo-root",
        default=str(_repo_root_from_script()),
        help="Path to repository root (default: auto-detected).",
    )
    parser.add_argument(
        "--package",
        action="append",
        default=[],
        help="Package archive or package directory path (can be specified multiple times).",
    )
    parser.add_argument(
        "--runtime-bin",
        default="",
        help="Optional runtime binary path for --check-config replay validation.",
    )
    parser.add_argument(
        "--skip-fixtures",
        action="store_true",
        help="Skip built-in valid/invalid fixture checks.",
    )
    return parser.parse_args()


def _build_fixture_project(repo_root: pathlib.Path, fixture_root: pathlib.Path) -> pathlib.Path:
    template_path = repo_root / "tools" / "system-composer" / "templates" / "sim-quickstart" / "system.json"
    payload = json.loads(template_path.read_text(encoding="utf-8"))

    project_dir = fixture_root / "fixture-project"
    project_dir.mkdir(parents=True, exist_ok=True)
    behavior_dir = project_dir / "behaviors"
    behavior_dir.mkdir(parents=True, exist_ok=True)
    (behavior_dir / "fixture.xml").write_text("<root />\n", encoding="utf-8")

    payload["meta"]["name"] = "fixture-project"
    payload["meta"]["created"] = "2026-04-16T19:01:02+00:00"
    payload["topology"]["runtime"]["automation_enabled"] = True
    payload["topology"]["runtime"]["behavior_tree_path"] = "behaviors/fixture.xml"
    payload["topology"]["runtime"]["telemetry"] = {
        "enabled": True,
        "influxdb": {
            "url": "http://localhost:8086",
            "org": "anolis",
            "bucket": "anolis",
            "token": "fixture-secret",
        },
    }

    (project_dir / "system.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return project_dir


def _extract_archive(archive_path: pathlib.Path, out_dir: pathlib.Path) -> pathlib.Path:
    root = out_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path, mode="r") as archive:
        archive.extractall(root)
    return root


def _recompute_checksums(package_root: pathlib.Path) -> None:
    lines: list[str] = []
    for path in sorted(package_root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(package_root).as_posix()
        if rel == "meta/checksums.sha256":
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {rel}")
    (package_root / "meta" / "checksums.sha256").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _run_fixture_checks(
    *,
    repo_root: pathlib.Path,
    runtime_bin: pathlib.Path | None,
    exporter,
    package_validator,
) -> tuple[int, int]:
    checks = 0
    failures = 0
    with tempfile.TemporaryDirectory(prefix="anolis-package-fixtures-") as tmp_dir:
        root = pathlib.Path(tmp_dir)
        project_dir = _build_fixture_project(repo_root, root)
        package_path = root / "fixture.anpkg"

        exporter.build_package(project_dir=project_dir, out_path=package_path)
        checks += 1
        try:
            package_validator.validate_package(package_path, runtime_bin=runtime_bin)
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"  - [fixture valid] unexpected failure: {exc}")

        # Invalid fixture 1: checksum drift.
        drift_root = _extract_archive(package_path, root / "invalid-checksum")
        runtime_path = drift_root / "runtime" / "anolis-runtime.yaml"
        runtime_path.write_text(runtime_path.read_text(encoding="utf-8") + "\n# tampered\n", encoding="utf-8")
        checks += 1
        try:
            package_validator.validate_package(drift_root, runtime_bin=None)
            failures += 1
            print("  - [fixture invalid checksum] expected failure, but validator passed")
        except package_validator.PackageValidationError:
            pass

        # Invalid fixture 2: secret leak (with recomputed checksums).
        secret_root = _extract_archive(package_path, root / "invalid-secret")
        secret_runtime_path = secret_root / "runtime" / "anolis-runtime.yaml"
        runtime_payload = yaml.safe_load(secret_runtime_path.read_text(encoding="utf-8"))
        runtime_payload.setdefault("telemetry", {}).setdefault("influxdb", {})["token"] = "leaked-token"
        secret_runtime_path.write_text(yaml.safe_dump(runtime_payload, sort_keys=False), encoding="utf-8")
        _recompute_checksums(secret_root)
        checks += 1
        try:
            package_validator.validate_package(secret_root, runtime_bin=None)
            failures += 1
            print("  - [fixture invalid secret] expected failure, but validator passed")
        except package_validator.PackageValidationError:
            pass

        # Invalid fixture 3: replay/path escape drift (with recomputed checksums).
        escape_root = _extract_archive(package_path, root / "invalid-path-escape")
        escape_runtime_path = escape_root / "runtime" / "anolis-runtime.yaml"
        escape_payload = yaml.safe_load(escape_runtime_path.read_text(encoding="utf-8"))
        escape_payload["providers"][0]["args"] = ["--config", "../../outside.yaml"]
        escape_runtime_path.write_text(yaml.safe_dump(escape_payload, sort_keys=False), encoding="utf-8")
        _recompute_checksums(escape_root)
        checks += 1
        try:
            package_validator.validate_package(escape_root, runtime_bin=None)
            failures += 1
            print("  - [fixture invalid path] expected failure, but validator passed")
        except package_validator.PackageValidationError:
            pass

    return checks, failures


def main() -> int:
    args = _parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    exporter, package_validator = _load_backend_modules(repo_root)
    runtime_bin = pathlib.Path(args.runtime_bin).resolve() if args.runtime_bin else None

    checks = 0
    failures = 0

    if not args.skip_fixtures:
        fixture_checks, fixture_failures = _run_fixture_checks(
            repo_root=repo_root,
            runtime_bin=runtime_bin,
            exporter=exporter,
            package_validator=package_validator,
        )
        checks += fixture_checks
        failures += fixture_failures

    for raw in args.package:
        checks += 1
        candidate = pathlib.Path(raw).expanduser()
        try:
            package_validator.validate_package(candidate, runtime_bin=runtime_bin)
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"  - [package {candidate}] {exc}")

    print("handoff-package contract summary")
    print(f"  checks run: {checks}")
    print(f"  failures: {failures}")
    if args.package:
        print(f"  user packages: {len(args.package)}")
    else:
        print("  user packages: 0")
    if args.skip_fixtures:
        print("  fixture checks: skipped")
    else:
        print("  fixture checks: enabled")
    print(f"  runtime replay binary: {runtime_bin if runtime_bin else 'not provided (static replay checks only)'}")

    if failures:
        print("\nHandoff package contract checks failed.")
        return 1

    print("\nAll handoff package contract checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
