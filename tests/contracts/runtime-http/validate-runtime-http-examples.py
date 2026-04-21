#!/usr/bin/env python3
"""
Runtime HTTP example payload validator.

Contracts-02 slice 2 scope:
1) Validate checked-in example payload files against OpenAPI response schemas.
2) Verify required operation coverage by example manifest entries.
3) Fail fast on example/spec drift.
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'pyyaml' (pip install pyyaml)") from exc

try:
    import jsonschema
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'jsonschema' (pip install jsonschema)") from exc


REQUIRED_OPERATIONS: list[tuple[str, str]] = [
    ("get", "/v0/runtime/status"),
    ("get", "/v0/providers/health"),
    ("get", "/v0/devices"),
    ("get", "/v0/devices/{provider_id}/{device_id}/capabilities"),
    ("get", "/v0/state"),
    ("get", "/v0/state/{provider_id}/{device_id}"),
    ("post", "/v0/call"),
    ("get", "/v0/mode"),
    ("post", "/v0/mode"),
    ("get", "/v0/parameters"),
    ("post", "/v0/parameters"),
    ("get", "/v0/automation/tree"),
    ("get", "/v0/automation/status"),
    ("get", "/v0/events"),
]


@dataclass
class Failure:
    stage: str
    message: str


@dataclass
class ExampleEntry:
    method: str
    path: str
    status: str
    content_type: str
    file: str


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
    return Path(__file__).resolve().parents[3]


def _load_yaml_mapping(path: Path) -> dict[str, Any]:
    try:
        payload = yaml.load(path.read_text(encoding="utf-8"), Loader=_UniqueKeyLoader)
    except Exception as exc:
        raise SystemExit(f"ERROR: failed to parse YAML file '{path}': {exc}") from exc
    if payload is None:
        raise SystemExit(f"ERROR: YAML file '{path}' is empty")
    if not isinstance(payload, dict):
        raise SystemExit(f"ERROR: YAML file '{path}' must contain a mapping root")
    return payload


def _resolve_json_pointer(document: dict[str, Any], ref: str) -> Any:
    if not ref.startswith("#/"):
        raise ValueError(f"unsupported external $ref: {ref}")
    current: Any = document
    for part in ref[2:].split("/"):
        token = part.replace("~1", "/").replace("~0", "~")
        if isinstance(current, dict) and token in current:
            current = current[token]
        else:
            raise KeyError(f"unresolved internal ref '{ref}'")
    return current


def _expand_refs(node: Any, document: dict[str, Any]) -> Any:
    if isinstance(node, dict):
        if "$ref" in node:
            ref_val = node["$ref"]
            if not isinstance(ref_val, str):
                raise ValueError("$ref value must be a string")
            resolved = copy.deepcopy(_resolve_json_pointer(document, ref_val))
            siblings = {k: v for k, v in node.items() if k != "$ref"}
            expanded_resolved = _expand_refs(resolved, document)
            if siblings:
                if not isinstance(expanded_resolved, dict):
                    raise ValueError("$ref with sibling keys must resolve to object")
                out = dict(expanded_resolved)
                for key, value in siblings.items():
                    out[key] = _expand_refs(value, document)
                return out
            return expanded_resolved
        return {key: _expand_refs(value, document) for key, value in node.items()}
    if isinstance(node, list):
        return [_expand_refs(item, document) for item in node]
    return node


def _parse_manifest(manifest_path: Path) -> list[ExampleEntry]:
    data = _load_yaml_mapping(manifest_path)
    raw = data.get("examples")
    if not isinstance(raw, list) or not raw:
        raise SystemExit(f"ERROR: manifest '{manifest_path}' must contain a non-empty 'examples' list")

    out: list[ExampleEntry] = []
    for idx, item in enumerate(raw):
        if not isinstance(item, dict):
            raise SystemExit(f"ERROR: manifest entry #{idx + 1} must be an object")
        method = item.get("method")
        path = item.get("path")
        status = item.get("status")
        content_type = item.get("content_type")
        file_name = item.get("file")
        if not all(isinstance(v, str) and v for v in (method, path, status, content_type, file_name)):
            raise SystemExit(
                f"ERROR: manifest entry #{idx + 1} requires non-empty string fields: "
                "method, path, status, content_type, file"
            )
        assert isinstance(method, str)
        assert isinstance(path, str)
        assert isinstance(status, str)
        assert isinstance(content_type, str)
        assert isinstance(file_name, str)
        out.append(
            ExampleEntry(
                method=method.lower(),
                path=path,
                status=status,
                content_type=content_type,
                file=file_name,
            )
        )
    return out


def _validate_manifest_coverage(entries: Iterable[ExampleEntry], failures: list[Failure]) -> None:
    covered = {(entry.method, entry.path) for entry in entries}
    for op in REQUIRED_OPERATIONS:
        if op not in covered:
            failures.append(
                Failure("coverage", f"missing manifest example for required operation {op[0].upper()} {op[1]}")
            )


def _validate_example_entry(
    entry: ExampleEntry,
    examples_dir: Path,
    openapi_doc: dict[str, Any],
    failures: list[Failure],
) -> None:
    paths = openapi_doc.get("paths", {})
    if not isinstance(paths, dict):
        failures.append(Failure("openapi", "OpenAPI 'paths' is missing or invalid"))
        return

    path_item = paths.get(entry.path)
    if not isinstance(path_item, dict):
        failures.append(Failure("manifest", f"{entry.file}: path not found in OpenAPI: {entry.path}"))
        return

    operation = path_item.get(entry.method)
    if not isinstance(operation, dict):
        failures.append(
            Failure("manifest", f"{entry.file}: operation not found in OpenAPI: {entry.method.upper()} {entry.path}")
        )
        return

    responses = operation.get("responses")
    if not isinstance(responses, dict):
        failures.append(Failure("openapi", f"{entry.file}: operation has no responses block"))
        return

    response_obj: Any = responses.get(entry.status)
    if response_obj is None:
        failures.append(
            Failure(
                "manifest",
                f"{entry.file}: status {entry.status} not declared for {entry.method.upper()} {entry.path}",
            )
        )
        return

    if isinstance(response_obj, dict) and "$ref" in response_obj:
        try:
            response_obj = _resolve_json_pointer(openapi_doc, response_obj["$ref"])
        except Exception as exc:
            failures.append(Failure("openapi", f"{entry.file}: failed to resolve response $ref: {exc}"))
            return

    if not isinstance(response_obj, dict):
        failures.append(Failure("openapi", f"{entry.file}: response object is invalid"))
        return

    content = response_obj.get("content")
    if not isinstance(content, dict):
        failures.append(Failure("openapi", f"{entry.file}: response has no content block"))
        return

    media = content.get(entry.content_type)
    if not isinstance(media, dict):
        failures.append(
            Failure("manifest", f"{entry.file}: content type {entry.content_type!r} not declared in OpenAPI response")
        )
        return

    schema = media.get("schema")
    if schema is None:
        failures.append(Failure("openapi", f"{entry.file}: response media type has no schema"))
        return

    try:
        expanded_schema = _expand_refs(schema, openapi_doc)
    except Exception as exc:
        failures.append(Failure("openapi", f"{entry.file}: schema expansion failed: {exc}"))
        return

    payload_path = (examples_dir / entry.file).resolve()
    if not payload_path.is_file():
        failures.append(Failure("manifest", f"{entry.file}: file not found"))
        return

    try:
        if entry.content_type == "application/json":
            instance = json.loads(payload_path.read_text(encoding="utf-8"))
        else:
            instance = payload_path.read_text(encoding="utf-8")
    except Exception as exc:
        failures.append(Failure("examples", f"{entry.file}: failed to parse payload: {exc}"))
        return

    validator_cls = jsonschema.validators.validator_for(expanded_schema)
    validator = validator_cls(expanded_schema)
    errors = sorted(validator.iter_errors(instance), key=lambda err: list(err.path))
    if errors:
        first = errors[0]
        json_path = "$" if not first.path else "$." + ".".join(str(part) for part in first.path)
        failures.append(Failure("schema", f"{entry.file}: {json_path}: {first.message}"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate runtime HTTP OpenAPI examples.")
    parser.add_argument(
        "--repo-root",
        default=str(_repo_root_from_script()),
        help="Path to anolis repository root (default: auto-detected).",
    )
    parser.add_argument(
        "--openapi",
        default="schemas/http/runtime-http.openapi.v0.yaml",
        help="OpenAPI spec path relative to repo root.",
    )
    parser.add_argument(
        "--manifest",
        default="tests/contracts/runtime-http/examples/manifest.yaml",
        help="Example manifest path relative to repo root.",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    openapi_path = (repo_root / args.openapi).resolve()
    manifest_path = (repo_root / args.manifest).resolve()
    examples_dir = manifest_path.parent

    if not openapi_path.is_file():
        print(f"ERROR: OpenAPI file not found: {openapi_path}", file=sys.stderr)
        return 1
    if not manifest_path.is_file():
        print(f"ERROR: example manifest not found: {manifest_path}", file=sys.stderr)
        return 1

    openapi_doc = _load_yaml_mapping(openapi_path)
    entries = _parse_manifest(manifest_path)

    failures: list[Failure] = []
    _validate_manifest_coverage(entries, failures)
    for entry in entries:
        _validate_example_entry(entry, examples_dir, openapi_doc, failures)

    print("runtime-http example validation summary")
    print(f"  openapi: {openapi_path}")
    print(f"  manifest: {manifest_path}")
    print(f"  examples checked: {len(entries)}")
    print(f"  required operations checked: {len(REQUIRED_OPERATIONS)}")

    if failures:
        print("\nFAILURES:")
        for failure in failures:
            print(f"  - [{failure.stage}] {failure.message}")
        return 1

    print("\nOpenAPI example checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
