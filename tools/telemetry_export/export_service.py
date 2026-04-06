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
    CsvSpoolResult,
    InfluxConfig,
    LimitConfig,
    Resolution,
    ServerConfig,
    SignalsQuery,
)
from tools.telemetry_export.export_core.serialization import (
    build_manifest,
    coerce_request_id,
    coerce_requester_id,
    iter_influx_csv_rows,
    json_error_payload,
    normalize_row,
    normalize_rows,
    parse_influx_csv_rows,
    render_csv,
)
from tools.telemetry_export.export_core.validation import validate_query_request

LOGGER = logging.getLogger("telemetry_export")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Anolis telemetry export MVP service")
    parser.add_argument(
        "--config",
        default="config/bioreactor/telemetry-export.bioreactor.yaml",
        help="Path to export service YAML config",
    )
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    return parser.parse_args()


class ExportService:
    """In-memory request handler facade for the HTTP layer."""

    def __init__(self, config: AppConfig):
        self.config = config

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
        manifest = build_manifest(
            query,
            self.config,
            row_count=len(normalized_rows),
            request_id=request_id,
            requester_id=requester_id,
        )

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

    def execute_csv_spooled_query(
        self,
        request_body: Any,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> CsvSpoolResult:
        query = validate_query_request(request_body, self.config.limits)
        return self.execute_csv_spooled_query_from_query(
            query,
            request_id=request_id,
            requester_id=requester_id,
        )

    def execute_csv_spooled_query_from_query(
        self,
        query: SignalsQuery,
        *,
        request_id: str = "unknown",
        requester_id: str = "anonymous",
    ) -> CsvSpoolResult:
        self.enforce_scope(query)
        if query.fmt != "csv":
            raise ApiError(
                HTTPStatus.BAD_REQUEST,
                "invalid_argument",
                "execute_csv_spooled_query requires format=csv",
            )

        flux_query = build_flux_query(query, self.config.influx.bucket)
        response = influx_query_csv_stream(self.config, flux_query)

        tmp_file = tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            delete=False,
            prefix="anolis_export_",
            suffix=".csv",
        )
        tmp_path = Path(tmp_file.name)
        row_count = 0

        try:
            writer = csv.DictWriter(tmp_file, fieldnames=query.columns)
            writer.writeheader()

            for raw_row in iter_influx_csv_rows(response):
                row_count += 1
                if row_count > self.config.limits.max_rows:
                    raise ApiError(
                        HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                        "limit_exceeded",
                        f"row count exceeds max_rows={self.config.limits.max_rows}",
                    )
                writer.writerow(normalize_row(raw_row, query.columns))

            tmp_file.flush()
            tmp_file.close()
            response.close()

            content_length = tmp_path.stat().st_size
            if content_length > self.config.limits.max_response_bytes:
                raise ApiError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                    "limit_exceeded",
                    f"response exceeds max_response_bytes={self.config.limits.max_response_bytes}",
                )

            manifest = build_manifest(
                query,
                self.config,
                row_count=row_count,
                request_id=request_id,
                requester_id=requester_id,
            )

            return CsvSpoolResult(
                path=tmp_path,
                row_count=row_count,
                content_length=content_length,
                manifest=manifest,
            )
        except Exception:
            try:
                tmp_file.close()
            except Exception:
                pass
            response.close()
            if tmp_path.exists():
                tmp_path.unlink(missing_ok=True)
            raise


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
            if query.fmt == "csv":
                result = self.service.execute_csv_spooled_query_from_query(
                    query,
                    request_id=request_id,
                    requester_id=requester_id,
                )
                try:
                    self.send_csv_file(
                        HTTPStatus.OK,
                        csv_path=result.path,
                        content_length=result.content_length,
                        manifest=result.manifest,
                        request_id=request_id,
                        requester_id=requester_id,
                    )
                finally:
                    result.path.unlink(missing_ok=True)
                return

            status, payload = self.service.execute_query_from_query(
                query,
                request_id=request_id,
                requester_id=requester_id,
            )

            self.send_json(status, payload, request_id=request_id)
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

    def send_csv_file(
        self,
        status: int | HTTPStatus,
        *,
        csv_path: Path,
        content_length: int,
        manifest: Any,
        request_id: str,
        requester_id: str,
    ) -> None:
        self.send_response(int(status))
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Length", str(content_length))
        self.send_header("X-Request-Id", request_id)
        self.send_header("X-Requester-Id", requester_id)

        if isinstance(manifest, dict):
            self.send_header("X-Export-Manifest", json.dumps(manifest, separators=(",", ":")))

        self.end_headers()
        with csv_path.open("rb") as handle:
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
