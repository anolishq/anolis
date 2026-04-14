#!/usr/bin/env python3
"""
Runtime HTTP OpenAPI structural validator.

Contracts-02 slice 1 scope:
1) Verify OpenAPI document shape and required metadata.
2) Verify required endpoint/method coverage.
3) Verify operation responses are present.
4) Verify internal $ref pointers resolve.
5) Verify /v0/events advertises text/event-stream.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'pyyaml' (pip install pyyaml)") from exc


@dataclass
class Failure:
    stage: str
    message: str


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


def _load_openapi(path: Path) -> dict:
    try:
        payload = yaml.load(path.read_text(encoding="utf-8"), Loader=_UniqueKeyLoader)
    except Exception as exc:
        raise SystemExit(f"ERROR: failed to parse OpenAPI file '{path}': {exc}") from exc

    if payload is None:
        raise SystemExit(f"ERROR: OpenAPI file '{path}' is empty")
    if not isinstance(payload, dict):
        raise SystemExit(f"ERROR: OpenAPI file '{path}' must contain a mapping root")
    return payload


def _iter_refs(node: Any) -> Iterable[str]:
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "$ref" and isinstance(value, str):
                yield value
            else:
                yield from _iter_refs(value)
    elif isinstance(node, list):
        for item in node:
            yield from _iter_refs(item)


def _resolve_json_pointer(document: dict, ref: str) -> bool:
    if not ref.startswith("#/"):
        return False
    current: Any = document
    parts = ref[2:].split("/")
    for part in parts:
        token = part.replace("~1", "/").replace("~0", "~")
        if isinstance(current, dict) and token in current:
            current = current[token]
        else:
            return False
    return True


def _validate_document_shape(doc: dict, failures: list[Failure]) -> None:
    openapi = doc.get("openapi")
    if not isinstance(openapi, str) or not openapi.startswith("3."):
        failures.append(Failure("shape", "openapi version must be a 3.x string"))

    info = doc.get("info")
    if not isinstance(info, dict):
        failures.append(Failure("shape", "missing required top-level object: info"))
    else:
        if not isinstance(info.get("title"), str) or not info.get("title"):
            failures.append(Failure("shape", "info.title must be a non-empty string"))
        if not isinstance(info.get("version"), str) or not info.get("version"):
            failures.append(Failure("shape", "info.version must be a non-empty string"))

    if not isinstance(doc.get("paths"), dict):
        failures.append(Failure("shape", "missing required top-level object: paths"))
    if not isinstance(doc.get("components"), dict):
        failures.append(Failure("shape", "missing required top-level object: components"))
    else:
        schemas = doc["components"].get("schemas")
        if not isinstance(schemas, dict) or not schemas:
            failures.append(Failure("shape", "components.schemas must be a non-empty object"))


def _validate_required_operations(doc: dict, failures: list[Failure]) -> None:
    paths = doc.get("paths", {})
    if not isinstance(paths, dict):
        return

    for method, path in REQUIRED_OPERATIONS:
        path_item = paths.get(path)
        if not isinstance(path_item, dict):
            failures.append(Failure("operations", f"missing required path: {path}"))
            continue

        operation = path_item.get(method)
        if not isinstance(operation, dict):
            failures.append(Failure("operations", f"missing required operation: {method.upper()} {path}"))
            continue

        responses = operation.get("responses")
        if not isinstance(responses, dict) or not responses:
            failures.append(Failure("operations", f"operation missing responses: {method.upper()} {path}"))


def _validate_internal_refs(doc: dict, failures: list[Failure]) -> None:
    refs = sorted(set(_iter_refs(doc)))
    unresolved: list[str] = []
    for ref in refs:
        if ref.startswith("#/") and not _resolve_json_pointer(doc, ref):
            unresolved.append(ref)
    for ref in unresolved:
        failures.append(Failure("refs", f"unresolved internal $ref: {ref}"))


def _validate_sse_contract(doc: dict, failures: list[Failure]) -> None:
    paths = doc.get("paths", {})
    if not isinstance(paths, dict):
        return

    events = paths.get("/v0/events")
    if not isinstance(events, dict):
        failures.append(Failure("sse", "missing /v0/events path"))
        return

    get_op = events.get("get")
    if not isinstance(get_op, dict):
        failures.append(Failure("sse", "missing GET /v0/events operation"))
        return

    responses = get_op.get("responses")
    if not isinstance(responses, dict):
        failures.append(Failure("sse", "GET /v0/events missing responses"))
        return

    ok = responses.get("200")
    if not isinstance(ok, dict):
        failures.append(Failure("sse", "GET /v0/events missing 200 response"))
        return

    content = ok.get("content")
    if not isinstance(content, dict) or "text/event-stream" not in content:
        failures.append(Failure("sse", "GET /v0/events 200 must advertise text/event-stream"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate runtime HTTP OpenAPI structural contract.")
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
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    spec_path = (repo_root / args.openapi).resolve()
    if not spec_path.is_file():
        print(f"ERROR: OpenAPI file not found: {spec_path}", file=sys.stderr)
        return 1

    document = _load_openapi(spec_path)
    failures: list[Failure] = []
    _validate_document_shape(document, failures)
    _validate_required_operations(document, failures)
    _validate_internal_refs(document, failures)
    _validate_sse_contract(document, failures)

    print("runtime-http openapi validation summary")
    print(f"  spec: {spec_path}")
    print(f"  required operations checked: {len(REQUIRED_OPERATIONS)}")

    if failures:
        print("\nFAILURES:")
        for failure in failures:
            print(f"  - [{failure.stage}] {failure.message}")
        return 1

    print("\nOpenAPI structural checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
