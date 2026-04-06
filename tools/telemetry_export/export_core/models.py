"""Core data models and shared constants for telemetry export service."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

DEFAULT_COLUMNS = [
    "timestamp",
    "runtime_name",
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
    allowed_runtime_names: tuple[str, ...]
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
    runtime_names: list[str]
    provider_ids: list[str]
    device_ids: list[str]
    signal_ids: list[str]
    original_request: dict[str, Any]


@dataclass(frozen=True)
class CsvSpoolResult:
    path: Path
    row_count: int
    content_length: int
    manifest: dict[str, Any]
