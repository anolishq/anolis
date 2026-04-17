"""Package v1 validator for commissioning handoff archives/directories."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import subprocess
import tempfile
import zipfile
from contextlib import contextmanager
from typing import Any, Iterator

import jsonschema
import yaml


class PackageValidationError(RuntimeError):
    """Raised when a handoff package fails validation."""


_WINDOWS_DRIVE_RE = re.compile(r"^[A-Za-z]:[\\/]")


def validate_package(package_path: pathlib.Path, runtime_bin: pathlib.Path | None = None) -> None:
    """Validate a package archive (.anpkg/.zip) or extracted package directory."""
    with _materialize_package_root(package_path) as package_root:
        _validate_structure(package_root)
        _validate_checksums(package_root)
        _assert_no_secret_leak(package_root)

        machine_schema = _load_schema("schemas/machine-profile.schema.json")
        runtime_schema = _load_schema("schemas/runtime-config.schema.json")
        machine_validator = jsonschema.Draft7Validator(machine_schema)
        runtime_validator = jsonschema.Draft7Validator(runtime_schema)

        manifest_path = package_root / "machine-profile.yaml"
        manifest_payload = _load_yaml_mapping(manifest_path)
        _validate_schema(machine_validator, manifest_payload, "machine-profile.yaml")
        _validate_manifest_refs(
            package_root=package_root,
            manifest_payload=manifest_payload,
            runtime_validator=runtime_validator,
        )
        _validate_replay(
            package_root=package_root,
            manifest_payload=manifest_payload,
            runtime_bin=runtime_bin,
        )


@contextmanager
def _materialize_package_root(package_path: pathlib.Path) -> Iterator[pathlib.Path]:
    path = package_path.resolve()
    if path.is_dir():
        yield path
        return

    if not path.is_file():
        raise PackageValidationError(f"Package path not found: {path}")

    suffix = path.suffix.lower()
    if suffix not in {".anpkg", ".zip"} and not zipfile.is_zipfile(path):
        raise PackageValidationError(f"Unsupported package format: {path}")

    with tempfile.TemporaryDirectory(prefix="anolis-package-validate-") as tmp_dir:
        root = pathlib.Path(tmp_dir).resolve()
        with zipfile.ZipFile(path, mode="r") as archive:
            archive.extractall(root)
        yield root


def _validate_structure(package_root: pathlib.Path) -> None:
    required_files = [
        "machine-profile.yaml",
        "runtime/anolis-runtime.yaml",
        "meta/provenance.json",
        "meta/checksums.sha256",
    ]
    for rel in required_files:
        if not (package_root / rel).is_file():
            raise PackageValidationError(f"Missing required package file: {rel}")

    providers_dir = package_root / "providers"
    if not providers_dir.is_dir():
        raise PackageValidationError("Missing required package directory: providers/")
    provider_files = sorted(providers_dir.glob("*.y*ml"))
    if not provider_files:
        raise PackageValidationError("No provider config files found under providers/")


def _validate_checksums(package_root: pathlib.Path) -> None:
    checksum_path = package_root / "meta" / "checksums.sha256"
    raw = checksum_path.read_text(encoding="utf-8")
    entries: dict[str, str] = {}
    for idx, line in enumerate(raw.splitlines(), start=1):
        text = line.strip()
        if text == "":
            continue
        parts = text.split("  ", 1)
        if len(parts) != 2:
            raise PackageValidationError(f"Invalid checksum line format at meta/checksums.sha256:{idx}")
        digest, rel_path = parts
        if rel_path in entries:
            raise PackageValidationError(f"Duplicate checksum entry for {rel_path}")
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise PackageValidationError(f"Invalid sha256 digest for {rel_path}")
        entries[rel_path] = digest

    discovered = sorted(_iter_package_files(package_root))
    expected = sorted(rel for rel in entries if rel != "meta/checksums.sha256")
    if discovered != expected:
        raise PackageValidationError(
            "Checksum file entries do not match package contents: "
            f"expected={expected}, discovered={discovered}"
        )

    for rel_path in discovered:
        payload = (package_root / rel_path).read_bytes()
        actual = hashlib.sha256(payload).hexdigest()
        expected_digest = entries.get(rel_path)
        if actual != expected_digest:
            raise PackageValidationError(f"Checksum mismatch for {rel_path}")


def _iter_package_files(package_root: pathlib.Path) -> list[str]:
    files: list[str] = []
    for candidate in sorted(package_root.rglob("*")):
        if not candidate.is_file():
            continue
        rel = candidate.relative_to(package_root).as_posix()
        if rel == "meta/checksums.sha256":
            continue
        files.append(rel)
    return files


def _assert_no_secret_leak(package_root: pathlib.Path) -> None:
    for rel in _iter_package_files(package_root):
        if not rel.endswith((".yaml", ".yml", ".json")):
            continue
        full = package_root / rel
        text = full.read_text(encoding="utf-8")
        if rel.endswith(".json"):
            payload = json.loads(text)
        else:
            payload = yaml.safe_load(text)
        for key_path, value in _iter_key_values(payload):
            if "token" in key_path.split(".")[-1].lower():
                if isinstance(value, str) and value.strip() != "":
                    raise PackageValidationError(f"Secret-like token value leaked at {rel}:{key_path}")


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


def _load_schema(rel_path: str) -> dict[str, Any]:
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    schema_path = (repo_root / rel_path).resolve()
    if not schema_path.is_file():
        raise PackageValidationError(f"Schema file not found: {schema_path}")
    payload = json.loads(schema_path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise PackageValidationError(f"Schema root must be object: {schema_path}")
    return payload


def _validate_schema(validator: jsonschema.Draft7Validator, payload: dict[str, Any], label: str) -> None:
    errors = sorted(validator.iter_errors(payload), key=lambda e: list(e.path))
    if errors:
        first = errors[0]
        path = "$." + ".".join(str(part) for part in first.path) if first.path else "$"
        raise PackageValidationError(f"Schema validation failed for {label}: {path}: {first.message}")


def _load_yaml_mapping(path: pathlib.Path) -> dict[str, Any]:
    payload = yaml.safe_load(path.read_text(encoding="utf-8"))
    if payload is None:
        return {}
    if not isinstance(payload, dict):
        raise PackageValidationError(f"YAML root must be object: {path}")
    return payload


def _validate_manifest_refs(
    *,
    package_root: pathlib.Path,
    manifest_payload: dict[str, Any],
    runtime_validator: jsonschema.Draft7Validator,
) -> None:
    providers = manifest_payload.get("providers", {})
    if not isinstance(providers, dict) or not providers:
        raise PackageValidationError("machine-profile.providers must be a non-empty object")
    manifest_provider_ids = set(providers.keys())

    runtime_profiles = manifest_payload.get("runtime_profiles", {})
    if not isinstance(runtime_profiles, dict) or "manual" not in runtime_profiles:
        raise PackageValidationError("machine-profile.runtime_profiles.manual is required")

    for profile_name, profile_rel in runtime_profiles.items():
        runtime_path = _resolve_required_path(
            package_root=package_root,
            raw=profile_rel,
            label=f"runtime_profiles.{profile_name}",
        )
        runtime_payload = _load_yaml_mapping(runtime_path)
        _validate_schema(runtime_validator, runtime_payload, runtime_path.relative_to(package_root).as_posix())
        runtime_provider_ids = _extract_runtime_provider_ids(runtime_payload)
        missing_in_manifest = sorted(runtime_provider_ids - manifest_provider_ids)
        missing_in_runtime = sorted(manifest_provider_ids - runtime_provider_ids)
        if missing_in_manifest:
            raise PackageValidationError(
                f"runtime_profiles.{profile_name}: provider IDs missing from manifest.providers: {missing_in_manifest}"
            )
        if missing_in_runtime:
            raise PackageValidationError(
                f"runtime_profiles.{profile_name}: manifest.providers IDs missing from runtime.providers: {missing_in_runtime}"
            )

        _validate_runtime_provider_args(package_root=package_root, runtime_payload=runtime_payload)
        _validate_runtime_behavior_path(package_root=package_root, runtime_payload=runtime_payload)

    for provider_id, provider_cfg in providers.items():
        if not isinstance(provider_cfg, dict):
            raise PackageValidationError(f"providers.{provider_id} must be an object")
        _resolve_required_path(
            package_root=package_root,
            raw=provider_cfg.get("config"),
            label=f"providers.{provider_id}.config",
        )

    behaviors = manifest_payload.get("behaviors", [])
    if behaviors is not None and not isinstance(behaviors, list):
        raise PackageValidationError("behaviors must be an array when present")
    for idx, behavior_rel in enumerate(behaviors or []):
        resolved = _resolve_required_path(
            package_root=package_root,
            raw=behavior_rel,
            label=f"behaviors[{idx}]",
        )
        rel = resolved.relative_to(package_root).as_posix()
        if not rel.startswith("runtime/behaviors/"):
            raise PackageValidationError(f"behaviors[{idx}] must be under runtime/behaviors/: {rel}")

    contracts = manifest_payload.get("contracts", {})
    if isinstance(contracts, dict):
        for key in ("runtime_config_baseline", "runtime_http_baseline"):
            _resolve_informational_path(
                package_root=package_root,
                raw=contracts.get(key),
                label=f"contracts.{key}",
            )

    validation = manifest_payload.get("validation", {})
    if isinstance(validation, dict):
        _resolve_informational_path(
            package_root=package_root,
            raw=validation.get("check_http_script"),
            label="validation.check_http_script",
        )


def _extract_runtime_provider_ids(runtime_payload: dict[str, Any]) -> set[str]:
    providers = runtime_payload.get("providers")
    if not isinstance(providers, list):
        raise PackageValidationError("runtime.providers must be an array")

    ids: set[str] = set()
    for idx, entry in enumerate(providers):
        if not isinstance(entry, dict):
            raise PackageValidationError(f"runtime.providers[{idx}] must be an object")
        provider_id = entry.get("id")
        if not isinstance(provider_id, str) or provider_id.strip() == "":
            raise PackageValidationError(f"runtime.providers[{idx}].id must be a non-empty string")
        ids.add(provider_id)
    return ids


def _validate_runtime_provider_args(*, package_root: pathlib.Path, runtime_payload: dict[str, Any]) -> None:
    providers = runtime_payload.get("providers", [])
    for idx, entry in enumerate(providers):
        if not isinstance(entry, dict):
            continue
        provider_id = entry.get("id", f"index-{idx}")
        args = entry.get("args")
        if not isinstance(args, list):
            raise PackageValidationError(f"runtime.providers[{idx}] args must be an array")
        token_idx = -1
        for i, value in enumerate(args[:-1]):
            if value == "--config":
                token_idx = i
                break
        if token_idx < 0:
            raise PackageValidationError(f"runtime provider '{provider_id}' missing --config argument")
        config_path = args[token_idx + 1]
        resolved = _resolve_required_path(
            package_root=package_root,
            raw=config_path,
            label=f"runtime.providers[{idx}].args(--config)",
        )
        rel = resolved.relative_to(package_root).as_posix()
        if not rel.startswith("providers/"):
            raise PackageValidationError(
                f"runtime provider '{provider_id}' config path must resolve under providers/: {config_path}"
            )


def _validate_runtime_behavior_path(*, package_root: pathlib.Path, runtime_payload: dict[str, Any]) -> None:
    automation = runtime_payload.get("automation")
    if not isinstance(automation, dict):
        return
    behavior_rel = automation.get("behavior_tree") or automation.get("behavior_tree_path")
    if not isinstance(behavior_rel, str) or behavior_rel.strip() == "":
        return
    resolved = _resolve_required_path(package_root=package_root, raw=behavior_rel, label="automation.behavior_tree")
    rel = resolved.relative_to(package_root).as_posix()
    if not rel.startswith("runtime/behaviors/"):
        raise PackageValidationError(f"automation.behavior_tree must resolve under runtime/behaviors/: {behavior_rel}")


def _validate_replay(
    *,
    package_root: pathlib.Path,
    manifest_payload: dict[str, Any],
    runtime_bin: pathlib.Path | None,
) -> None:
    runtime_profiles = manifest_payload.get("runtime_profiles", {})
    manual_rel = runtime_profiles.get("manual")
    runtime_path = _resolve_required_path(package_root=package_root, raw=manual_rel, label="runtime_profiles.manual")

    if runtime_bin is None:
        return

    runtime_path_bin = runtime_bin.resolve()
    if not runtime_path_bin.is_file():
        raise PackageValidationError(f"runtime binary not found: {runtime_path_bin}")

    rel_runtime = runtime_path.relative_to(package_root).as_posix()
    proc = subprocess.run(
        [str(runtime_path_bin), "--check-config", rel_runtime],
        cwd=str(package_root),
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        stderr = proc.stderr.strip()
        stdout = proc.stdout.strip()
        detail = stderr or stdout or f"exit {proc.returncode}"
        raise PackageValidationError(f"Runtime replay check failed for {rel_runtime}: {detail}")


def _resolve_required_path(*, package_root: pathlib.Path, raw: Any, label: str) -> pathlib.Path:
    if not isinstance(raw, str) or raw.strip() == "":
        raise PackageValidationError(f"{label}: path must be a non-empty string")
    path = _resolve_path(package_root=package_root, raw=raw, label=label)
    if not path.is_file():
        raise PackageValidationError(f"{label}: referenced file not found: {raw}")
    return path


def _resolve_informational_path(*, package_root: pathlib.Path, raw: Any, label: str) -> None:
    if raw in (None, ""):
        return
    if not isinstance(raw, str):
        raise PackageValidationError(f"{label}: path must be a string when present")
    path = _resolve_path(package_root=package_root, raw=raw, label=label)
    if path.exists() and not path.is_file():
        raise PackageValidationError(f"{label}: expected file path but found non-file entry: {raw}")


def _resolve_path(*, package_root: pathlib.Path, raw: str, label: str) -> pathlib.Path:
    if pathlib.Path(raw).is_absolute() or _WINDOWS_DRIVE_RE.match(raw):
        raise PackageValidationError(f"{label}: path must be relative (no absolute paths)")
    path = (package_root / raw).resolve()
    try:
        path.relative_to(package_root.resolve())
    except ValueError as exc:
        raise PackageValidationError(f"{label}: path escapes package root: {raw}") from exc
    return path
