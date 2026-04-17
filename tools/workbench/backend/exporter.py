"""Deterministic commissioning handoff package exporter.

Core export logic with no HTTP/UI dependency. Thin wrappers in server.py and
tools/package.py should call build_package().
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import sys
import zipfile
from datetime import datetime, timezone
from typing import Any

import jsonschema
import yaml

# Ensure tools/system-composer is importable as `backend`.
_WB_DIR = pathlib.Path(__file__).resolve().parent.parent
_SC_DIR = _WB_DIR.parent / "system-composer"
if str(_SC_DIR) not in sys.path:
    sys.path.insert(0, str(_SC_DIR))

from backend import paths as paths_module  # noqa: E402
from backend import renderer as renderer_module  # noqa: E402


class ExportError(RuntimeError):
    """Raised when export cannot produce a valid package."""


_ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)
_MACHINE_ID_RE = re.compile(r"[^a-z0-9-]+")
_MACHINE_PROFILE_SCHEMA_PATH = paths_module.REPO_ROOT / "schemas" / "machine-profile.schema.json"
_MACHINE_PROFILE_SCHEMA_CACHE: "dict[str, Any] | None" = None


def build_package(project_dir: pathlib.Path, out_path: pathlib.Path) -> None:
    """Build a deterministic .anpkg zip archive from a commissioned project."""
    project_root = project_dir.resolve()
    system_path = project_root / "system.json"
    if not system_path.is_file():
        raise ExportError(f"Project file not found: {system_path}")

    system = _load_system_json(system_path)
    project_name = project_root.name
    rendered = renderer_module.render(system, project_name)
    runtime_yaml = rendered.get("anolis-runtime.yaml")
    if not isinstance(runtime_yaml, str) or runtime_yaml.strip() == "":
        raise ExportError("Renderer did not produce anolis-runtime.yaml")

    runtime_payload = yaml.safe_load(runtime_yaml) or {}
    if not isinstance(runtime_payload, dict):
        raise ExportError("Rendered runtime YAML root must be a mapping/object")

    provider_ids = _rewrite_provider_args(runtime_payload)
    provider_files = _collect_provider_files(rendered, project_root, provider_ids)
    behavior_rel_paths = _rewrite_and_collect_behaviors(
        runtime_payload=runtime_payload,
        project_dir=project_root,
    )

    redaction_applied = _redact_runtime_secrets(runtime_payload)
    runtime_text = yaml.safe_dump(runtime_payload, sort_keys=False)

    machine_profile = _build_machine_profile(
        system=system,
        project_name=project_name,
        provider_ids=provider_ids,
        behavior_rel_paths=behavior_rel_paths,
    )
    _validate_machine_profile(machine_profile)
    machine_profile_text = yaml.safe_dump(machine_profile, sort_keys=False)

    exported_at = _deterministic_exported_at(system)
    provenance = {
        "exported_at": exported_at,
        "schema_versions": {
            "machine-profile": 1,
            "runtime-config": 1,
        },
        "package_format_version": 1,
        "source_project": project_name,
        "redaction_policy": {
            "telemetry_influxdb_token_removed": redaction_applied,
            "deploy_time_env_var": "INFLUXDB_TOKEN",
        },
    }
    provenance_text = json.dumps(provenance, indent=2, sort_keys=True) + "\n"

    files: dict[str, bytes] = {
        "machine-profile.yaml": machine_profile_text.encode("utf-8"),
        "runtime/anolis-runtime.yaml": runtime_text.encode("utf-8"),
        "meta/provenance.json": provenance_text.encode("utf-8"),
    }

    for rel_path, content in provider_files.items():
        files[rel_path] = content

    for rel_path, source_path in behavior_rel_paths.items():
        files[rel_path] = source_path.read_bytes()

    _assert_no_secret_leak(files)
    files["meta/checksums.sha256"] = _checksums_file_bytes(files)
    _write_zip_deterministic(files=files, out_path=out_path.resolve())


def _load_system_json(system_path: pathlib.Path) -> dict[str, Any]:
    try:
        payload = json.loads(system_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        raise ExportError(f"Failed reading {system_path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ExportError("system.json root must be an object")
    return payload


def _rewrite_provider_args(runtime_payload: dict[str, Any]) -> list[str]:
    providers = runtime_payload.get("providers")
    if not isinstance(providers, list) or not providers:
        raise ExportError("runtime.providers must be a non-empty list")

    provider_ids: list[str] = []
    seen: set[str] = set()
    for entry in providers:
        if not isinstance(entry, dict):
            raise ExportError("runtime.providers entries must be objects")
        provider_id = entry.get("id")
        if not isinstance(provider_id, str) or provider_id.strip() == "":
            raise ExportError("runtime.providers[].id must be a non-empty string")
        provider_id = provider_id.strip()
        if provider_id in seen:
            raise ExportError(f"Duplicate provider id in runtime.providers: {provider_id}")
        seen.add(provider_id)
        provider_ids.append(provider_id)

        expected_cfg = f"providers/{provider_id}.yaml"
        raw_args = entry.get("args")
        args = [str(item) for item in raw_args] if isinstance(raw_args, list) else []
        replaced = False
        for idx, token in enumerate(args[:-1]):
            if token == "--config":
                args[idx + 1] = expected_cfg
                replaced = True
                break
        if not replaced:
            args.extend(["--config", expected_cfg])
        entry["args"] = args

    return provider_ids


def _collect_provider_files(
    rendered: dict[str, str],
    project_dir: pathlib.Path,
    provider_ids: list[str],
) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for provider_id in provider_ids:
        rel = f"providers/{provider_id}.yaml"
        rendered_text = rendered.get(rel)
        if isinstance(rendered_text, str) and rendered_text.strip() != "":
            files[rel] = rendered_text.encode("utf-8")
            continue

        fallback = project_dir / rel
        if fallback.is_file():
            files[rel] = fallback.read_bytes()
            continue

        raise ExportError(f"Provider config not found for {provider_id}: expected '{rel}'")
    return files


def _rewrite_and_collect_behaviors(
    *,
    runtime_payload: dict[str, Any],
    project_dir: pathlib.Path,
) -> dict[str, pathlib.Path]:
    behavior_files: dict[str, pathlib.Path] = {}
    automation = runtime_payload.get("automation")
    if not isinstance(automation, dict):
        return behavior_files

    behavior_ref = automation.get("behavior_tree") or automation.get("behavior_tree_path")
    if not isinstance(behavior_ref, str) or behavior_ref.strip() == "":
        return behavior_files

    source = _resolve_behavior_path(behavior_ref.strip(), project_dir)
    rel = f"runtime/behaviors/{source.name}"
    automation["behavior_tree"] = rel
    automation.pop("behavior_tree_path", None)
    behavior_files[rel] = source
    return behavior_files


def _resolve_behavior_path(raw: str, project_dir: pathlib.Path) -> pathlib.Path:
    raw_path = pathlib.Path(raw)
    if raw_path.is_absolute():
        raise ExportError("automation.behavior_tree must be a relative path")

    repo_root = paths_module.REPO_ROOT.resolve()
    candidates = [
        (project_dir / raw_path).resolve(),
        (repo_root / raw_path).resolve(),
    ]
    for candidate in candidates:
        if candidate.is_file() and (_is_within(candidate, project_dir) or _is_within(candidate, repo_root)):
            return candidate
    raise ExportError(f"Behavior tree file not found for automation.behavior_tree='{raw}'")


def _is_within(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _redact_runtime_secrets(runtime_payload: dict[str, Any]) -> bool:
    redacted = False
    telemetry = runtime_payload.get("telemetry")
    if not isinstance(telemetry, dict):
        return redacted

    if "influx_token" in telemetry:
        telemetry.pop("influx_token", None)
        redacted = True

    influxdb = telemetry.get("influxdb")
    if isinstance(influxdb, dict) and "token" in influxdb:
        influxdb.pop("token", None)
        redacted = True

    return redacted


def _build_machine_profile(
    *,
    system: dict[str, Any],
    project_name: str,
    provider_ids: list[str],
    behavior_rel_paths: dict[str, pathlib.Path],
) -> dict[str, Any]:
    meta = system.get("meta")
    display_name = project_name
    if isinstance(meta, dict):
        raw = meta.get("name")
        if isinstance(raw, str) and raw.strip() != "":
            display_name = raw.strip()

    machine_id = _machine_id_from_name(project_name)
    providers = {
        provider_id: {"config": f"providers/{provider_id}.yaml"} for provider_id in provider_ids
    }
    compatibility_providers = {
        provider_id: {"strategy": "local-build", "version": "unspecified"} for provider_id in provider_ids
    }

    payload: dict[str, Any] = {
        "schema_version": 1,
        "machine_id": machine_id,
        "display_name": display_name,
        "runtime_profiles": {
            "manual": "runtime/anolis-runtime.yaml",
        },
        "providers": providers,
        "validation": {
            "expected_providers": provider_ids,
        },
        "contracts": {
            "runtime_config_baseline": "docs/contracts/runtime-config-baseline.md",
            "runtime_http_baseline": "docs/contracts/runtime-http-baseline.md",
        },
        "compatibility": {
            "runtime": {
                "config_contract": "01-runtime-config",
                "http_contract": "02-runtime-http",
            },
            "providers": compatibility_providers,
        },
    }
    if behavior_rel_paths:
        payload["behaviors"] = sorted(behavior_rel_paths.keys())
    return payload


def _validate_machine_profile(profile: dict[str, Any]) -> None:
    global _MACHINE_PROFILE_SCHEMA_CACHE
    if _MACHINE_PROFILE_SCHEMA_CACHE is None:
        _MACHINE_PROFILE_SCHEMA_CACHE = json.loads(
            _MACHINE_PROFILE_SCHEMA_PATH.read_text(encoding="utf-8")
        )
    validator = jsonschema.Draft7Validator(_MACHINE_PROFILE_SCHEMA_CACHE)
    errors = sorted(validator.iter_errors(profile), key=lambda e: list(e.path))
    if errors:
        msgs = "; ".join(f"{'.' + '.'.join(str(p) for p in e.path) if e.path else '$'}: {e.message}" for e in errors)
        raise ExportError(f"Generated machine-profile failed schema validation: {msgs}")


def _machine_id_from_name(name: str) -> str:
    lowered = name.strip().lower().replace("_", "-")
    cleaned = _MACHINE_ID_RE.sub("-", lowered)
    cleaned = re.sub(r"-{2,}", "-", cleaned).strip("-")
    if cleaned == "":
        cleaned = "machine"
    if not cleaned[0].isalnum():
        cleaned = f"m-{cleaned}"
    return cleaned


def _deterministic_exported_at(system: dict[str, Any]) -> str:
    meta = system.get("meta")
    if isinstance(meta, dict):
        created = meta.get("created")
        if isinstance(created, str):
            parsed = _normalize_iso_timestamp(created)
            if parsed is not None:
                return parsed
    return "1970-01-01T00:00:00Z"


def _normalize_iso_timestamp(raw: str) -> str | None:
    text = raw.strip()
    if text == "":
        return None
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    else:
        parsed = parsed.astimezone(timezone.utc)
    return parsed.replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _assert_no_secret_leak(files: dict[str, bytes]) -> None:
    for rel_path, content in sorted(files.items()):
        if rel_path.endswith(".xml"):
            continue

        text = content.decode("utf-8")
        if rel_path.endswith(".yaml") or rel_path.endswith(".yml"):
            payload = yaml.safe_load(text)
        elif rel_path.endswith(".json"):
            payload = json.loads(text)
        else:
            continue

        for key_path, value in _iter_key_values(payload):
            if "token" in key_path.split(".")[-1].lower():
                if isinstance(value, str) and value.strip() != "":
                    raise ExportError(f"Secret-like token value leaked in package file '{rel_path}' at {key_path}")


def _iter_key_values(payload: Any, prefix: str = "$"):
    if isinstance(payload, dict):
        for key, value in payload.items():
            child = f"{prefix}.{key}" if prefix else str(key)
            yield from _iter_key_values(value, child)
        return
    if isinstance(payload, list):
        for idx, value in enumerate(payload):
            child = f"{prefix}[{idx}]"
            yield from _iter_key_values(value, child)
        return
    yield prefix, payload


def _checksums_file_bytes(files: dict[str, bytes]) -> bytes:
    lines: list[str] = []
    for rel_path in sorted(path for path in files if path != "meta/checksums.sha256"):
        digest = hashlib.sha256(files[rel_path]).hexdigest()
        lines.append(f"{digest}  {rel_path}")
    return ("\n".join(lines) + "\n").encode("utf-8")


def _write_zip_deterministic(*, files: dict[str, bytes], out_path: pathlib.Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")

    with zipfile.ZipFile(tmp_path, mode="w") as archive:
        for rel_path in sorted(files):
            info = zipfile.ZipInfo(filename=rel_path, date_time=_ZIP_EPOCH)
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = (0o100644 & 0xFFFF) << 16
            archive.writestr(info, files[rel_path])

    tmp_path.replace(out_path)
