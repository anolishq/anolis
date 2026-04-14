#!/usr/bin/env python3
"""
Runtime HTTP live conformance validator.

Contracts-02 slice 2 scope:
1) Start runtime + provider-sim fixture.
2) Exercise required `/v0` operations.
3) Validate each live response against the OpenAPI schema declared for its actual status code.
"""

from __future__ import annotations

import argparse
import copy
import json
import socket
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
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

try:
    import requests
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ERROR: missing dependency 'requests' (pip install requests)") from exc


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
class CaptureEntry:
    name: str
    method: str
    path_template: str
    actual_path: str
    status_code: int
    content_type: str
    body_file: str


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


def _pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        return int(sock.getsockname()[1])


def _first_existing(paths: Iterable[Path]) -> Path | None:
    for path in paths:
        if path.exists() and path.is_file():
            return path.resolve()
    return None


def _runtime_candidates(root: Path) -> list[Path]:
    names = ["anolis-runtime.exe", "anolis-runtime"]
    build_root = root / "build"
    preset_dirs = [
        "dev-release",
        "dev-debug",
        "dev-windows-release",
        "dev-windows-debug",
        "ci-linux-release",
        "ci-linux-release-strict",
        "ci-linux-arm64-release",
        "ci-linux-arm64-release-strict",
        "ci-windows-release",
        "ci-windows-release-strict",
        "ci-linux-compat",
    ]
    out: list[Path] = []
    for name in names:
        for preset in preset_dirs:
            out.extend(
                [
                    build_root / preset / "core" / name,
                    build_root / preset / "core" / "Release" / name,
                    build_root / preset / "core" / "Debug" / name,
                ]
            )
    return out


def _provider_candidates(root: Path) -> list[Path]:
    names = ["anolis-provider-sim.exe", "anolis-provider-sim"]
    build_roots = [
        root.parent / "anolis-provider-sim" / "build",
        root / "anolis-provider-sim" / "build",
    ]
    preset_dirs = [
        "dev-release",
        "dev-debug",
        "dev-windows-release",
        "dev-windows-debug",
        "ci-linux-release",
        "ci-windows-release",
    ]
    out: list[Path] = []
    for build_root in build_roots:
        for name in names:
            for preset in preset_dirs:
                out.extend(
                    [
                        build_root / preset / name,
                        build_root / preset / "Release" / name,
                        build_root / preset / "Debug" / name,
                    ]
                )
    return out


def _resolve_runtime_binary(repo_root: Path, override: str | None) -> Path:
    if override:
        runtime = Path(override).expanduser().resolve()
        if runtime.is_file():
            return runtime
        raise SystemExit(f"ERROR: runtime binary not found: {runtime}")
    found = _first_existing(_runtime_candidates(repo_root))
    if found is None:
        raise SystemExit(
            "ERROR: runtime binary not found.\nBuild runtime first or pass --runtime-bin <path> explicitly."
        )
    return found


def _resolve_provider_binary(repo_root: Path, override: str | None) -> Path:
    if override:
        provider = Path(override).expanduser().resolve()
        if provider.is_file():
            return provider
        raise SystemExit(f"ERROR: provider binary not found: {provider}")
    found = _first_existing(_provider_candidates(repo_root))
    if found is None:
        raise SystemExit(
            "ERROR: provider binary not found.\n"
            "Build anolis-provider-sim first or pass --provider-bin <path> explicitly."
        )
    return found


def _resolve_operation(openapi_doc: dict[str, Any], method: str, path_template: str) -> dict[str, Any]:
    paths = openapi_doc.get("paths", {})
    if not isinstance(paths, dict):
        raise KeyError("OpenAPI 'paths' block missing")
    path_item = paths.get(path_template)
    if not isinstance(path_item, dict):
        raise KeyError(f"OpenAPI path not found: {path_template}")
    operation = path_item.get(method.lower())
    if not isinstance(operation, dict):
        raise KeyError(f"OpenAPI operation not found: {method.upper()} {path_template}")
    return operation


def _resolve_response_schema(
    openapi_doc: dict[str, Any],
    operation: dict[str, Any],
    status_code: int,
    content_type: str,
) -> Any:
    responses = operation.get("responses")
    if not isinstance(responses, dict):
        raise KeyError("operation has no responses block")
    status_key = str(status_code)
    response_obj = responses.get(status_key)
    if response_obj is None:
        raise KeyError(f"status code {status_key} not declared in OpenAPI operation")

    if isinstance(response_obj, dict) and "$ref" in response_obj:
        response_obj = _resolve_json_pointer(openapi_doc, response_obj["$ref"])

    if not isinstance(response_obj, dict):
        raise KeyError("resolved response object is invalid")

    content = response_obj.get("content")
    if not isinstance(content, dict):
        raise KeyError("response has no content block")

    media = content.get(content_type)
    if not isinstance(media, dict):
        raise KeyError(f"content type {content_type!r} not declared for status {status_key}")

    schema = media.get("schema")
    if schema is None:
        raise KeyError("response media type has no schema")

    return _expand_refs(schema, openapi_doc)


def _validate_json_instance(schema: Any, payload: Any) -> list[str]:
    validator_cls = jsonschema.validators.validator_for(schema)
    validator = validator_cls(schema)
    errors = sorted(validator.iter_errors(payload), key=lambda err: list(err.path))
    out: list[str] = []
    for err in errors:
        json_path = "$" if not err.path else "$." + ".".join(str(part) for part in err.path)
        out.append(f"{json_path}: {err.message}")
    return out


def _invoke_json(
    method: str,
    url: str,
    *,
    json_body: dict[str, Any] | None = None,
    timeout: float = 5.0,
) -> requests.Response:
    if method.upper() == "GET":
        return requests.get(url, timeout=timeout)
    if method.upper() == "POST":
        return requests.post(url, json=json_body, timeout=timeout)
    raise ValueError(f"unsupported method {method}")


def _capture_body_file(
    capture_dir: Path,
    *,
    name: str,
    status_code: int,
    payload: Any,
    as_json: bool,
) -> str:
    suffix = "json" if as_json else "txt"
    file_name = f"{name}.{status_code}.{suffix}"
    out_path = capture_dir / file_name
    if as_json:
        out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    else:
        out_path.write_text(str(payload), encoding="utf-8")
    return file_name


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate live runtime HTTP responses against OpenAPI.")
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
        "--runtime-bin",
        default=None,
        help="Path to anolis-runtime binary (optional; auto-detected if omitted).",
    )
    parser.add_argument(
        "--provider-bin",
        default=None,
        help="Path to anolis-provider-sim binary (optional; auto-detected if omitted).",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=0,
        help="HTTP port to use (0 = auto-pick free port).",
    )
    parser.add_argument(
        "--capture-dir",
        default=None,
        help="Optional directory to write captured live responses for example refresh.",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    openapi_path = (repo_root / args.openapi).resolve()
    if not openapi_path.is_file():
        print(f"ERROR: OpenAPI file not found: {openapi_path}", file=sys.stderr)
        return 1

    runtime_bin = _resolve_runtime_binary(repo_root, args.runtime_bin)
    provider_bin = _resolve_provider_binary(repo_root, args.provider_bin)
    openapi_doc = _load_yaml_mapping(openapi_path)

    capture_dir: Path | None = None
    captures: list[CaptureEntry] = []
    if args.capture_dir:
        candidate = Path(args.capture_dir).expanduser()
        if not candidate.is_absolute():
            candidate = (repo_root / candidate).resolve()
        capture_dir = candidate
        capture_dir.mkdir(parents=True, exist_ok=True)

    # Import after repo-root resolution to avoid path ambiguity when script is invoked from elsewhere.
    sys.path.insert(0, str(repo_root))
    from tests.support.runtime_fixture import RuntimeFixture  # pylint: disable=import-outside-toplevel

    port = args.port if args.port > 0 else _pick_free_port()
    fixture_provider_config_path = (repo_root / "tests/integration/fixtures/provider-sim-default.yaml").resolve()
    fixture_provider_config = str(fixture_provider_config_path).replace("\\", "/")
    fixture_config = {
        "runtime": {},
        "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_bin).replace("\\", "/"),
                "args": [
                    "--config",
                    fixture_provider_config,
                ],
                "timeout_ms": 5000,
            }
        ],
        "polling": {"interval_ms": 500},
        "telemetry": {"enabled": False},
        "automation": {"enabled": False},
        "logging": {"level": "info"},
    }
    fixture = RuntimeFixture(runtime_bin, provider_bin, http_port=port, config_dict=fixture_config)
    failures: list[Failure] = []
    checks_run = 0
    checks_passed = 0

    def run_json_check(
        *,
        name: str,
        method: str,
        path_template: str,
        actual_path: str,
        json_body: dict[str, Any] | None = None,
        expected_status: int | None = None,
    ) -> None:
        nonlocal checks_run, checks_passed
        checks_run += 1
        try:
            op = _resolve_operation(openapi_doc, method, path_template)
            resp = _invoke_json(method, f"{fixture.base_url}{actual_path}", json_body=json_body)
            if expected_status is not None and resp.status_code != expected_status:
                failures.append(
                    Failure(
                        "status",
                        f"{name}: expected HTTP {expected_status}, got {resp.status_code} "
                        f"({method.upper()} {actual_path})",
                    )
                )
                return

            schema = _resolve_response_schema(
                openapi_doc=openapi_doc,
                operation=op,
                status_code=resp.status_code,
                content_type="application/json",
            )
            try:
                payload = resp.json()
            except ValueError as exc:
                failures.append(Failure("response", f"{name}: response is not valid JSON: {exc}"))
                return

            if capture_dir is not None:
                body_file = _capture_body_file(
                    capture_dir,
                    name=name,
                    status_code=resp.status_code,
                    payload=payload,
                    as_json=True,
                )
                captures.append(
                    CaptureEntry(
                        name=name,
                        method=method.upper(),
                        path_template=path_template,
                        actual_path=actual_path,
                        status_code=resp.status_code,
                        content_type="application/json",
                        body_file=body_file,
                    )
                )

            validation_errors = _validate_json_instance(schema, payload)
            if validation_errors:
                failures.append(
                    Failure(
                        "schema",
                        f"{name}: schema mismatch ({method.upper()} {path_template}, status={resp.status_code}): "
                        f"{validation_errors[0]}",
                    )
                )
                return
            checks_passed += 1
        except Exception as exc:  # noqa: BLE001
            failures.append(Failure("check", f"{name}: {exc}"))

    def run_events_check(*, name: str) -> None:
        nonlocal checks_run, checks_passed
        checks_run += 1
        path_template = "/v0/events"
        try:
            op = _resolve_operation(openapi_doc, "get", path_template)
            resp = requests.get(
                f"{fixture.base_url}/v0/events?provider_id=sim0",
                stream=True,
                timeout=5.0,
                headers={"Accept": "text/event-stream"},
            )
            try:
                if resp.status_code == 200:
                    ctype = resp.headers.get("Content-Type", "")
                    if "text/event-stream" not in ctype:
                        failures.append(
                            Failure(
                                "response",
                                f"{name}: expected text/event-stream content type, got {ctype!r}",
                            )
                        )
                        return
                    schema = _resolve_response_schema(
                        openapi_doc=openapi_doc,
                        operation=op,
                        status_code=resp.status_code,
                        content_type="text/event-stream",
                    )
                    validation_errors = _validate_json_instance(schema, "")
                    if validation_errors:
                        failures.append(
                            Failure(
                                "schema",
                                f"{name}: event-stream schema invalid: {validation_errors[0]}",
                            )
                        )
                        return
                    if capture_dir is not None:
                        body_file = _capture_body_file(
                            capture_dir,
                            name=name,
                            status_code=resp.status_code,
                            payload="event: state_update\nid: 0\ndata: {}\n\n",
                            as_json=False,
                        )
                        captures.append(
                            CaptureEntry(
                                name=name,
                                method="GET",
                                path_template=path_template,
                                actual_path="/v0/events?provider_id=sim0",
                                status_code=resp.status_code,
                                content_type="text/event-stream",
                                body_file=body_file,
                            )
                        )
                else:
                    schema = _resolve_response_schema(
                        openapi_doc=openapi_doc,
                        operation=op,
                        status_code=resp.status_code,
                        content_type="application/json",
                    )
                    try:
                        payload = resp.json()
                    except ValueError as exc:
                        failures.append(Failure("response", f"{name}: non-JSON error response: {exc}"))
                        return
                    if capture_dir is not None:
                        body_file = _capture_body_file(
                            capture_dir,
                            name=name,
                            status_code=resp.status_code,
                            payload=payload,
                            as_json=True,
                        )
                        captures.append(
                            CaptureEntry(
                                name=name,
                                method="GET",
                                path_template=path_template,
                                actual_path="/v0/events?provider_id=sim0",
                                status_code=resp.status_code,
                                content_type="application/json",
                                body_file=body_file,
                            )
                        )
                    validation_errors = _validate_json_instance(schema, payload)
                    if validation_errors:
                        failures.append(Failure("schema", f"{name}: {validation_errors[0]}"))
                        return
                checks_passed += 1
            finally:
                resp.close()
        except Exception as exc:  # noqa: BLE001
            failures.append(Failure("check", f"{name}: {exc}"))

    try:
        started = fixture.start(wait_for_ready=True, provider_id="sim0", min_device_count=1, startup_timeout=20.0)
        if not started:
            capture = fixture.get_output_capture()
            tail = capture.get_recent_output(120) if capture else "(no runtime output)"
            print("ERROR: runtime fixture failed to start", file=sys.stderr)
            print(tail, file=sys.stderr)
            return 1

        devices_resp = requests.get(f"{fixture.base_url}/v0/devices", timeout=5.0)
        devices_resp.raise_for_status()
        devices = devices_resp.json().get("devices", [])
        if not devices:
            print("ERROR: conformance bootstrap failed, /v0/devices returned zero devices", file=sys.stderr)
            return 1
        first = devices[0]
        provider_id = first.get("provider_id")
        device_id = first.get("device_id")
        if not isinstance(provider_id, str) or not isinstance(device_id, str):
            print("ERROR: conformance bootstrap failed, invalid provider_id/device_id in /v0/devices", file=sys.stderr)
            return 1

        run_json_check(
            name="runtime_status",
            method="GET",
            path_template="/v0/runtime/status",
            actual_path="/v0/runtime/status",
        )
        run_json_check(
            name="providers_health",
            method="GET",
            path_template="/v0/providers/health",
            actual_path="/v0/providers/health",
        )
        run_json_check(name="devices", method="GET", path_template="/v0/devices", actual_path="/v0/devices")
        run_json_check(
            name="device_capabilities",
            method="GET",
            path_template="/v0/devices/{provider_id}/{device_id}/capabilities",
            actual_path=f"/v0/devices/{provider_id}/{device_id}/capabilities",
        )
        run_json_check(name="state_collection", method="GET", path_template="/v0/state", actual_path="/v0/state")
        run_json_check(
            name="device_state",
            method="GET",
            path_template="/v0/state/{provider_id}/{device_id}",
            actual_path=f"/v0/state/{provider_id}/{device_id}",
        )
        # Use an intentionally malformed call body to guarantee a deterministic schema-validated error path.
        run_json_check(
            name="call_invalid_payload",
            method="POST",
            path_template="/v0/call",
            actual_path="/v0/call",
            json_body={},
            expected_status=400,
        )
        run_json_check(
            name="device_capabilities_not_found",
            method="GET",
            path_template="/v0/devices/{provider_id}/{device_id}/capabilities",
            actual_path=f"/v0/devices/{provider_id}/definitely_missing_device/capabilities",
            expected_status=404,
        )
        run_json_check(
            name="mode_get",
            method="GET",
            path_template="/v0/mode",
            actual_path="/v0/mode",
            expected_status=503,
        )
        run_json_check(
            name="mode_post_invalid_payload",
            method="POST",
            path_template="/v0/mode",
            actual_path="/v0/mode",
            json_body={},
            expected_status=503,
        )
        run_json_check(
            name="parameters_get",
            method="GET",
            path_template="/v0/parameters",
            actual_path="/v0/parameters",
            expected_status=503,
        )
        run_json_check(
            name="parameters_post_invalid_payload",
            method="POST",
            path_template="/v0/parameters",
            actual_path="/v0/parameters",
            json_body={},
            expected_status=503,
        )
        run_json_check(
            name="automation_tree",
            method="GET",
            path_template="/v0/automation/tree",
            actual_path="/v0/automation/tree",
            expected_status=503,
        )
        run_json_check(
            name="automation_status",
            method="GET",
            path_template="/v0/automation/status",
            actual_path="/v0/automation/status",
            expected_status=503,
        )
        run_events_check(name="events")
    finally:
        fixture.cleanup()

    print("runtime-http conformance summary")
    print(f"  runtime: {runtime_bin}")
    print(f"  provider: {provider_bin}")
    print(f"  base_url: http://127.0.0.1:{port}")
    print(f"  checks run: {checks_run}")
    print(f"  checks passed: {checks_passed}")
    print(f"  required operations covered: {len(REQUIRED_OPERATIONS)}")
    if capture_dir is not None:
        print(f"  capture_dir: {capture_dir}")

    if checks_run < len(REQUIRED_OPERATIONS):
        failures.append(
            Failure(
                "coverage",
                f"internal error: executed {checks_run} checks, expected {len(REQUIRED_OPERATIONS)}",
            )
        )

    if failures:
        print("\nFAILURES:")
        for failure in failures:
            print(f"  - [{failure.stage}] {failure.message}")
        return 1

    if capture_dir is not None:
        manifest_path = capture_dir / "manifest.json"
        manifest_payload = {
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "base_url": fixture.base_url,
            "checks": [
                {
                    "name": cap.name,
                    "method": cap.method,
                    "path_template": cap.path_template,
                    "actual_path": cap.actual_path,
                    "status_code": cap.status_code,
                    "content_type": cap.content_type,
                    "body_file": cap.body_file,
                }
                for cap in captures
            ],
        }
        manifest_path.write_text(json.dumps(manifest_payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"  capture_manifest: {manifest_path}")

    print("\nRuntime HTTP conformance checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
