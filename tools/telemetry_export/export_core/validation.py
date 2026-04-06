"""Request validation and query model construction."""

from __future__ import annotations

import re
from datetime import datetime, timezone
from http import HTTPStatus
from typing import Any

from .models import (
    ALLOWED_AGGREGATIONS,
    ALLOWED_COLUMNS,
    ALLOWED_FORMATS,
    ALLOWED_RESOLUTION_MODES,
    DEFAULT_COLUMNS,
    NON_NUMERIC_TYPED_COLUMNS,
    ApiError,
    LimitConfig,
    Resolution,
    SignalsQuery,
)

INTERVAL_RE = re.compile(r"^[1-9][0-9]*(ms|s|m|h|d|w)$")


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

    runtime_names = ensure_string_list(
        selector.get("runtime_names"),
        "selector.runtime_names",
        limits.max_selector_items,
    )
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
        runtime_names=runtime_names,
        provider_ids=provider_ids,
        device_ids=device_ids,
        signal_ids=signal_ids,
        original_request=body,
    )
