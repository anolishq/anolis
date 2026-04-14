"""Unit tests for backend/projects.py save-time validation behavior."""

import json
import pathlib
import sys

import pytest

# Ensure tools/system-composer/ is on path so 'backend' is importable
_SC_DIR = str(pathlib.Path(__file__).parent.parent.parent)
if _SC_DIR not in sys.path:
    sys.path.insert(0, _SC_DIR)

from backend import projects  # noqa: E402


def _load_template(name: str) -> dict:
    template_path = pathlib.Path(__file__).parent.parent.parent / "templates" / name / "system.json"
    return json.loads(template_path.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def test_save_project_rejects_schema_invalid_payload(monkeypatch: pytest.MonkeyPatch, tmp_path: pathlib.Path) -> None:
    monkeypatch.setattr(projects, "SYSTEMS_ROOT", tmp_path / "systems")

    with pytest.raises(projects.ProjectValidationError) as exc_info:
        projects.save_project("invalid-schema", {"schema_version": 1})

    errors = exc_info.value.errors
    assert any(err.get("source") == "schema" for err in errors), errors
    assert not (tmp_path / "systems" / "invalid-schema" / "system.json").exists()


def test_save_project_rejects_semantic_invalid_payload(monkeypatch: pytest.MonkeyPatch, tmp_path: pathlib.Path) -> None:
    monkeypatch.setattr(projects, "SYSTEMS_ROOT", tmp_path / "systems")
    system = _load_template("sim-quickstart")
    system["topology"]["runtime"]["providers"].append(
        {
            "id": "sim0",
            "kind": "sim",
            "timeout_ms": 5000,
            "hello_timeout_ms": 2000,
            "ready_timeout_ms": 10000,
            "restart_policy": {"enabled": False},
        }
    )

    with pytest.raises(projects.ProjectValidationError) as exc_info:
        projects.save_project("invalid-semantic", system)

    errors = exc_info.value.errors
    assert any(err.get("source") == "semantic" for err in errors), errors
    assert any("Duplicate provider IDs" in err.get("message", "") for err in errors), errors
    assert not (tmp_path / "systems" / "invalid-semantic" / "system.json").exists()


def test_save_project_writes_outputs_for_valid_payload(monkeypatch: pytest.MonkeyPatch, tmp_path: pathlib.Path) -> None:
    monkeypatch.setattr(projects, "SYSTEMS_ROOT", tmp_path / "systems")
    system = _load_template("sim-quickstart")

    projects.save_project("valid-project", system)

    project_root = tmp_path / "systems" / "valid-project"
    assert (project_root / "system.json").is_file()
    assert (project_root / "anolis-runtime.yaml").is_file()
    assert (project_root / "providers" / "sim0.yaml").is_file()
