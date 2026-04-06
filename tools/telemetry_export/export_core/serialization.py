"""CSV parsing, row normalization, and response payload helpers."""

from __future__ import annotations

import csv
import hashlib
import io
import json
import uuid
from datetime import datetime, timezone
from typing import Any

from .models import AppConfig, SignalsQuery


def iter_influx_csv_rows(stream_response: Any) -> Any:
    raw_stream = getattr(stream_response, "raw", None)
    if raw_stream is not None:
        text_stream = io.TextIOWrapper(raw_stream, encoding="utf-8", newline="")
        try:
            yield from _iter_influx_csv_rows_from_reader(csv.reader(text_stream))
        finally:
            text_stream.detach()
        return

    yield from _iter_influx_csv_rows_from_iter_lines(stream_response)


def _iter_influx_csv_rows_from_reader(reader: Any) -> Any:
    header: list[str] | None = None

    for parsed in reader:
        if not parsed:
            continue
        first = parsed[0].strip() if parsed[0] else ""
        if first.startswith("#"):
            continue
        if header is None:
            header = parsed
            continue

        yield _normalize_csv_record(header, parsed)


def _iter_influx_csv_rows_from_iter_lines(stream_response: Any) -> Any:
    header: list[str] | None = None
    for raw_line in stream_response.iter_lines(decode_unicode=True):
        if raw_line is None:
            continue
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parsed = next(csv.reader([line]))
        if header is None:
            header = parsed
            continue

        yield _normalize_csv_record(header, parsed)


def _normalize_csv_record(header: list[str], parsed: list[str]) -> dict[str, str]:
    normalized: dict[str, str] = {}
    for idx, key in enumerate(header):
        if not key:
            continue
        normalized[key] = parsed[idx] if idx < len(parsed) else ""
    return normalized


def parse_influx_csv_rows(csv_text: str) -> list[dict[str, str]]:
    reader = csv.reader(io.StringIO(csv_text, newline=""))
    return list(_iter_influx_csv_rows_from_reader(reader))


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
    return [normalize_row(raw, columns) for raw in raw_rows]


def normalize_row(raw: dict[str, str], columns: list[str]) -> dict[str, Any]:
    value, value_type = infer_value_and_type(raw)
    normalized = {
        "timestamp": raw.get("_time", ""),
        "runtime_name": raw.get("runtime_name", ""),
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
    return {key: normalized.get(key) for key in columns}


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
