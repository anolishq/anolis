"""InfluxDB query client helpers for export service."""

from __future__ import annotations

import logging
from http import HTTPStatus

import requests

from .models import ApiError, AppConfig

LOGGER = logging.getLogger("telemetry_export")


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


def influx_query_csv_stream(config: AppConfig, flux_query: str) -> requests.Response:
    url = f"{config.influx.url}/api/v2/query"
    headers = {
        "Authorization": f"Token {config.influx.token}",
        "Accept": "application/csv",
        "Accept-Encoding": "identity",
        "Content-Type": "application/vnd.flux",
    }

    try:
        response = requests.post(
            url,
            params={"org": config.influx.org},
            headers=headers,
            data=flux_query.encode("utf-8"),
            timeout=config.limits.request_timeout_seconds,
            stream=True,
        )
    except requests.Timeout as exc:
        raise ApiError(HTTPStatus.GATEWAY_TIMEOUT, "upstream_timeout", "InfluxDB query timed out") from exc
    except requests.RequestException as exc:
        raise ApiError(HTTPStatus.BAD_GATEWAY, "upstream_error", f"InfluxDB request failed: {exc}") from exc

    if response.status_code < 200 or response.status_code >= 300:
        detail = ""
        try:
            detail = response.text.strip()
        except Exception:
            detail = ""
        if len(detail) > 300:
            detail = detail[:300] + "..."
        response.close()
        raise ApiError(
            HTTPStatus.BAD_GATEWAY,
            "upstream_error",
            f"InfluxDB query failed with status={response.status_code}: {detail}",
        )

    raw_stream = getattr(response, "raw", None)
    if raw_stream is not None:
        try:
            raw_stream.decode_content = True
        except Exception:
            LOGGER.debug("Unable to set response.raw.decode_content=True", exc_info=True)

    LOGGER.debug(
        "Influx stream response: status=%s content_encoding=%s content_type=%s transfer_encoding=%s",
        response.status_code,
        response.headers.get("Content-Encoding", ""),
        response.headers.get("Content-Type", ""),
        response.headers.get("Transfer-Encoding", ""),
    )

    return response
