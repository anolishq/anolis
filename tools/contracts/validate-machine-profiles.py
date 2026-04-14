#!/usr/bin/env python3
"""
Machine profile contract validator.

Runs three layers of checks:
1) machine-profile schema validation
2) machine-profile reference integrity validation
3) referenced runtime profile validation against runtime-config schema
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    import jsonschema
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'jsonschema' (pip install jsonschema)") from exc

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'pyyaml' (pip install pyyaml)") from exc

_WINDOWS_DRIVE_RE = re.compile(r"^[A-Za-z]:[\\/]")


@dataclass
class Failure:
    stage: str
    path: Path
    message: str


class _UniqueKeyLoader(yaml.SafeLoader):
    """PyYAML loader that rejects duplicate mapping keys."""


def _construct_mapping_no_duplicates(loader: _UniqueKeyLoader, node: yaml.Node, deep: bool = False) -> dict:
    mapping: dict = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping",
                node.start_mark,
                f"found duplicate key ({key!r})",
                key_node.start_mark,
            )
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


_UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    _construct_mapping_no_duplicates,
)


def _repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def _glob_paths(root: Path, patterns: Sequence[str]) -> list[Path]:
    out: list[Path] = []
    seen: set[Path] = set()
    for pattern in patterns:
        for entry in glob.glob(str(root / pattern), recursive=True):
            candidate = Path(entry).resolve()
            if candidate.is_file() and candidate not in seen:
                out.append(candidate)
                seen.add(candidate)
    out.sort()
    return out


def _format_json_path(error: jsonschema.ValidationError) -> str:
    if not error.path:
        return "$"
    return "$." + ".".join(str(part) for part in error.path)


def _load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"ERROR: json parse failed at {path}: {exc}") from exc


def _build_validator(schema: dict, *, expected_draft: str, label: str) -> jsonschema.Validator:
    schema_draft = schema.get("$schema")
    if schema_draft != expected_draft:
        raise SystemExit(f"ERROR: {label} schema draft mismatch. Expected '{expected_draft}', found '{schema_draft}'.")

    validator_cls = jsonschema.validators.validator_for(schema)
    try:
        validator_cls.check_schema(schema)
    except jsonschema.SchemaError as exc:
        raise SystemExit(f"ERROR: {label} schema meta-validation failed: {exc}") from exc

    if validator_cls is not jsonschema.Draft7Validator:
        raise SystemExit(f"ERROR: {label} schema must validate with Draft7Validator in this wave.")

    return validator_cls(schema)


def _load_yaml_file(path: Path) -> dict:
    payload = yaml.load(path.read_text(encoding="utf-8"), Loader=_UniqueKeyLoader)
    if payload is None:
        return {}
    if not isinstance(payload, dict):
        raise ValueError("root must be a mapping/object")
    return payload


def _schema_validate(validator: jsonschema.Validator, path: Path) -> tuple[dict | None, list[str]]:
    try:
        payload = _load_yaml_file(path)
    except Exception as exc:
        return None, [f"yaml parse failed: {exc}"]

    errors = sorted(validator.iter_errors(payload), key=lambda e: list(e.path))
    return payload, [f"{_format_json_path(err)}: {err.message}" for err in errors]


def _is_relative_path(raw: str) -> bool:
    return bool(raw) and not Path(raw).is_absolute() and not _WINDOWS_DRIVE_RE.match(raw)


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _resolve_path(repo_root: Path, manifest_path: Path, raw: str, *, base: str) -> Path:
    if base == "repo":
        return (repo_root / raw).resolve()
    return (manifest_path.parent / raw).resolve()


def _validate_path_reference(
    *,
    repo_root: Path,
    manifest_path: Path,
    raw: str,
    label: str,
    base: str = "manifest",
) -> tuple[Path | None, list[str]]:
    if not isinstance(raw, str) or not raw.strip():
        return None, [f"{label}: path must be a non-empty string"]
    if not _is_relative_path(raw):
        return None, [f"{label}: path must be relative (no absolute paths)"]

    resolved = _resolve_path(repo_root, manifest_path, raw, base=base)
    if not _is_within(resolved, repo_root):
        return None, [f"{label}: path escapes repository root: {raw}"]
    if not resolved.is_file():
        return resolved, [f"{label}: referenced file not found: {raw}"]
    return resolved, []


def _validate_runtime_profile_schema(
    *,
    runtime_validator: jsonschema.Validator,
    runtime_path: Path,
    manifest_path: Path,
) -> list[str]:
    payload, schema_errors = _schema_validate(runtime_validator, runtime_path)
    if payload is None:
        return [f"runtime profile '{runtime_path.name}' parse failed: {schema_errors[0]}"]
    if schema_errors:
        return [f"runtime profile '{runtime_path.name}' schema error: {error}" for error in schema_errors]
    return []


def _validate_manifest_refs(
    *,
    repo_root: Path,
    manifest_path: Path,
    payload: dict,
    runtime_validator: jsonschema.Validator,
) -> list[str]:
    errors: list[str] = []

    runtime_profiles = payload.get("runtime_profiles", {})
    for profile_name, profile_path in runtime_profiles.items():
        resolved, path_errors = _validate_path_reference(
            repo_root=repo_root,
            manifest_path=manifest_path,
            raw=profile_path,
            label=f"runtime_profiles.{profile_name}",
            base="manifest",
        )
        errors.extend(path_errors)
        if resolved is not None and not path_errors:
            errors.extend(
                _validate_runtime_profile_schema(
                    runtime_validator=runtime_validator,
                    runtime_path=resolved,
                    manifest_path=manifest_path,
                )
            )

    providers = payload.get("providers", {})
    for provider_id, provider_cfg in providers.items():
        provider_path = provider_cfg.get("config")
        _, path_errors = _validate_path_reference(
            repo_root=repo_root,
            manifest_path=manifest_path,
            raw=provider_path,
            label=f"providers.{provider_id}.config",
            base="manifest",
        )
        errors.extend(path_errors)

    for index, behavior_path in enumerate(payload.get("behaviors", [])):
        _, path_errors = _validate_path_reference(
            repo_root=repo_root,
            manifest_path=manifest_path,
            raw=behavior_path,
            label=f"behaviors[{index}]",
            base="manifest",
        )
        errors.extend(path_errors)

    contracts = payload.get("contracts", {})
    for key in ("runtime_config_baseline", "runtime_http_baseline"):
        _, path_errors = _validate_path_reference(
            repo_root=repo_root,
            manifest_path=manifest_path,
            raw=contracts.get(key, ""),
            label=f"contracts.{key}",
            base="repo",
        )
        errors.extend(path_errors)

    validation = payload.get("validation", {})
    check_http_script = validation.get("check_http_script")
    if isinstance(check_http_script, str) and check_http_script:
        _, path_errors = _validate_path_reference(
            repo_root=repo_root,
            manifest_path=manifest_path,
            raw=check_http_script,
            label="validation.check_http_script",
            base="repo",
        )
        errors.extend(path_errors)

    expected_providers = validation.get("expected_providers")
    if isinstance(expected_providers, list):
        for provider_id in expected_providers:
            if provider_id not in providers:
                errors.append(
                    f"validation.expected_providers contains provider not defined in providers: {provider_id}"
                )

    compatibility = payload.get("compatibility", {})
    compatibility_providers = compatibility.get("providers", {})
    provider_keys = set(providers.keys())
    compatibility_keys = set(compatibility_providers.keys())
    missing_compat = sorted(provider_keys - compatibility_keys)
    extra_compat = sorted(compatibility_keys - provider_keys)
    if missing_compat:
        errors.append("compatibility.providers missing entries for provider IDs: " + ", ".join(missing_compat))
    if extra_compat:
        errors.append("compatibility.providers has unknown provider IDs: " + ", ".join(extra_compat))

    return errors


def _check_expected_success(
    *,
    machine_validator: jsonschema.Validator,
    runtime_validator: jsonschema.Validator,
    files: Iterable[Path],
    repo_root: Path,
    failures: list[Failure],
) -> None:
    for path in files:
        payload, schema_errors = _schema_validate(machine_validator, path)
        if schema_errors:
            failures.append(
                Failure(
                    stage="schema",
                    path=path,
                    message=" | ".join(schema_errors),
                )
            )
            continue
        assert payload is not None

        ref_errors = _validate_manifest_refs(
            repo_root=repo_root,
            manifest_path=path,
            payload=payload,
            runtime_validator=runtime_validator,
        )
        if ref_errors:
            failures.append(
                Failure(
                    stage="references",
                    path=path,
                    message=" | ".join(ref_errors),
                )
            )


def _check_expected_schema_failures(
    *,
    machine_validator: jsonschema.Validator,
    files: Iterable[Path],
    failures: list[Failure],
) -> None:
    for path in files:
        _, schema_errors = _schema_validate(machine_validator, path)
        if not schema_errors:
            failures.append(
                Failure(
                    stage="schema",
                    path=path,
                    message="expected schema failure, but file passed schema validation",
                )
            )


def _check_expected_reference_failures(
    *,
    machine_validator: jsonschema.Validator,
    runtime_validator: jsonschema.Validator,
    files: Iterable[Path],
    repo_root: Path,
    failures: list[Failure],
) -> None:
    for path in files:
        payload, schema_errors = _schema_validate(machine_validator, path)
        if schema_errors:
            failures.append(
                Failure(
                    stage="schema",
                    path=path,
                    message=(
                        "invalid/references fixture must be schema-valid and reference-invalid. "
                        f"Schema errors: {' | '.join(schema_errors)}"
                    ),
                )
            )
            continue
        assert payload is not None
        ref_errors = _validate_manifest_refs(
            repo_root=repo_root,
            manifest_path=path,
            payload=payload,
            runtime_validator=runtime_validator,
        )
        if not ref_errors:
            failures.append(
                Failure(
                    stage="references",
                    path=path,
                    message="expected reference failure, but reference checks passed",
                )
            )


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Anolis machine profile contracts.")
    parser.add_argument(
        "--repo-root",
        default=str(_repo_root_from_script()),
        help="Path to anolis repository root (default: auto-detected).",
    )
    parser.add_argument(
        "--machine-schema",
        default="schemas/machine-profile.schema.json",
        help="Machine profile schema path relative to repo root.",
    )
    parser.add_argument(
        "--runtime-schema",
        default="schemas/runtime-config.schema.json",
        help="Runtime config schema path relative to repo root.",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    machine_schema_path = (repo_root / args.machine_schema).resolve()
    runtime_schema_path = (repo_root / args.runtime_schema).resolve()
    if not machine_schema_path.is_file():
        print(f"ERROR: machine profile schema file not found: {machine_schema_path}", file=sys.stderr)
        return 1
    if not runtime_schema_path.is_file():
        print(f"ERROR: runtime config schema file not found: {runtime_schema_path}", file=sys.stderr)
        return 1

    machine_schema = _load_json(machine_schema_path)
    runtime_schema = _load_json(runtime_schema_path)
    machine_validator = _build_validator(
        machine_schema,
        expected_draft="http://json-schema.org/draft-07/schema#",
        label="machine profile",
    )
    runtime_validator = _build_validator(
        runtime_schema,
        expected_draft="http://json-schema.org/draft-07/schema#",
        label="runtime config",
    )

    tracked_manifests = _glob_paths(repo_root, ("config/**/machine-profile.yaml",))
    valid_fixtures = _glob_paths(repo_root, ("tests/contracts/machine-profile/valid/*.yaml",))
    invalid_schema_fixtures = _glob_paths(repo_root, ("tests/contracts/machine-profile/invalid/schema/*.yaml",))
    invalid_reference_fixtures = _glob_paths(repo_root, ("tests/contracts/machine-profile/invalid/references/*.yaml",))

    if not tracked_manifests:
        print("ERROR: no machine profile manifests discovered under config/**/machine-profile.yaml", file=sys.stderr)
        return 1
    if not valid_fixtures:
        print("ERROR: no valid machine-profile fixtures discovered", file=sys.stderr)
        return 1
    if not invalid_schema_fixtures:
        print("ERROR: no invalid/schema machine-profile fixtures discovered", file=sys.stderr)
        return 1
    if not invalid_reference_fixtures:
        print("ERROR: no invalid/references machine-profile fixtures discovered", file=sys.stderr)
        return 1

    failures: list[Failure] = []
    _check_expected_success(
        machine_validator=machine_validator,
        runtime_validator=runtime_validator,
        files=[*tracked_manifests, *valid_fixtures],
        repo_root=repo_root,
        failures=failures,
    )
    _check_expected_schema_failures(
        machine_validator=machine_validator,
        files=invalid_schema_fixtures,
        failures=failures,
    )
    _check_expected_reference_failures(
        machine_validator=machine_validator,
        runtime_validator=runtime_validator,
        files=invalid_reference_fixtures,
        repo_root=repo_root,
        failures=failures,
    )

    print("machine-profile contract summary")
    print(f"  tracked manifests checked: {len(tracked_manifests)}")
    print(f"  valid fixtures checked: {len(valid_fixtures)}")
    print(f"  invalid/schema fixtures checked: {len(invalid_schema_fixtures)}")
    print(f"  invalid/references fixtures checked: {len(invalid_reference_fixtures)}")

    if failures:
        print("\nFAILURES:")
        for failure in failures:
            try:
                rel = failure.path.relative_to(repo_root)
            except ValueError:
                rel = failure.path
            print(f"  - [{failure.stage}] {rel}: {failure.message}")
        return 1

    print("\nAll machine-profile contract checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
