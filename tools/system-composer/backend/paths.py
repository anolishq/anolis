"""Path helpers for System Composer.

This module keeps repo-relative paths centralized so backend modules do not
duplicate hardcoded string literals or depend on the caller's CWD.
"""

from __future__ import annotations

import os
import pathlib
import warnings

BACKEND_DIR = pathlib.Path(__file__).resolve().parent
COMPOSER_DIR = BACKEND_DIR.parent
TOOLS_DIR = COMPOSER_DIR.parent
REPO_ROOT = TOOLS_DIR.parent

_LEGACY_SYSTEMS_ROOT = REPO_ROOT / "systems"
_DEFAULT_SYSTEMS_ROOT = pathlib.Path.home() / ".anolis" / "systems"
_LEGACY_PATH_WARNING_EMITTED = False


def _resolve_systems_root() -> pathlib.Path:
    """Resolve project storage root with env override + developer fallback."""
    override = os.getenv("ANOLIS_DATA_DIR")
    if override and override.strip():
        return pathlib.Path(override).expanduser().resolve()
    if _LEGACY_SYSTEMS_ROOT.exists():
        return _LEGACY_SYSTEMS_ROOT.resolve()
    return _DEFAULT_SYSTEMS_ROOT.resolve()


SYSTEMS_ROOT = _resolve_systems_root()
DATA_ROOT = SYSTEMS_ROOT.parent
TEMPLATES_ROOT = COMPOSER_DIR / "templates"
FRONTEND_DIR = COMPOSER_DIR / "frontend"
CATALOG_PATH = COMPOSER_DIR / "catalog" / "providers.json"
SYSTEM_SCHEMA_PATH = COMPOSER_DIR / "schema" / "system.schema.json"


def resolve_repo_path(path_value: str) -> pathlib.Path:
    """Resolve executable/config paths with ANOLIS_DATA_DIR-first semantics."""
    global _LEGACY_PATH_WARNING_EMITTED

    path = pathlib.Path(path_value).expanduser()
    if path.is_absolute():
        return path

    preferred = (DATA_ROOT / path).resolve()
    if preferred.exists():
        return preferred

    systems_relative = (SYSTEMS_ROOT / path).resolve()
    if systems_relative.exists():
        return systems_relative

    legacy = (REPO_ROOT / path).resolve()
    if legacy.exists():
        if not _LEGACY_PATH_WARNING_EMITTED:
            warnings.warn(
                "Resolved executable path against legacy repo root. "
                "Update system paths to ANOLIS_DATA_DIR-relative or absolute paths.",
                RuntimeWarning,
                stacklevel=2,
            )
            _LEGACY_PATH_WARNING_EMITTED = True
        return legacy

    return preferred
