"""Unit tests for path resolution and ANOLIS_DATA_DIR behavior."""

from __future__ import annotations

import importlib
import pathlib
import warnings


def _reload_paths_module():
    import anolis_composer_backend.paths as paths_module

    return importlib.reload(paths_module)


def test_systems_root_uses_anolis_data_dir_override(monkeypatch, tmp_path: pathlib.Path) -> None:
    override = tmp_path / "custom-systems-root"
    monkeypatch.setenv("ANOLIS_DATA_DIR", str(override))

    paths_module = _reload_paths_module()

    assert paths_module.SYSTEMS_ROOT == override.resolve()
    assert paths_module.DATA_ROOT == override.resolve().parent


def test_resolve_repo_path_prefers_data_root(monkeypatch, tmp_path: pathlib.Path) -> None:
    systems_root = tmp_path / "systems"
    data_root = systems_root.parent
    runtime_rel = pathlib.Path("bin") / "anolis-runtime"
    preferred_runtime = data_root / runtime_rel
    legacy_runtime = tmp_path / "legacy-repo" / runtime_rel

    preferred_runtime.parent.mkdir(parents=True, exist_ok=True)
    preferred_runtime.write_text("runtime", encoding="utf-8")
    legacy_runtime.parent.mkdir(parents=True, exist_ok=True)
    legacy_runtime.write_text("legacy-runtime", encoding="utf-8")

    monkeypatch.setenv("ANOLIS_DATA_DIR", str(systems_root))
    paths_module = _reload_paths_module()
    monkeypatch.setattr(paths_module, "REPO_ROOT", (tmp_path / "legacy-repo").resolve())
    monkeypatch.setattr(paths_module, "_LEGACY_PATH_WARNING_EMITTED", False)

    resolved = paths_module.resolve_repo_path(str(runtime_rel))
    assert resolved == preferred_runtime.resolve()


def test_resolve_repo_path_falls_back_to_legacy_with_warning(monkeypatch, tmp_path: pathlib.Path) -> None:
    systems_root = tmp_path / "systems"
    runtime_rel = pathlib.Path("build") / "dev-release" / "core" / "anolis-runtime"
    legacy_repo = tmp_path / "legacy-repo"
    legacy_runtime = legacy_repo / runtime_rel

    legacy_runtime.parent.mkdir(parents=True, exist_ok=True)
    legacy_runtime.write_text("legacy-runtime", encoding="utf-8")

    monkeypatch.setenv("ANOLIS_DATA_DIR", str(systems_root))
    paths_module = _reload_paths_module()
    monkeypatch.setattr(paths_module, "REPO_ROOT", legacy_repo.resolve())
    monkeypatch.setattr(paths_module, "_LEGACY_PATH_WARNING_EMITTED", False)

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        resolved = paths_module.resolve_repo_path(str(runtime_rel))

    assert resolved == legacy_runtime.resolve()
    assert any("legacy repo root" in str(item.message).lower() for item in caught)
