#!/usr/bin/env python3
"""Telemetry export MVP service.

External data-plane service for querying InfluxDB telemetry (`anolis_signal`) with
explicit guardrails and auth. This service intentionally does not modify
`anolis-runtime` HTTP APIs.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import hmac
import io
import json
import logging
import os
import re
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import requests
import yaml

LOGGER = logging.getLogger("telemetry_export")

DEFAULT_COLUMNS = [
    "timestamp",
    "provider_id",
    "device_id",
    "signal_id",
    "value",
    "value_type",
    "quality",
]

ALLOWED_COLUMNS = set(DEFAULT_COLUMNS).union(
    {
        "value_double",
        "value_int",
        "value_uint",
        "value_bool",
        "value_string",
    }
)

ALLOWED_FORMATS = {"json", "csv"}
ALLOWED_RESOLUTION_MODES = {"raw_event", "downsampled"}
ALLOWED_AGGREGATIONS = {"last", "mean", "min", "max", "count"}
INTERVAL_RE = re.compile(r"^[1-9][0-9]*(ms|s|m|h|d|w)$")
NUMERIC_VALUE_FIELDS = ("value_double", "value_int", "value_uint")
NON_NUMERIC_VALUE_FIELDS = ("value_bool", "value_string")
AUX_FIELDS_LAST_ONLY = ("quality",)
NON_NUMERIC_TYPED_COLUMNS = {"value_bool", "value_string"}


class ApiError(Exception):
    """Structured API error with HTTP mapping."""

    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


@dataclass(frozen=True)
class InfluxConfig:
    url: str
    org: str
    bucket: str
    token: str


@dataclass(frozen=True)
class LimitConfig:
    max_span_seconds: int
    max_rows: int
    max_response_bytes: int
    max_selector_items: int
    request_timeout_seconds: int
    max_request_bytes: int


@dataclass(frozen=True)
class ServerConfig:
    host: str
    port: int
    auth_token: str


@dataclass(frozen=True)
class AuthorizationConfig:
    enforce_selector_scope: bool
    allowed_provider_ids: tuple[str, ...]
    allowed_device_ids: tuple[str, ...]
    allowed_signal_ids: tuple[str, ...]


@dataclass(frozen=True)
class AppConfig:
    server: ServerConfig
    influx: InfluxConfig
    limits: LimitConfig
    authorization: AuthorizationConfig


@dataclass(frozen=True)
class Resolution:
    mode: str
    interval: str | None = None
    aggregation: str | None = None


@dataclass(frozen=True)
class SignalsQuery:
    start: datetime
    end: datetime
    resolution: Resolution
    fmt: str
    columns: list[str]
    provider_ids: list[str]
    device_ids: list[str]
    signal_ids: list[str]
    original_request: dict[str, Any]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Anolis telemetry export MVP service")
    parser.add_argument(
        "--config",
        default="config/bioreactor/telemetry-export.bioreactor.yaml",
        help="Path to export service YAML config",
    )
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    return parser.parse_args()


def parse_bool(value: Any, field_name: str) -> bool:
    if isinstance(value, bool):
        return value
    raise RuntimeError(f"Invalid config: {field_name} must be boolean")


def parse_allowed_list(value: Any, field_name: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list):
        raise RuntimeError(f"Invalid config: {field_name} must be an array of strings")

    parsed: list[str] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise RuntimeError(f"Invalid config: {field_name} entries must be non-empty strings")
        normalized = item.strip()
        if normalized not in seen:
            parsed.append(normalized)
            seen.add(normalized)

    return tuple(parsed)


def resolve_secret(*, env_name: str, config_value: str, field_name: str) -> str:
    """Resolve secret using env override then config fallback."""
    env_value = os.getenv(env_name, "").strip()
    if env_value:
        return env_value

    if config_value:
        return config_value

    raise RuntimeError(
        f"Invalid config: {field_name} must be set (or provide env override {env_name})"
    )


def load_config(path: Path) -> AppConfig:
    if not path.exists() or not path.is_file():
        raise RuntimeError(f"Config file not found: {path}")

    raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise RuntimeError("Invalid config: expected top-level mapping")

    server_raw = raw.get("server")
    influx_raw = raw.get("influxdb")
    limits_raw = raw.get("limits")
    authorization_raw = raw.get("authorization", {})

    if not isinstance(server_raw, dict):
        raise RuntimeError("Invalid config: 'server' section is required")
    if not isinstance(influx_raw, dict):
        raise RuntimeError("Invalid config: 'influxdb' section is required")
    if not isinstance(limits_raw, dict):
        raise RuntimeError("Invalid config: 'limits' section is required")
    if authorization_raw is None:
        authorization_raw = {}
    if not isinstance(authorization_raw, dict):
        raise RuntimeError("Invalid config: 'authorization' section must be an object when set")

    server = ServerConfig(
        host=str(server_raw.get("host", "127.0.0.1")),
        port=int(server_raw.get("port", 8091)),
        auth_token=resolve_secret(
            env_name="ANOLIS_EXPORT_AUTH_TOKEN",
            config_value=str(server_raw.get("auth_token", "")).strip(),
            field_name="server.auth_token",
        ),
    )

    influx = InfluxConfig(
        url=str(influx_raw.get("url", "")).rstrip("/"),
        org=str(influx_raw.get("org", "")).strip(),
        bucket=str(influx_raw.get("bucket", "")).strip(),
        token=resolve_secret(
            env_name="ANOLIS_EXPORT_INFLUX_TOKEN",
            config_value=str(influx_raw.get("token", "")).strip(),
            field_name="influxdb.token",
        ),
    )

    limits = LimitConfig(
        max_span_seconds=int(limits_raw.get("max_span_seconds", 86400)),
        max_rows=int(limits_raw.get("max_rows", 50000)),
        max_response_bytes=int(limits_raw.get("max_response_bytes", 10_000_000)),
        max_selector_items=int(limits_raw.get("max_selector_items", 128)),
        request_timeout_seconds=int(limits_raw.get("request_timeout_seconds", 15)),
        max_request_bytes=int(limits_raw.get("max_request_bytes", 200_000)),
    )

    authorization = AuthorizationConfig(
        enforce_selector_scope=parse_bool(
            authorization_raw.get("enforce_selector_scope", False),
            "authorization.enforce_selector_scope",
        ),
        allowed_provider_ids=parse_allowed_list(
            authorization_raw.get("allowed_provider_ids"),
            "authorization.allowed_provider_ids",
        ),
        allowed_device_ids=parse_allowed_list(
            authorization_raw.get("allowed_device_ids"),
            "authorization.allowed_device_ids",
        ),
        allowed_signal_ids=parse_allowed_list(
            authorization_raw.get("allowed_signal_ids"),
            "authorization.allowed_signal_ids",
        ),
    )

    if not influx.url or not influx.org or not influx.bucket:
        raise RuntimeError("Invalid config: influxdb url/org/bucket are required")

    return AppConfig(server=server, influx=influx, limits=limits, authorization=authorization)


def parse_rfc3339_utc(value: str, field_name: str) -> datetime:
    normalized = value.strip()
    if normalized.endswith("Z"):
        normalized = normalized[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError as exc:
        raise ApiError(
            HTTPStatus.BAD_REQUEST,
            "invalid_argument",
            f"{field_name} must be RFC3339 (example: 2026-04-01T00:00:00Z)",
        ) from exc

    if parsed.tzinfo is None:
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", f"{field_name} must include timezone")

    return parsed.astimezone(timezone.utc)


def ensure_string_list(value: Any, field_name: str, max_items: int) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", f"{field_name} must be an array of strings")
    if len(value) > max_items:
        raise ApiError(
            HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
            "limit_exceeded",
            f"{field_name} exceeds max_selector_items={max_items}",
        )

    result: list[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise ApiError(
                HTTPStatus.BAD_REQUEST,
                "invalid_argument",
                f"{field_name} entries must be non-empty strings",
            )
        result.append(item.strip())
    return result


def parse_resolution(value: Any) -> Resolution:
    if not isinstance(value, dict):
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "resolution object is required")

    mode = value.get("mode")
    if not isinstance(mode, str) or mode not in ALLOWED_RESOLUTION_MODES:
        raise ApiError(
            HTTPStatus.BAD_REQUEST,
            "invalid_argument",
            f"resolution.mode must be one of: {sorted(ALLOWED_RESOLUTION_MODES)}",
        )

    if mode == "raw_event":
        return Resolution(mode=mode)

    interval = value.get("interval")
    aggregation = value.get("aggregation")

    if not isinstance(interval, str) or not INTERVAL_RE.match(interval):
        raise ApiError(
            HTTPStatus.BAD_REQUEST,
            "invalid_argument",
            "downsampled resolution.interval must match ^[1-9][0-9]*(ms|s|m|h|d|w)$",
        )

    if not isinstance(aggregation, str) or aggregation not in ALLOWED_AGGREGATIONS:
        raise ApiError(
            HTTPStatus.BAD_REQUEST,
            "invalid_argument",
            f"downsampled resolution.aggregation must be one of: {sorted(ALLOWED_AGGREGATIONS)}",
        )

    return Resolution(mode=mode, interval=interval, aggregation=aggregation)


def parse_columns(value: Any) -> list[str]:
    if value is None:
        return list(DEFAULT_COLUMNS)
    if not isinstance(value, list) or len(value) == 0:
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "columns must be a non-empty array of strings")

    cols: list[str] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, str):
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "columns entries must be strings")
        column = item.strip()
        if column not in ALLOWED_COLUMNS:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", f"unsupported column: {column}")
        if column not in seen:
            cols.append(column)
            seen.add(column)

    return cols


def validate_downsample_column_combination(resolution: Resolution, columns: list[str]) -> None:
    """Reject invalid downsample aggregation/column combinations."""
    if resolution.mode != "downsampled" or resolution.aggregation == "last":
        return

    invalid = sorted(set(columns).intersection(NON_NUMERIC_TYPED_COLUMNS))
    if invalid:
        raise ApiError(
            HTTPStatus.BAD_REQUEST,
            "invalid_argument",
            (
                "downsampled aggregation "
                f"'{resolution.aggregation}' is not valid with non-numeric typed columns: {invalid}; "
                "use aggregation='last' or remove those columns"
            ),
        )


def validate_query_request(body: Any, limits: LimitConfig) -> SignalsQuery:
    if not isinstance(body, dict):
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "request body must be a JSON object")

    time_range = body.get("time_range")
    if not isinstance(time_range, dict):
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "time_range object is required")

    start_raw = time_range.get("start")
    end_raw = time_range.get("end")
    if not isinstance(start_raw, str) or not isinstance(end_raw, str):
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "time_range.start and time_range.end are required")

    start = parse_rfc3339_utc(start_raw, "time_range.start")
    end = parse_rfc3339_utc(end_raw, "time_range.end")

    if not start < end:
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "time_range.start must be earlier than end")

    span_seconds = int((end - start).total_seconds())
    if span_seconds > limits.max_span_seconds:
        raise ApiError(
            HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
            "limit_exceeded",
            f"time range exceeds max_span_seconds={limits.max_span_seconds}",
        )

    selector = body.get("selector", {})
    if selector is None:
        selector = {}
    if not isinstance(selector, dict):
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", "selector must be an object")

    provider_ids = ensure_string_list(selector.get("provider_ids"), "selector.provider_ids", limits.max_selector_items)
    device_ids = ensure_string_list(selector.get("device_ids"), "selector.device_ids", limits.max_selector_items)
    signal_ids = ensure_string_list(selector.get("signal_ids"), "selector.signal_ids", limits.max_selector_items)

    resolution = parse_resolution(body.get("resolution"))

    fmt = body.get("format", "json")
    if not isinstance(fmt, str) or fmt not in ALLOWED_FORMATS:
        raise ApiError(HTTPStatus.BAD_REQUEST, "invalid_argument", f"format must be one of: {sorted(ALLOWED_FORMATS)}")

    if "timezone" in body:
        raise ApiError(
            HTTPStatus.BAD_REQUEST,
            "invalid_argument",
            "timezone is not supported in v1 (timestamps are always UTC)",
        )

    columns = parse_columns(body.get("columns"))
    validate_downsample_column_combination(resolution, columns)

    return SignalsQuery(
        start=start,
        end=end,
        resolution=resolution,
        fmt=fmt,
        columns=columns,
        provider_ids=provider_ids,
        device_ids=device_ids,
        signal_ids=signal_ids,
        original_request=body,
    )


def flux_quote(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def build_selector_filter(field_name: str, values: list[str]) -> str | None:
    if not values:
        return None
    clauses = [f'r.{field_name} == "{flux_quote(v)}"' for v in values]
    return " or ".join(clauses)


def build_field_filter(field_names: tuple[str, ...]) -> str:
    clauses = [f'r._field == "{flux_quote(field)}"' for field in field_names]
    return " or ".join(clauses)


def build_base_flux_lines(request: SignalsQuery, bucket: str) -> list[str]:
    start_iso = request.start.isoformat().replace("+00:00", "Z")
    end_iso = request.end.isoformat().replace("+00:00", "Z")

    lines = [
        f'from(bucket:"{flux_quote(bucket)}")',
        f'  |> range(start: time(v: "{start_iso}"), stop: time(v: "{end_iso}"))',
        '  |> filter(fn:(r) => r._measurement == "anolis_signal")',
    ]

    for field_name, values in (
        ("provider_id", request.provider_ids),
        ("device_id", request.device_ids),
        ("signal_id", request.signal_ids),
    ):
        expr = build_selector_filter(field_name, values)
        if expr:
            lines.append(f"  |> filter(fn:(r) => {expr})")

    return lines


def build_base_flux_pipeline(request: SignalsQuery, bucket: str) -> str:
    return "\n".join(build_base_flux_lines(request, bucket))


def build_downsample_query(request: SignalsQuery, bucket: str) -> str:
    base = build_base_flux_pipeline(request, bucket)
    numeric_filter = build_field_filter(NUMERIC_VALUE_FIELDS)
    non_numeric_filter = build_field_filter(NON_NUMERIC_VALUE_FIELDS + AUX_FIELDS_LAST_ONLY)
    numeric_agg = request.resolution.aggregation

    return "\n".join(
        [
            "numeric = (",
            base,
            f"  |> filter(fn:(r) => {numeric_filter})",
            (
                "  |> aggregateWindow("
                f"every: {request.resolution.interval}, "
                f"fn: {numeric_agg}, "
                "createEmpty: false)"
            ),
            ")",
            "",
            "non_numeric = (",
            base,
            f"  |> filter(fn:(r) => {non_numeric_filter})",
            (
                "  |> aggregateWindow("
                f"every: {request.resolution.interval}, "
                "fn: last, "
                "createEmpty: false)"
            ),
            ")",
            "",
            "union(tables:[numeric, non_numeric])",
            (
                '  |> pivot(rowKey:["_time","provider_id","device_id","signal_id"], '
                'columnKey:["_field"], valueColumn:"_value")'
            ),
            (
                '  |> keep(columns:["_time","provider_id","device_id","signal_id","quality",'
                '"value_double","value_int","value_uint","value_bool","value_string"])'
            ),
            '  |> sort(columns:["_time","provider_id","device_id","signal_id"])',
        ]
    )


def build_raw_or_project_query(request: SignalsQuery, bucket: str) -> str:
    lines = build_base_flux_lines(request, bucket)

    lines.extend(
        [
            (
                '  |> pivot(rowKey:["_time","provider_id","device_id","signal_id"], '
                'columnKey:["_field"], valueColumn:"_value")'
            ),
            (
                '  |> keep(columns:["_time","provider_id","device_id","signal_id","quality",'
                '"value_double","value_int","value_uint","value_bool","value_string"])'
            ),
            '  |> sort(columns:["_time","provider_id","device_id","signal_id"])',
        ]
    )

    return "\n".join(lines)


def build_flux_query(request: SignalsQuery, bucket: str) -> str:
    if request.resolution.mode == "downsampled":
        return build_downsample_query(request, bucket)
    return build_raw_or_project_query(request, bucket)


def influx_query_csv(config: AppConfig, flux_query: str) -> str:
    url = f"{config.influx.url}/api/v2/query"
    headers = {
        "Authorization": f"Token {config.influx.token}",
        "Accept": "application/csv",
        "Content-Type": "application/vnd.flux",
    }

    try:
        response = requests.post(
            url,
            params={"org": config.influx.org},
            headers=headers,
            data=flux_query.encode("utf-8"),
            timeout=config.limits.request_timeout_seconds,
        )
    except requests.Timeout as exc:
        raise ApiError(HTTPStatus.GATEWAY_TIMEOUT, "upstream_timeout", "InfluxDB query timed out") from exc
    except requests.RequestException as exc:
        raise ApiError(HTTPStatus.BAD_GATEWAY, "upstream_error", f"InfluxDB request failed: {exc}") from exc

    if response.status_code < 200 or response.status_code >= 300:
        detail = response.text.strip()
        if len(detail) > 300:
            detail = detail[:300] + "..."
        raise ApiError(
            HTTPStatus.BAD_GATEWAY,
            "upstream_error",
            f"InfluxDB query failed with status={response.status_code}: {detail}",
        )

    return response.text


def parse_influx_csv_rows(csv_text: str) -> list[dict[str, str]]:
    data_lines = [line for line in csv_text.splitlines() if line and not line.startswith("#")]
    if not data_lines:
        return []

    reader = csv.DictReader(data_lines)
    rows: list[dict[str, str]] = []
    for row in reader:
        normalized: dict[str, str] = {}
        for key, value in row.items():
            if key is None:
                continue
            normalized[key] = "" if value is None else value
        rows.append(normalized)
    return rows


def infer_value_and_type(row: dict[str, str]) -> tuple[Any, str]:
    if row.get("value_double", "") != "":
        try:
            return float(row["value_double"]), "double"
        except ValueError:
            return row["value_double"], "double"
    if row.get("value_int", "") != "":
        try:
            return int(row["value_int"]), "int64"
        except ValueError:
            return row["value_int"], "int64"
    if row.get("value_uint", "") != "":
        try:
            return int(row["value_uint"]), "uint64"
        except ValueError:
            return row["value_uint"], "uint64"
    if row.get("value_bool", "") != "":
        text = row["value_bool"].strip().lower()
        if text == "true":
            return True, "bool"
        if text == "false":
            return False, "bool"
        return row["value_bool"], "bool"
    if row.get("value_string", "") != "":
        return row["value_string"], "string"
    return None, "unknown"


def normalize_rows(raw_rows: list[dict[str, str]], columns: list[str]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []

    for raw in raw_rows:
        value, value_type = infer_value_and_type(raw)
        normalized = {
            "timestamp": raw.get("_time", ""),
            "provider_id": raw.get("provider_id", ""),
            "device_id": raw.get("device_id", ""),
            "signal_id": raw.get("signal_id", ""),
            "quality": raw.get("quality", ""),
            "value": value,
            "value_type": value_type,
            "value_double": raw.get("value_double", ""),
            "value_int": raw.get("value_int", ""),
            "value_uint": raw.get("value_uint", ""),
            "value_bool": raw.get("value_bool", ""),
            "value_string": raw.get("value_string", ""),
        }
        result.append({key: normalized.get(key) for key in columns})

    return result


def build_manifest(
    request: SignalsQuery,
    config: AppConfig,
    row_count: int,
    *,
    request_id: str,
    requester_id: str,
) -> dict[str, Any]:
    request_hash = hashlib.sha256(json.dumps(request.original_request, sort_keys=True).encode("utf-8")).hexdigest()
    resolution: dict[str, Any] = {"mode": request.resolution.mode}
    if request.resolution.mode == "downsampled":
        resolution["interval"] = request.resolution.interval
        resolution["aggregation"] = request.resolution.aggregation

    return {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "request_hash": f"sha256:{request_hash}",
        "request_id": request_id,
        "requester_id": requester_id,
        "source": {
            "org": config.influx.org,
            "bucket": config.influx.bucket,
            "url": config.influx.url,
        },
        "range": {
            "start": request.start.isoformat().replace("+00:00", "Z"),
            "end": request.end.isoformat().replace("+00:00", "Z"),
        },
        "resolution": resolution,
        "row_count": row_count,
    }


def render_csv(rows: list[dict[str, Any]], columns: list[str]) -> str:
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=columns)
    writer.writeheader()
    for row in rows:
        writer.writerow(row)
    return buffer.getvalue()


def json_error_payload(code: str, message: str) -> dict[str, Any]:
    return {
        "status": "error",
        "error": {
            "code": code,
            "message": message,
        },
    }


def coerce_request_id(value: str | None) -> str:
    if not value or not value.strip():
        return str(uuid.uuid4())

    candidate = value.strip()[:128]
    sanitized = "".join(ch for ch in candidate if ch.isalnum() or ch in "-_.:")
    return sanitized or str(uuid.uuid4())


def coerce_requester_id(value: str | None) -> str:
    if not value or not value.strip():
        return "anonymous"

    candidate = value.strip()[:128]
    sanitized = "".join(ch for ch in candidate if ch.isalnum() or ch in "-_.:@/")
    return sanitized or "anonymous"


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
            status, payload = self.service.execute_query(body, request_id=request_id, requester_id=requester_id)

            if payload.get("format") == "csv" and isinstance(payload.get("csv_body"), str):
                self.send_csv(
                    status,
                    csv_body=payload["csv_body"],
                    manifest=payload.get("manifest"),
                    request_id=request_id,
                    requester_id=requester_id,
                )
                return

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

    def send_csv(
        self,
        status: int | HTTPStatus,
        *,
        csv_body: str,
        manifest: Any,
        request_id: str,
        requester_id: str,
    ) -> None:
        body = csv_body.encode("utf-8")
        self.send_response(int(status))
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Request-Id", request_id)
        self.send_header("X-Requester-Id", requester_id)

        if isinstance(manifest, dict):
            self.send_header("X-Export-Manifest", json.dumps(manifest, separators=(",", ":")))

        self.end_headers()
        self.wfile.write(body)


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
