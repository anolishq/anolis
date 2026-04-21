#!/usr/bin/env python3
"""
Runtime config contract validator.

Runs two-layer checks:
1) JSON Schema validation
2) Runtime loader validation via `anolis-runtime --check-config`
"""

from __future__ import annotations

import argparse
import glob
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'pyyaml' (pip install pyyaml)") from exc

try:
    import jsonschema
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'jsonschema' (pip install jsonschema)") from exc


@dataclass
class Failure:
    stage: str
    path: Path
    message: str


def _repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def _glob_paths(root: Path, patterns: Sequence[str]) -> List[Path]:
    out: list[Path] = []
    seen: set[Path] = set()
    for pattern in patterns:
        for p in glob.glob(str(root / pattern), recursive=True):
            candidate = Path(p).resolve()
            if candidate not in seen and candidate.is_file():
                seen.add(candidate)
                out.append(candidate)
    out.sort()
    return out


def _format_json_path(error: jsonschema.ValidationError) -> str:
    if not error.path:
        return "$"
    return "$." + ".".join(str(part) for part in error.path)


def _load_schema(schema_path: Path) -> dict:
    try:
        return json.loads(schema_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"ERROR: schema json parse failed at {schema_path}: {exc}") from exc


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


def _build_validator(schema: dict) -> jsonschema.Validator:
    schema_draft = schema.get("$schema")
    expected_draft = "http://json-schema.org/draft-07/schema#"
    if schema_draft != expected_draft:
        raise SystemExit(f"ERROR: runtime schema draft mismatch. Expected '{expected_draft}', found '{schema_draft}'.")

    validator_cls = jsonschema.validators.validator_for(schema)
    try:
        validator_cls.check_schema(schema)
    except jsonschema.SchemaError as exc:
        raise SystemExit(f"ERROR: schema meta-validation failed: {exc}") from exc

    if validator_cls is not jsonschema.Draft7Validator:
        raise SystemExit("ERROR: runtime schema must validate with Draft7Validator in this wave.")

    return validator_cls(schema)


def _schema_validate(validator: jsonschema.Validator, config_path: Path) -> list[str]:
    try:
        payload = yaml.load(
            config_path.read_text(encoding="utf-8"),
            Loader=_UniqueKeyLoader,
        )
    except Exception as exc:
        return [f"yaml parse failed: {exc}"]

    if payload is None:
        payload = {}
    if not isinstance(payload, dict):
        return ["root must be a mapping/object"]

    errors = sorted(validator.iter_errors(payload), key=lambda e: list(e.path))
    return [f"{_format_json_path(err)}: {err.message}" for err in errors]


def _summarize_errors(errors: Sequence[str]) -> str:
    return " | ".join(errors)


def _resolve_runtime_binary(repo_root: Path, override: str | None) -> Path:
    if override:
        runtime = Path(override).expanduser().resolve()
        if not runtime.is_file():
            raise SystemExit(f"ERROR: runtime binary not found: {runtime}")
        return runtime

    candidates = [
        repo_root / "build/dev-release/core/anolis-runtime",
        repo_root / "build/dev-windows-release/core/Release/anolis-runtime.exe",
        repo_root / "build/ci-windows-release-strict/core/Release/anolis-runtime.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    in_path = shutil.which("anolis-runtime")
    if in_path:
        return Path(in_path).resolve()

    raise SystemExit("ERROR: anolis-runtime binary not found.\nBuild runtime first or pass --runtime-bin <path>.")


def _run_check_config(runtime_bin: Path, config_path: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [str(runtime_bin), "--check-config", str(config_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout.strip()


def _check_expected_schema_failures(
    validator: jsonschema.Validator,
    files: Iterable[Path],
    failures: list[Failure],
) -> None:
    for path in files:
        errs = _schema_validate(validator, path)
        if not errs:
            failures.append(
                Failure(
                    stage="schema",
                    path=path,
                    message="expected schema failure, but file passed schema validation",
                )
            )


def _check_expected_runtime_failures(
    validator: jsonschema.Validator,
    runtime_bin: Path,
    files: Iterable[Path],
    failures: list[Failure],
) -> None:
    for path in files:
        errs = _schema_validate(validator, path)
        if errs:
            failures.append(
                Failure(
                    stage="schema",
                    path=path,
                    message=(
                        "invalid/runtime fixture must be schema-valid and runtime-invalid. "
                        f"Schema errors: {_summarize_errors(errs)}"
                    ),
                )
            )
            continue
        rc, output = _run_check_config(runtime_bin, path)
        if rc == 0:
            failures.append(
                Failure(
                    stage="runtime",
                    path=path,
                    message="expected runtime check failure, but --check-config exited 0",
                )
            )
        elif not output:
            failures.append(
                Failure(
                    stage="runtime",
                    path=path,
                    message="runtime check failed as expected but produced no output",
                )
            )


def _check_expected_success(
    validator: jsonschema.Validator,
    runtime_bin: Path,
    files: Iterable[Path],
    failures: list[Failure],
) -> None:
    for path in files:
        errs = _schema_validate(validator, path)
        if errs:
            failures.append(
                Failure(
                    stage="schema",
                    path=path,
                    message=_summarize_errors(errs),
                )
            )
            continue
        rc, output = _run_check_config(runtime_bin, path)
        if rc != 0:
            failures.append(
                Failure(
                    stage="runtime",
                    path=path,
                    message=output or "runtime --check-config failed without output",
                )
            )


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Anolis runtime config contract.")
    parser.add_argument(
        "--repo-root",
        default=str(_repo_root_from_script()),
        help="Path to anolis repository root (default: auto-detected).",
    )
    parser.add_argument(
        "--schema",
        default="schemas/runtime/runtime-config.schema.json",
        help="Schema path relative to repo root.",
    )
    parser.add_argument(
        "--runtime-bin",
        default=None,
        help="Path to anolis-runtime binary. If omitted, common build paths are searched.",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    schema_path = (repo_root / args.schema).resolve()

    if not schema_path.is_file():
        print(f"ERROR: schema file not found: {schema_path}", file=sys.stderr)
        return 1

    schema = _load_schema(schema_path)
    validator = _build_validator(schema)

    tracked_runtime_configs = _glob_paths(
        repo_root,
        (
            "config/anolis-runtime*.yaml",
            "config/**/anolis-runtime*.yaml",
        ),
    )
    valid_fixtures = _glob_paths(repo_root, ("tests/contracts/runtime-config/valid/*.yaml",))
    invalid_schema_fixtures = _glob_paths(repo_root, ("tests/contracts/runtime-config/invalid/schema/*.yaml",))
    invalid_runtime_fixtures = _glob_paths(repo_root, ("tests/contracts/runtime-config/invalid/runtime/*.yaml",))

    if not tracked_runtime_configs:
        print(
            "ERROR: no runtime config files discovered from target patterns",
            file=sys.stderr,
        )
        return 1
    if not valid_fixtures:
        print("ERROR: no valid runtime-config fixtures discovered", file=sys.stderr)
        return 1
    if not invalid_schema_fixtures:
        print(
            "ERROR: no invalid/schema runtime-config fixtures discovered",
            file=sys.stderr,
        )
        return 1
    if not invalid_runtime_fixtures:
        print(
            "ERROR: no invalid/runtime runtime-config fixtures discovered",
            file=sys.stderr,
        )
        return 1

    runtime_bin = _resolve_runtime_binary(repo_root, args.runtime_bin)

    failures: list[Failure] = []
    _check_expected_success(validator, runtime_bin, [*tracked_runtime_configs, *valid_fixtures], failures)
    _check_expected_schema_failures(validator, invalid_schema_fixtures, failures)
    _check_expected_runtime_failures(validator, runtime_bin, invalid_runtime_fixtures, failures)

    print("runtime-config contract summary")
    print(f"  runtime configs checked: {len(tracked_runtime_configs)}")
    print(f"  valid fixtures checked: {len(valid_fixtures)}")
    print(f"  invalid/schema fixtures checked: {len(invalid_schema_fixtures)}")
    print(f"  invalid/runtime fixtures checked: {len(invalid_runtime_fixtures)}")
    print(f"  runtime binary: {runtime_bin}")

    if failures:
        print("\nFAILURES:")
        for f in failures:
            try:
                rel = f.path.relative_to(repo_root)
            except ValueError:
                rel = f.path
            print(f"  - [{f.stage}] {rel}: {f.message}")
        return 1

    print("\nAll runtime-config contract checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
