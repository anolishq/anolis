"""Tests for package-context validation and replay checks."""

from __future__ import annotations

import hashlib
import json
import pathlib
import sys
import zipfile

import pytest
import yaml

_WB_DIR = pathlib.Path(__file__).resolve().parents[2]
_EXPORTER_DIR = _WB_DIR / "backend"
if str(_EXPORTER_DIR) not in sys.path:
    sys.path.insert(0, str(_EXPORTER_DIR))

import exporter  # noqa: E402
import package_validator  # noqa: E402


def test_validate_package_accepts_valid_archive(tmp_path: pathlib.Path) -> None:
    project_dir = _make_project(tmp_path, "valid-package")
    package_path = tmp_path / "valid-package.anpkg"
    exporter.build_package(project_dir=project_dir, out_path=package_path)
    package_validator.validate_package(package_path)


def test_validate_package_rejects_checksum_drift(tmp_path: pathlib.Path) -> None:
    project_dir = _make_project(tmp_path, "checksum-drift")
    package_path = tmp_path / "checksum-drift.anpkg"
    exporter.build_package(project_dir=project_dir, out_path=package_path)

    extract_dir = tmp_path / "extract"
    with zipfile.ZipFile(package_path, mode="r") as archive:
        archive.extractall(extract_dir)

    runtime_path = extract_dir / "runtime" / "anolis-runtime.yaml"
    runtime_path.write_text(runtime_path.read_text(encoding="utf-8") + "\n# tampered\n", encoding="utf-8")

    with pytest.raises(package_validator.PackageValidationError, match="Checksum mismatch"):
        package_validator.validate_package(extract_dir)


def test_validate_package_rejects_secret_leak_even_with_updated_checksums(tmp_path: pathlib.Path) -> None:
    project_dir = _make_project(tmp_path, "secret-drift")
    package_path = tmp_path / "secret-drift.anpkg"
    exporter.build_package(project_dir=project_dir, out_path=package_path)

    extract_dir = tmp_path / "extract-secret"
    with zipfile.ZipFile(package_path, mode="r") as archive:
        archive.extractall(extract_dir)

    runtime_path = extract_dir / "runtime" / "anolis-runtime.yaml"
    payload = yaml.safe_load(runtime_path.read_text(encoding="utf-8"))
    payload.setdefault("telemetry", {}).setdefault("influxdb", {})["token"] = "reintroduced-secret"
    runtime_path.write_text(yaml.safe_dump(payload, sort_keys=False), encoding="utf-8")
    _recompute_checksums(extract_dir)

    with pytest.raises(package_validator.PackageValidationError, match="Secret-like token value leaked"):
        package_validator.validate_package(extract_dir)


def test_validate_package_rejects_provider_path_escape(tmp_path: pathlib.Path) -> None:
    project_dir = _make_project(tmp_path, "escape-drift")
    package_path = tmp_path / "escape-drift.anpkg"
    exporter.build_package(project_dir=project_dir, out_path=package_path)

    extract_dir = tmp_path / "extract-escape"
    with zipfile.ZipFile(package_path, mode="r") as archive:
        archive.extractall(extract_dir)

    runtime_path = extract_dir / "runtime" / "anolis-runtime.yaml"
    payload = yaml.safe_load(runtime_path.read_text(encoding="utf-8"))
    payload["providers"][0]["args"] = ["--config", "../../outside.yaml"]
    runtime_path.write_text(yaml.safe_dump(payload, sort_keys=False), encoding="utf-8")
    _recompute_checksums(extract_dir)

    with pytest.raises(package_validator.PackageValidationError, match="path escapes package root"):
        package_validator.validate_package(extract_dir)


def _make_project(tmp_path: pathlib.Path, name: str) -> pathlib.Path:
    project = tmp_path / name
    project.mkdir(parents=True, exist_ok=True)

    system = {
        "schema_version": 1,
        "meta": {
            "name": name,
            "created": "2026-04-16T19:01:02+00:00",
            "template": "fixture",
        },
        "topology": {
            "runtime": {
                "name": name,
                "http_port": 8080,
                "http_bind": "127.0.0.1",
                "cors_origins": ["http://localhost:3000"],
                "cors_allow_credentials": False,
                "shutdown_timeout_ms": 2000,
                "startup_timeout_ms": 30000,
                "polling_interval_ms": 500,
                "log_level": "info",
                "telemetry": {
                    "enabled": True,
                    "influxdb": {
                        "url": "http://localhost:8086",
                        "org": "anolis",
                        "bucket": "anolis",
                        "token": "fixture-secret",
                    },
                },
                "automation_enabled": True,
                "behavior_tree_path": "behaviors/local.xml",
                "providers": [
                    {
                        "id": "sim0",
                        "kind": "sim",
                        "timeout_ms": 5000,
                        "hello_timeout_ms": 2000,
                        "ready_timeout_ms": 10000,
                        "restart_policy": {"enabled": False},
                    }
                ],
            },
            "providers": {
                "sim0": {
                    "kind": "sim",
                    "provider_name": "sim0",
                    "startup_policy": "degraded",
                    "simulation_mode": "non_interacting",
                    "tick_rate_hz": 10.0,
                    "devices": [
                        {"id": "tempctl0", "type": "tempctl", "initial_temp": 25.0},
                    ],
                }
            },
        },
        "paths": {
            "runtime_executable": "build/dev-release/core/anolis-runtime",
            "providers": {
                "sim0": {
                    "executable": "../anolis-provider-sim/build/dev-release/anolis-provider-sim",
                }
            },
        },
    }

    behavior_dir = project / "behaviors"
    behavior_dir.mkdir(parents=True, exist_ok=True)
    (behavior_dir / "local.xml").write_text("<root />\n", encoding="utf-8")
    (project / "system.json").write_text(json.dumps(system, indent=2), encoding="utf-8")
    return project


def _recompute_checksums(package_root: pathlib.Path) -> None:
    lines: list[str] = []
    for candidate in sorted(package_root.rglob("*")):
        if not candidate.is_file():
            continue
        rel = candidate.relative_to(package_root).as_posix()
        if rel == "meta/checksums.sha256":
            continue
        lines.append(f"{hashlib.sha256(candidate.read_bytes()).hexdigest()}  {rel}")
    (package_root / "meta" / "checksums.sha256").write_text("\n".join(lines) + "\n", encoding="utf-8")
