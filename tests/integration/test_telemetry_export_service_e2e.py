"""End-to-end export service integration against a live InfluxDB fixture."""
# ruff: noqa: I001

from __future__ import annotations

from collections.abc import Generator
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time
from typing import Any

import pytest
import requests
import yaml


pytestmark = pytest.mark.skipif(
    os.getenv("ANOLIS_EXPORT_E2E", "0") != "1",
    reason="set ANOLIS_EXPORT_E2E=1 to run export service e2e integration tests",
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _write_points(influx_url: str, token: str, org: str, bucket: str, lines: list[str]) -> None:
    payload = "\n".join(lines) + "\n"
    response = requests.post(
        f"{influx_url}/api/v2/write",
        params={"org": org, "bucket": bucket, "precision": "ms"},
        headers={"Authorization": f"Token {token}", "Content-Type": "text/plain; charset=utf-8"},
        data=payload.encode("utf-8"),
        timeout=15,
    )
    response.raise_for_status()


def _wait_for_http_ok(url: str, timeout_seconds: int = 30) -> None:
    start = time.time()
    while time.time() - start < timeout_seconds:
        try:
            response = requests.get(url, timeout=2)
            if response.status_code == 200:
                return
        except requests.RequestException:
            pass
        time.sleep(0.3)
    raise RuntimeError(f"Timed out waiting for healthy endpoint: {url}")


@pytest.fixture()
def export_service_process() -> Generator[tuple[subprocess.Popen[str], dict[str, Any]], None, None]:
    repo_root = _repo_root()
    influx_cfg = {
        "url": os.getenv("ANOLIS_EXPORT_E2E_INFLUX_URL", "http://127.0.0.1:8086"),
        "org": os.getenv("ANOLIS_EXPORT_E2E_INFLUX_ORG", "anolis"),
        "bucket": os.getenv("ANOLIS_EXPORT_E2E_INFLUX_BUCKET", "anolis"),
        "token": os.getenv("ANOLIS_EXPORT_E2E_INFLUX_TOKEN", "dev-token"),
    }
    service_cfg = {
        "server": {"host": "127.0.0.1", "port": 18091, "auth_token": "export-e2e-token"},
        "influxdb": influx_cfg,
        "authorization": {
            "enforce_selector_scope": True,
            "allowed_runtime_names": ["e2e-runtime"],
            "allowed_provider_ids": ["bread0"],
            "allowed_device_ids": [],
            "allowed_signal_ids": [],
        },
        "limits": {
            "max_span_seconds": 86400,
            "max_rows": 5000,
            "max_response_bytes": 5000,
            "max_selector_items": 128,
            "request_timeout_seconds": 15,
            "max_request_bytes": 200000,
        },
    }

    with tempfile.TemporaryDirectory(prefix="anolis_export_e2e_") as tmp_dir:
        cfg_path = Path(tmp_dir) / "telemetry-export.e2e.yaml"
        cfg_path.write_text(yaml.safe_dump(service_cfg, sort_keys=False), encoding="utf-8")

        process = subprocess.Popen(
            [sys.executable, "-m", "tools.telemetry_export.export_service", "--config", str(cfg_path)],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            _wait_for_http_ok("http://127.0.0.1:18091/v1/health", timeout_seconds=30)
            yield process, service_cfg
        except Exception as exc:
            exit_code = process.poll()
            startup_output = ""
            if exit_code is not None and process.stdout is not None:
                try:
                    startup_output = process.stdout.read().strip()
                except Exception:
                    startup_output = "<unable to read process output>"
            detail = (
                f"export service failed to start (exit_code={exit_code}). startup_output={startup_output or '<none>'}"
            )
            raise RuntimeError(detail) from exc
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()


def test_export_service_e2e_paths(
    export_service_process: tuple[subprocess.Popen[str], dict[str, Any]],
) -> None:
    _process, service_cfg = export_service_process
    influx_cfg = service_cfg["influxdb"]
    base_url = "http://127.0.0.1:18091"
    headers = {
        "Authorization": "Bearer export-e2e-token",
        "Content-Type": "application/json",
        "X-Requester-Id": "pytest-e2e",
    }

    base_ts = 1711929600000
    points = [
        (
            "anolis_signal,runtime_name=e2e-runtime,provider_id=bread0,device_id=dcmt0,signal_id=motor.rpm "
            f'value_double=120.5,quality="OK" {base_ts}'
        ),
        (
            "anolis_signal,runtime_name=e2e-runtime,provider_id=bread0,device_id=rlht0,signal_id=note "
            f'value_string="line1\\\\nline2",quality="OK" {base_ts + 10}'
        ),
        (
            "anolis_signal,runtime_name=e2e-runtime,provider_id=ezo0,device_id=ph0,signal_id=ph.value "
            f'value_double=7.12,quality="OK" {base_ts + 20}'
        ),
    ]
    for idx in range(350):
        points.append(
            "anolis_signal,runtime_name=e2e-runtime,provider_id=bread0,device_id=dcmt0,signal_id=motor.rpm "
            f'value_double={100 + idx / 10.0},quality="OK" {base_ts + 100 + idx}'
        )
    _write_points(influx_cfg["url"], influx_cfg["token"], influx_cfg["org"], influx_cfg["bucket"], points)

    raw_request = {
        "time_range": {"start": "2024-04-01T00:00:00Z", "end": "2024-04-01T01:00:00Z"},
        "selector": {
            "runtime_names": ["e2e-runtime"],
            "provider_ids": ["bread0"],
            "device_ids": ["rlht0"],
        },
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }
    raw_response = requests.post(
        f"{base_url}/v1/exports/signals:query",
        headers=headers,
        json=raw_request,
        timeout=30,
    )
    assert raw_response.status_code == 200, raw_response.text
    raw_payload = raw_response.json()
    assert raw_payload["status"] == "ok"
    assert raw_payload["manifest"]["row_count"] > 0
    assert raw_payload["manifest"]["export_id"]

    csv_request = {
        "time_range": {"start": "2024-04-01T00:00:00Z", "end": "2024-04-01T01:00:00Z"},
        "selector": {
            "runtime_names": ["e2e-runtime"],
            "provider_ids": ["bread0"],
        },
        "resolution": {"mode": "downsampled", "interval": "10s", "aggregation": "last"},
        "format": "csv",
    }
    csv_response = requests.post(
        f"{base_url}/v1/exports/signals:query",
        headers=headers,
        json=csv_request,
        timeout=30,
    )
    assert csv_response.status_code == 200
    assert csv_response.headers["Content-Type"].startswith("text/csv")
    assert "X-Export-Manifest" not in csv_response.headers
    assert csv_response.headers.get("X-Export-Id")
    assert csv_response.headers.get("X-Export-Manifest-Hash", "").startswith("sha256:")

    export_id = csv_response.headers["X-Export-Id"]
    manifest_response = requests.get(
        f"{base_url}/v1/exports/manifests/{export_id}",
        headers={"Authorization": "Bearer export-e2e-token"},
        timeout=10,
    )
    assert manifest_response.status_code == 200
    manifest_payload = manifest_response.json()
    assert manifest_payload["status"] == "ok"
    assert manifest_payload["manifest"]["export_id"] == export_id

    ndjson_request = {
        "time_range": {"start": "2024-04-01T00:00:00Z", "end": "2024-04-01T01:00:00Z"},
        "selector": {"runtime_names": ["e2e-runtime"], "provider_ids": ["bread0"]},
        "resolution": {"mode": "raw_event"},
        "format": "ndjson",
    }
    ndjson_response = requests.post(
        f"{base_url}/v1/exports/signals:query",
        headers=headers,
        json=ndjson_request,
        timeout=30,
    )
    assert ndjson_response.status_code == 200, ndjson_response.text
    assert ndjson_response.headers["Content-Type"].startswith("application/x-ndjson")
    assert len([line for line in ndjson_response.text.splitlines() if line.strip()]) > 0

    large_request = {
        "time_range": {"start": "2024-04-01T00:00:00Z", "end": "2024-04-01T01:00:00Z"},
        "selector": {
            "runtime_names": ["e2e-runtime"],
            "provider_ids": ["bread0"],
        },
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }
    large_response = requests.post(
        f"{base_url}/v1/exports/signals:query",
        headers=headers,
        json=large_request,
        timeout=30,
    )
    assert large_response.status_code == 413

    denied_request = {
        "time_range": {"start": "2024-04-01T00:00:00Z", "end": "2024-04-01T01:00:00Z"},
        "selector": {"runtime_names": ["e2e-runtime"], "provider_ids": ["ezo0"]},
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }
    denied_response = requests.post(
        f"{base_url}/v1/exports/signals:query",
        headers=headers,
        json=denied_request,
        timeout=30,
    )
    assert denied_response.status_code == 403
