#!/usr/bin/env python3
"""Telemetry export MVP service.

External data-plane service for querying InfluxDB telemetry (`anolis_signal`) with
explicit guardrails and auth. This service intentionally does not modify
`anolis-runtime` HTTP APIs.
"""

from __future__ import annotations

import argparse
import csv
import hmac
import json
import logging
import tempfile
import threading
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from tools.telemetry_export.export_core.config import load_config
from tools.telemetry_export.export_core.flux_builder import build_flux_query
from tools.telemetry_export.export_core.influx_client import influx_query_csv, influx_query_csv_stream
from tools.telemetry_export.export_core.models import (
    ApiError,
    AppConfig,
    AuthorizationConfig,
    InfluxConfig,
    LimitConfig,
    Resolution,
    ServerConfig,
    SignalsQuery,
    SpoolResult,
)
from tools.telemetry_export.export_core.serialization import (
    build_manifest,
    coerce_request_id,
    coerce_requester_id,
    compute_manifest_hash,
    iter_influx_csv_rows,
    json_error_payload,
    normalize_row,
    normalize_rows,
    parse_influx_csv_rows,
    render_csv,
)
from tools.telemetry_export.export_core.validation import validate_query_request

LOGGER = logging.getLogger("telemetry_export")

# Re-export selected helpers for compatibility with existing tests/tools.
__all__ = [
    "ApiError",
    "AppConfig",
    "AuthorizationConfig",
    "ExportService",
    "InfluxConfig",
    "LimitConfig",
    "Resolution",
    "ServerConfig",
    "SignalsQuery",
    "build_flux_query",
    "influx_query_csv",
    "influx_query_csv_stream",
    "load_config",
    "normalize_row",
    "normalize_rows",
    "parse_influx_csv_rows",
    "render_csv",
    "validate_query_request",
]

CONTENT_TYPE_BY_FORMAT = {
    "csv": "text/csv; charset=utf-8",
    "json": "application/json",
    "ndjson": "application/x-ndjson; charset=utf-8",
}

SUFFIX_BY_FORMAT = {
    "csv": ".csv",
    "json": ".json",
    "ndjson": ".ndjson",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Anolis telemetry export MVP service")
    parser.add_argument(
        "--config",
        default="config/bioreactor/telemetry-export.bioreactor.yaml",
        help="Path to export service YAML config",
    )
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    return parser.parse_args()


class _BoundedTextWriter:
    """UTF-8 text writer with deterministic max-byte enforcement."""

    def __init__(self, handle: Any, max_bytes: int):
        self._handle = handle
        self._max_bytes = max_bytes
        self._bytes_written = 0

    def write(self, text: str) -> int:
        encoded = text.encode("utf-8")
        self._bytes_written += len(encoded)
        if self._bytes_written > self._max_bytes:
            raise ApiError(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "limit_exceeded",
                f"response exceeds max_response_bytes={self._max_bytes}",
            )
        written = self._handle.write(text)
        return int(written)

    def flush(self) -> None:
        self._handle.flush()

    @property
    def bytes_written(self) -> int:
        return self._bytes_written


class ExportService:
    """In-memory request handler facade for the HTTP layer."""

    def __init__(self, config: AppConfig):
        self.config = config
        self._manifest_lock = threading.Lock()
        self._manifest_by_export_id: dict[str, dict[str, Any]] = {}

    def authorize(self, authorization_header: str | None) -> None:
        if not authorization_header or not authorization_header.startswith("Bearer "):
            raise ApiError(HTTPStatus.UNAUTHORIZED, "unauthenticated", "Authorization: Bearer <token> is required")

        supplied = authorization_header[len("Bearer ") :].strip()
        if not supplied or not hmac.compare_digest(supplied, self.config.server.auth_token):
            raise ApiError(HTTPStatus.UNAUTHORIZED, "unauthenticated", "invalid bearer token")

    def enforce_scope(self, query: SignalsQuery) -> None:
        auth = self.config.authorization
        if not auth.enforce_selector_scope:
            return

        self.enforce_scope_dimension(query.runtime_names, auth.allowed_runtime_names, "selector.runtime_names")
        self.enforce_scope_dimension(query.provider_ids, auth.allowed_provider_ids, "selector.provider_ids")
        self.enforce_scope_dimension(query.device_ids, auth.allowed_device_ids, "selector.device_ids")
        self.enforce_scope_dimension(query.signal_ids, auth.allowed_signal_ids, "selector.signal_ids")

    @staticmethod
    def enforce_scope_dimension(requested: list[str], allowed: tuple[str, ...], field_name: str) -> None:
        if not allowed:
            return

        if not requested:
            raise ApiError(
                HTTPStatus.FORBIDDEN,
                "permission_denied",
                f"{field_name} must be explicitly set when selector scope enforcement is enabled",
            )

        allowed_set = set(allowed)
        denied = [value for value in requested if value not in allowed_set]
        if denied:
            raise ApiError(
                HTTPStatus.FORBIDDEN,
                "permission_denied",
                f"{field_name} contains unauthorized values: {', '.join(denied[:5])}",
            )

    def get_manifest(self, export_id: str) -> dict[str, Any] | None:
        with self._manifest_lock:
            record = self._manifest_by_export_id.get(export_id)
            if record is None:
                return None
            return dict(record["manifest"])

    def _store_manifest(self, export_id: str, manifest: dict[str, Any], manifest_hash: str) -> None:
        with self._manifest_lock:
            self._manifest_by_export_id[export_id] = {
                "manifest_hash": manifest_hash,
                "manifest": manifest,
            }

    def execute_query(
        self,
        request_body: Any,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> tuple[int, dict[str, Any]]:
        query = validate_query_request(request_body, self.config.limits)
        return self.execute_query_from_query(query, request_id=request_id, requester_id=requester_id)

    def execute_query_from_query(
        self,
        query: SignalsQuery,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> tuple[int, dict[str, Any]]:
        # Backward-compatible in-memory helper used by unit tests and direct callers.
        self.enforce_scope(query)
        flux_query = build_flux_query(query, self.config.influx.bucket)
        csv_text = influx_query_csv(self.config, flux_query)
        raw_rows = parse_influx_csv_rows(csv_text)

        if len(raw_rows) > self.config.limits.max_rows:
            raise ApiError(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "limit_exceeded",
                f"row count exceeds max_rows={self.config.limits.max_rows}",
            )

        normalized_rows = normalize_rows(raw_rows, query.columns)
        export_id = str(uuid.uuid4())
        manifest = build_manifest(
            query,
            self.config,
            row_count=len(normalized_rows),
            export_id=export_id,
            request_id=request_id,
            requester_id=requester_id,
        )
        manifest_hash = compute_manifest_hash(manifest)
        self._store_manifest(export_id, manifest, manifest_hash)

        if query.fmt == "json":
            payload = {
                "status": "ok",
                "dataset": "signals",
                "format": "json",
                "manifest": manifest,
                "data": normalized_rows,
            }
            encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            if len(encoded) > self.config.limits.max_response_bytes:
                raise ApiError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                    "limit_exceeded",
                    f"response exceeds max_response_bytes={self.config.limits.max_response_bytes}",
                )
            return HTTPStatus.OK, payload

        if query.fmt == "ndjson":
            ndjson_body = "".join(json.dumps(row, separators=(",", ":")) + "\n" for row in normalized_rows)
            if len(ndjson_body.encode("utf-8")) > self.config.limits.max_response_bytes:
                raise ApiError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                    "limit_exceeded",
                    f"response exceeds max_response_bytes={self.config.limits.max_response_bytes}",
                )
            return HTTPStatus.OK, {
                "status": "ok",
                "dataset": "signals",
                "format": "ndjson",
                "manifest": manifest,
                "ndjson_body": ndjson_body,
            }

        csv_body = render_csv(normalized_rows, query.columns)
        if len(csv_body.encode("utf-8")) > self.config.limits.max_response_bytes:
            raise ApiError(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "limit_exceeded",
                f"response exceeds max_response_bytes={self.config.limits.max_response_bytes}",
            )

        return HTTPStatus.OK, {
            "status": "ok",
            "dataset": "signals",
            "format": "csv",
            "manifest": manifest,
            "csv_body": csv_body,
        }

    def execute_spooled_query(
        self,
        request_body: Any,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> SpoolResult:
        query = validate_query_request(request_body, self.config.limits)
        return self.execute_spooled_query_from_query(
            query,
            request_id=request_id,
            requester_id=requester_id,
        )

    def execute_spooled_query_from_query(
        self,
        query: SignalsQuery,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> SpoolResult:
        self.enforce_scope(query)

        flux_query = build_flux_query(query, self.config.influx.bucket)
        response = influx_query_csv_stream(self.config, flux_query)

        suffix = SUFFIX_BY_FORMAT.get(query.fmt)
        content_type = CONTENT_TYPE_BY_FORMAT.get(query.fmt)
        if suffix is None or content_type is None:
            response.close()
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", f"unsupported format: {query.fmt}")

        tmp_file = tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            delete=False,
            prefix="anolis_export_",
            suffix=suffix,
        )
        tmp_path = Path(tmp_file.name)
        bounded_writer = (
            _BoundedTextWriter(tmp_file, self.config.limits.max_response_bytes) if query.fmt == "json" else None
        )
        export_id = str(uuid.uuid4())
        row_count = 0
        content_length = 0

        try:
            LOGGER.debug(
                "request_id=%s starting spooled export format=%s columns=%s",
                request_id,
                query.fmt,
                ",".join(query.columns),
            )
            if query.fmt == "csv":
                csv_writer = csv.DictWriter(tmp_file, fieldnames=query.columns)
                csv_writer.writeheader()
            elif query.fmt == "json":
                assert bounded_writer is not None
                bounded_writer.write('{"status":"ok","dataset":"signals","format":"json","data":[')
                json_first = True
            else:
                json_first = False

            try:
                for raw_row in iter_influx_csv_rows(response):
                    row_count += 1
                    if row_count > self.config.limits.max_rows:
                        raise ApiError(
                            HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                            "limit_exceeded",
                            f"row count exceeds max_rows={self.config.limits.max_rows}",
                        )

                    normalized = normalize_row(raw_row, query.columns)

                    if query.fmt == "csv":
                        csv_writer.writerow(normalized)
                    elif query.fmt == "json":
                        assert bounded_writer is not None
                        if not json_first:
                            bounded_writer.write(",")
                        bounded_writer.write(json.dumps(normalized, separators=(",", ":")))
                        json_first = False
                    else:
                        tmp_file.write(json.dumps(normalized, separators=(",", ":")))
                        tmp_file.write("\n")
            except UnicodeDecodeError as exc:
                content_encoding = ""
                headers = getattr(response, "headers", None)
                if headers is not None:
                    try:
                        content_encoding = str(headers.get("Content-Encoding", "")).strip()
                    except Exception:
                        content_encoding = ""
                LOGGER.exception(
                    "request_id=%s stream decode failure format=%s content_encoding=%s",
                    request_id,
                    query.fmt,
                    content_encoding or "<none>",
                )
                raise ApiError(
                    HTTPStatus.BAD_GATEWAY,
                    "upstream_error",
                    f"failed to decode Influx CSV stream as UTF-8 (content_encoding={content_encoding or 'unknown'})",
                ) from exc

            manifest = build_manifest(
                query,
                self.config,
                row_count=row_count,
                export_id=export_id,
                request_id=request_id,
                requester_id=requester_id,
            )
            manifest_hash = compute_manifest_hash(manifest)
            self._store_manifest(export_id, manifest, manifest_hash)

            if query.fmt == "json":
                assert bounded_writer is not None
                bounded_writer.write('],"manifest":')
                bounded_writer.write(json.dumps(manifest, separators=(",", ":")))
                bounded_writer.write("}")
                bounded_writer.flush()
                content_length = bounded_writer.bytes_written
            else:
                tmp_file.flush()

            tmp_file.close()
            response.close()
            if query.fmt != "json":
                content_length = tmp_path.stat().st_size

            LOGGER.debug(
                "request_id=%s completed spooled export format=%s rows=%d bytes=%d export_id=%s",
                request_id,
                query.fmt,
                row_count,
                content_length,
                export_id,
            )

            return SpoolResult(
                path=tmp_path,
                fmt=query.fmt,
                content_type=content_type,
                row_count=row_count,
                content_length=content_length,
                export_id=export_id,
                manifest_hash=manifest_hash,
                manifest=manifest,
            )
        except Exception:
            try:
                tmp_file.close()
            except Exception:
                pass
            try:
                response.close()
            except Exception:
                pass
            try:
                if tmp_path.exists():
                    tmp_path.unlink(missing_ok=True)
            except Exception:
                pass
            raise

    # Backward-compatible aliases kept for existing tests/callers.
    def execute_csv_spooled_query(
        self,
        request_body: Any,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> SpoolResult:
        result = self.execute_spooled_query(
            request_body,
            request_id=request_id,
            requester_id=requester_id,
        )
        if result.fmt != "csv":
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "format must be csv")
        return result

    def execute_csv_spooled_query_from_query(
        self,
        query: SignalsQuery,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> SpoolResult:
        result = self.execute_spooled_query_from_query(
            query,
            request_id=request_id,
            requester_id=requester_id,
        )
        if result.fmt != "csv":
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "format must be csv")
        return result


class ExportRequestHandler(BaseHTTPRequestHandler):
    """HTTP request adapter for ExportService."""

    service: ExportService

    def log_message(self, format_str: str, *args: Any) -> None:
        LOGGER.info("%s - %s", self.address_string(), format_str % args)

    def do_GET(self) -> None:
        request_id = coerce_request_id(self.headers.get("X-Request-Id"))

        if self.path == "/v1/health":
            self.send_json(HTTPStatus.OK, {"status": "ok"}, request_id=request_id)
            return

        prefix = "/v1/exports/manifests/"
        if self.path.startswith(prefix):
            self.service.authorize(self.headers.get("Authorization"))
            export_id = self.path[len(prefix) :].strip()
            if not export_id:
                self.send_json(
                    HTTPStatus.BAD_REQUEST,
                    json_error_payload("invalid_argument", "export_id is required"),
                    request_id=request_id,
                )
                return
            manifest = self.service.get_manifest(export_id)
            if manifest is None:
                self.send_json(
                    HTTPStatus.NOT_FOUND,
                    json_error_payload("not_found", f"manifest not found for export_id={export_id}"),
                    request_id=request_id,
                )
                return
            self.send_json(
                HTTPStatus.OK,
                {"status": "ok", "export_id": export_id, "manifest": manifest},
                request_id=request_id,
            )
            return

        self.send_json(
            HTTPStatus.NOT_FOUND,
            json_error_payload("not_found", "route not found"),
            request_id=request_id,
        )

    def do_POST(self) -> None:
        request_id = coerce_request_id(self.headers.get("X-Request-Id"))
        requester_id = coerce_requester_id(self.headers.get("X-Requester-Id"))

        LOGGER.info("request_id=%s method=POST path=%s requester=%s", request_id, self.path, requester_id)

        if self.path != "/v1/exports/signals:query":
            self.send_json(
                HTTPStatus.NOT_FOUND,
                json_error_payload("not_found", "route not found"),
                request_id=request_id,
            )
            return

        try:
            self.service.authorize(self.headers.get("Authorization"))
            body = self.read_json_body(self.service.config.limits.max_request_bytes)
            query = validate_query_request(body, self.service.config.limits)
            result = self.service.execute_spooled_query_from_query(
                query,
                request_id=request_id,
                requester_id=requester_id,
            )
            try:
                self.send_file_response(
                    HTTPStatus.OK,
                    export_path=result.path,
                    content_length=result.content_length,
                    content_type=result.content_type,
                    export_id=result.export_id,
                    manifest_hash=result.manifest_hash,
                    request_id=request_id,
                    requester_id=requester_id,
                )
            finally:
                result.path.unlink(missing_ok=True)
        except ApiError as exc:
            LOGGER.warning(
                "request_id=%s api_error status=%s code=%s message=%s",
                request_id,
                int(exc.status),
                exc.code,
                exc.message,
            )
            self.send_json(exc.status, json_error_payload(exc.code, exc.message), request_id=request_id)
        except Exception as exc:  # pragma: no cover - defensive fallback
            LOGGER.exception("request_id=%s unhandled export service error", request_id)
            self.send_json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                json_error_payload("internal", f"unexpected error: {exc}"),
                request_id=request_id,
            )

    def read_json_body(self, max_request_bytes: int) -> Any:
        content_length_raw = self.headers.get("Content-Length", "")
        if not content_length_raw.isdigit():
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "Content-Length header is required")

        content_length = int(content_length_raw)
        if content_length <= 0:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "request body is required")
        if content_length > max_request_bytes:
            raise ApiError(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "limit_exceeded",
                f"request body exceeds max_request_bytes={max_request_bytes}",
            )

        raw = self.rfile.read(content_length)
        try:
            return json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", f"invalid JSON body: {exc}") from exc

    def send_json(self, status: int | HTTPStatus, payload: dict[str, Any], *, request_id: str) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(int(status))
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Request-Id", request_id)
        self.end_headers()
        self.wfile.write(body)

    def send_file_response(
        self,
        status: int | HTTPStatus,
        *,
        export_path: Path,
        content_length: int,
        content_type: str,
        export_id: str,
        manifest_hash: str,
        request_id: str,
        requester_id: str,
    ) -> None:
        self.send_response(int(status))
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(content_length))
        self.send_header("X-Request-Id", request_id)
        self.send_header("X-Requester-Id", requester_id)
        self.send_header("X-Export-Id", export_id)
        self.send_header("X-Export-Manifest-Hash", manifest_hash)
        self.end_headers()
        with export_path.open("rb") as handle:
            while True:
                chunk = handle.read(64 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)


def run_server(config: AppConfig) -> None:
    handler_cls = ExportRequestHandler
    handler_cls.service = ExportService(config)

    server = ThreadingHTTPServer((config.server.host, config.server.port), handler_cls)
    LOGGER.info("Telemetry export service listening on %s:%d", config.server.host, config.server.port)

    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        LOGGER.info("Shutdown requested")
    finally:
        server.server_close()
        LOGGER.info("Telemetry export service stopped")


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="[%(asctime)s] [%(levelname)s] %(message)s",
    )

    config = load_config(Path(args.config))
    run_server(config)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
