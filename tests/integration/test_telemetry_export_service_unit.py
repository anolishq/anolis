"""Unit-style tests for telemetry export MVP service logic."""

from __future__ import annotations

import importlib.util
import io
import json
import sys
from pathlib import Path

import pytest


def _load_module():
    root = Path(__file__).resolve().parents[2]
    module_path = root / "tools" / "telemetry_export" / "export_service.py"
    spec = importlib.util.spec_from_file_location("telemetry_export_service", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _test_config(module):
    return module.AppConfig(
        server=module.ServerConfig(host="127.0.0.1", port=8091, auth_token="export-dev-token"),
        influx=module.InfluxConfig(url="http://127.0.0.1:8086", org="anolis", bucket="anolis", token="dev-token"),
        limits=module.LimitConfig(
            max_span_seconds=3600,
            max_rows=100,
            max_response_bytes=1_000_000,
            max_selector_items=16,
            request_timeout_seconds=2,
            max_request_bytes=200_000,
        ),
        authorization=module.AuthorizationConfig(
            enforce_selector_scope=False,
            allowed_runtime_names=(),
            allowed_provider_ids=(),
            allowed_device_ids=(),
            allowed_signal_ids=(),
        ),
    )


def _sample_csv_rows() -> str:
    return "\n".join(
        [
            ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
            ",,0,2026-04-01T00:00:01Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.5,,,,",
            ",,0,2026-04-01T00:00:02Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.6,,,,",
        ]
    )


def test_validate_query_request_accepts_raw_event():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {
            "runtime_names": ["bioreactor-telemetry"],
            "provider_ids": ["bread0", "ezo0"],
            "device_ids": ["rlht0", "ph0"],
            "signal_ids": ["tc1_temp", "ph.value"],
        },
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }

    parsed = module.validate_query_request(request, cfg.limits)

    assert parsed.resolution.mode == "raw_event"
    assert parsed.fmt == "json"
    assert parsed.runtime_names == ["bioreactor-telemetry"]
    assert parsed.provider_ids == ["bread0", "ezo0"]


def test_validate_query_request_rejects_invalid_downsample_interval():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "downsampled", "interval": "0s", "aggregation": "mean"},
    }

    with pytest.raises(module.ApiError) as exc_info:
        module.validate_query_request(request, cfg.limits)

    assert exc_info.value.status == 400
    assert exc_info.value.code == "invalid_argument"


def test_validate_query_request_rejects_timezone_input():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "timezone": "America/Toronto",
    }

    with pytest.raises(module.ApiError) as exc_info:
        module.validate_query_request(request, cfg.limits)

    assert exc_info.value.status == 400
    assert exc_info.value.code == "invalid_argument"


def test_validate_query_request_rejects_non_numeric_typed_columns_for_non_last_downsample():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "downsampled", "interval": "10s", "aggregation": "mean"},
        "columns": ["timestamp", "provider_id", "value_bool"],
    }

    with pytest.raises(module.ApiError) as exc_info:
        module.validate_query_request(request, cfg.limits)

    assert exc_info.value.status == 400
    assert exc_info.value.code == "invalid_argument"


def test_build_flux_query_includes_expected_filters_and_aggregate_window():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {
            "runtime_names": ["bioreactor-telemetry"],
            "provider_ids": ["bread0"],
            "device_ids": ["rlht0"],
            "signal_ids": ["tc1_temp"],
        },
        "resolution": {"mode": "downsampled", "interval": "10s", "aggregation": "last"},
        "format": "json",
    }

    parsed = module.validate_query_request(request, cfg.limits)
    flux = module.build_flux_query(parsed, cfg.influx.bucket)

    assert 'r._measurement == "anolis_signal"' in flux
    assert 'r.runtime_name == "bioreactor-telemetry"' in flux
    assert 'r.provider_id == "bread0"' in flux
    assert 'r.device_id == "rlht0"' in flux
    assert 'r.signal_id == "tc1_temp"' in flux
    assert "aggregateWindow(every: 10s, fn: last, createEmpty: false)" in flux


def test_build_flux_query_downsample_uses_last_for_non_numeric_fields():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {"provider_ids": ["bread0"]},
        "resolution": {"mode": "downsampled", "interval": "10s", "aggregation": "mean"},
        "format": "json",
    }
    parsed = module.validate_query_request(request, cfg.limits)
    flux = module.build_flux_query(parsed, cfg.influx.bucket)

    assert "numeric = (" in flux
    assert "non_numeric = (" in flux
    assert "union(tables:[numeric, non_numeric])" in flux
    assert "fn: mean" in flux
    assert "fn: last" in flux
    assert 'pivot(rowKey:["_time","runtime_name","provider_id","device_id","signal_id"]' in flux
    assert 'keep(columns:["_time","runtime_name","provider_id","device_id","signal_id","quality"' in flux


def test_build_flux_query_raw_snapshot_deterministic_selector_order():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {
            "runtime_names": ["rt-b", "rt-a", "rt-a"],
            "provider_ids": ["ezo0", "bread0"],
            "device_ids": ["ph0", "dcmt0"],
            "signal_ids": ["ph.value", "motor.rpm"],
        },
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }
    parsed = module.validate_query_request(request, cfg.limits)
    flux = module.build_flux_query(parsed, cfg.influx.bucket)

    expected = "\n".join(
        [
            'from(bucket:"anolis")',
            '  |> range(start: time(v: "2026-04-01T00:00:00Z"), stop: time(v: "2026-04-01T00:10:00Z"))',
            '  |> filter(fn:(r) => r._measurement == "anolis_signal")',
            '  |> filter(fn:(r) => r.runtime_name == "rt-a" or r.runtime_name == "rt-b")',
            '  |> filter(fn:(r) => r.provider_id == "bread0" or r.provider_id == "ezo0")',
            '  |> filter(fn:(r) => r.device_id == "dcmt0" or r.device_id == "ph0")',
            '  |> filter(fn:(r) => r.signal_id == "motor.rpm" or r.signal_id == "ph.value")',
            (
                '  |> pivot(rowKey:["_time","runtime_name","provider_id","device_id","signal_id"], '
                'columnKey:["_field"], valueColumn:"_value")'
            ),
            (
                '  |> keep(columns:["_time","runtime_name","provider_id","device_id","signal_id","quality",'
                '"value_double","value_int","value_uint","value_bool","value_string"])'
            ),
            '  |> sort(columns:["_time","runtime_name","provider_id","device_id","signal_id"])',
        ]
    )

    assert flux == expected


def test_build_flux_query_downsample_snapshot():
    module = _load_module()
    cfg = _test_config(module)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {"provider_ids": ["ezo0", "bread0"]},
        "resolution": {"mode": "downsampled", "interval": "10s", "aggregation": "max"},
        "format": "json",
    }
    parsed = module.validate_query_request(request, cfg.limits)
    flux = module.build_flux_query(parsed, cfg.influx.bucket)

    expected = "\n".join(
        [
            "numeric = (",
            'from(bucket:"anolis")',
            '  |> range(start: time(v: "2026-04-01T00:00:00Z"), stop: time(v: "2026-04-01T00:10:00Z"))',
            '  |> filter(fn:(r) => r._measurement == "anolis_signal")',
            '  |> filter(fn:(r) => r.provider_id == "bread0" or r.provider_id == "ezo0")',
            '  |> filter(fn:(r) => r._field == "value_double" or r._field == "value_int" or r._field == "value_uint")',
            "  |> aggregateWindow(every: 10s, fn: max, createEmpty: false)",
            ")",
            "",
            "non_numeric = (",
            'from(bucket:"anolis")',
            '  |> range(start: time(v: "2026-04-01T00:00:00Z"), stop: time(v: "2026-04-01T00:10:00Z"))',
            '  |> filter(fn:(r) => r._measurement == "anolis_signal")',
            '  |> filter(fn:(r) => r.provider_id == "bread0" or r.provider_id == "ezo0")',
            '  |> filter(fn:(r) => r._field == "value_bool" or r._field == "value_string" or r._field == "quality")',
            "  |> aggregateWindow(every: 10s, fn: last, createEmpty: false)",
            ")",
            "",
            "union(tables:[numeric, non_numeric])",
            (
                '  |> pivot(rowKey:["_time","runtime_name","provider_id","device_id","signal_id"], '
                'columnKey:["_field"], valueColumn:"_value")'
            ),
            (
                '  |> keep(columns:["_time","runtime_name","provider_id","device_id","signal_id","quality",'
                '"value_double","value_int","value_uint","value_bool","value_string"])'
            ),
            '  |> sort(columns:["_time","runtime_name","provider_id","device_id","signal_id"])',
        ]
    )

    assert flux == expected


def test_execute_query_enforces_auth_and_row_limit(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)
    svc = module.ExportService(cfg)

    with pytest.raises(module.ApiError) as exc_info:
        svc.authorize("Bearer wrong-token")
    assert exc_info.value.status == 401

    strict_cfg = module.AppConfig(
        server=cfg.server,
        influx=cfg.influx,
        limits=module.LimitConfig(
            max_span_seconds=cfg.limits.max_span_seconds,
            max_rows=1,
            max_response_bytes=cfg.limits.max_response_bytes,
            max_selector_items=cfg.limits.max_selector_items,
            request_timeout_seconds=cfg.limits.request_timeout_seconds,
            max_request_bytes=cfg.limits.max_request_bytes,
        ),
        authorization=cfg.authorization,
    )
    strict_svc = module.ExportService(strict_cfg)

    monkeypatch.setattr(module, "influx_query_csv", lambda _cfg, _query: _sample_csv_rows())

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }

    with pytest.raises(module.ApiError) as limit_exc:
        strict_svc.execute_query(request)

    assert limit_exc.value.status == 413
    assert limit_exc.value.code == "limit_exceeded"


def test_execute_query_enforces_selector_scope(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)

    scoped_cfg = module.AppConfig(
        server=cfg.server,
        influx=cfg.influx,
        limits=cfg.limits,
        authorization=module.AuthorizationConfig(
            enforce_selector_scope=True,
            allowed_runtime_names=("bioreactor-telemetry",),
            allowed_provider_ids=("bread0",),
            allowed_device_ids=(),
            allowed_signal_ids=(),
        ),
    )
    scoped_svc = module.ExportService(scoped_cfg)

    monkeypatch.setattr(module, "influx_query_csv", lambda _cfg, _query: _sample_csv_rows())

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {"provider_ids": ["ezo0"]},
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }

    with pytest.raises(module.ApiError) as scope_exc:
        scoped_svc.execute_query(request)

    assert scope_exc.value.status == 403
    assert scope_exc.value.code == "permission_denied"


def test_execute_query_enforces_runtime_scope(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)

    scoped_cfg = module.AppConfig(
        server=cfg.server,
        influx=cfg.influx,
        limits=cfg.limits,
        authorization=module.AuthorizationConfig(
            enforce_selector_scope=True,
            allowed_runtime_names=("bioreactor-telemetry",),
            allowed_provider_ids=(),
            allowed_device_ids=(),
            allowed_signal_ids=(),
        ),
    )
    scoped_svc = module.ExportService(scoped_cfg)

    monkeypatch.setattr(module, "influx_query_csv", lambda _cfg, _query: _sample_csv_rows())

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {"runtime_names": ["different-runtime"]},
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }

    with pytest.raises(module.ApiError) as scope_exc:
        scoped_svc.execute_query(request)

    assert scope_exc.value.status == 403
    assert scope_exc.value.code == "permission_denied"


def test_execute_query_csv_payload_contains_manifest_and_request_trace(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)
    svc = module.ExportService(cfg)

    monkeypatch.setattr(module, "influx_query_csv", lambda _cfg, _query: _sample_csv_rows())

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {"provider_ids": ["bread0"]},
        "resolution": {"mode": "raw_event"},
        "format": "csv",
    }

    status, payload = svc.execute_query(request, request_id="req-123", requester_id="operator-a")

    assert status == 200
    assert payload["format"] == "csv"
    assert "csv_body" in payload
    assert "runtime_name" in payload["csv_body"]
    assert "tc1_temp" in payload["csv_body"]
    assert payload["manifest"]["request_id"] == "req-123"
    assert payload["manifest"]["requester_id"] == "operator-a"


def test_load_config_prefers_env_over_config_tokens(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    module = _load_module()

    cfg_path = tmp_path / "telemetry-export.yaml"
    cfg_path.write_text(
        "\n".join(
            [
                "server:",
                "  host: 127.0.0.1",
                "  port: 8091",
                "  auth_token: config-auth-token",
                "influxdb:",
                "  url: http://127.0.0.1:8086",
                "  org: anolis",
                "  bucket: anolis",
                "  token: config-influx-token",
                "limits:",
                "  max_span_seconds: 86400",
                "  max_rows: 50000",
                "  max_response_bytes: 10000000",
                "  max_selector_items: 128",
                "  request_timeout_seconds: 15",
                "  max_request_bytes: 200000",
                "authorization:",
                "  enforce_selector_scope: true",
                "  allowed_runtime_names: [bioreactor-telemetry]",
            ]
        ),
        encoding="utf-8",
    )

    monkeypatch.setenv("ANOLIS_EXPORT_AUTH_TOKEN", "env-auth-token")
    monkeypatch.setenv("ANOLIS_EXPORT_INFLUX_TOKEN", "env-influx-token")

    loaded = module.load_config(cfg_path)
    assert loaded.server.auth_token == "env-auth-token"
    assert loaded.influx.token == "env-influx-token"
    assert loaded.authorization.allowed_runtime_names == ("bioreactor-telemetry",)


def test_load_config_rejects_invalid_server_port(tmp_path: Path):
    module = _load_module()

    cfg_path = tmp_path / "telemetry-export-invalid-port.yaml"
    cfg_path.write_text(
        "\n".join(
            [
                "server:",
                "  host: 127.0.0.1",
                "  port: 0",
                "  auth_token: export-dev-token",
                "influxdb:",
                "  url: http://127.0.0.1:8086",
                "  org: anolis",
                "  bucket: anolis",
                "  token: dev-token",
                "limits:",
                "  max_span_seconds: 86400",
                "  max_rows: 50000",
                "  max_response_bytes: 10000000",
                "  max_selector_items: 128",
                "  request_timeout_seconds: 15",
                "  max_request_bytes: 200000",
            ]
        ),
        encoding="utf-8",
    )

    with pytest.raises(RuntimeError) as exc_info:
        module.load_config(cfg_path)
    assert "server.port" in str(exc_info.value)


def test_load_config_allows_response_size_smaller_than_request_size(tmp_path: Path):
    module = _load_module()

    cfg_path = tmp_path / "telemetry-export-invalid-limits.yaml"
    cfg_path.write_text(
        "\n".join(
            [
                "server:",
                "  host: 127.0.0.1",
                "  port: 8091",
                "  auth_token: export-dev-token",
                "influxdb:",
                "  url: http://127.0.0.1:8086",
                "  org: anolis",
                "  bucket: anolis",
                "  token: dev-token",
                "limits:",
                "  max_span_seconds: 86400",
                "  max_rows: 50000",
                "  max_response_bytes: 1000",
                "  max_selector_items: 128",
                "  request_timeout_seconds: 15",
                "  max_request_bytes: 200000",
            ]
        ),
        encoding="utf-8",
    )

    loaded = module.load_config(cfg_path)
    assert loaded.limits.max_response_bytes == 1000
    assert loaded.limits.max_request_bytes == 200000


def test_load_config_rejects_scope_enforcement_without_allowlists(tmp_path: Path):
    module = _load_module()

    cfg_path = tmp_path / "telemetry-export-invalid-authz.yaml"
    cfg_path.write_text(
        "\n".join(
            [
                "server:",
                "  host: 127.0.0.1",
                "  port: 8091",
                "  auth_token: export-dev-token",
                "influxdb:",
                "  url: http://127.0.0.1:8086",
                "  org: anolis",
                "  bucket: anolis",
                "  token: dev-token",
                "limits:",
                "  max_span_seconds: 86400",
                "  max_rows: 50000",
                "  max_response_bytes: 10000000",
                "  max_selector_items: 128",
                "  request_timeout_seconds: 15",
                "  max_request_bytes: 200000",
                "authorization:",
                "  enforce_selector_scope: true",
                "  allowed_runtime_names: []",
                "  allowed_provider_ids: []",
                "  allowed_device_ids: []",
                "  allowed_signal_ids: []",
            ]
        ),
        encoding="utf-8",
    )

    with pytest.raises(RuntimeError) as exc_info:
        module.load_config(cfg_path)
    assert "authorization" in str(exc_info.value)


def test_execute_csv_spooled_query_writes_temp_file_and_manifest(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)
    svc = module.ExportService(cfg)

    class _FakeStreamResponse:
        def __init__(self, lines: list[str]):
            self._lines = lines
            self.closed = False

        def iter_lines(self, decode_unicode: bool = True):
            assert decode_unicode is True
            for line in self._lines:
                yield line

        def close(self):
            self.closed = True

    fake_lines = [
        "#group,false,false,true,true,true,true,false,false,false,false,false,false,false",
        "#datatype,string,long,dateTime:RFC3339,string,string,string,string,string,double,long,unsignedLong,string,string",
        ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
        ",,0,2026-04-01T00:00:01Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.5,,,,",
        ",,0,2026-04-01T00:00:02Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.6,,,,",
    ]

    monkeypatch.setattr(module, "influx_query_csv_stream", lambda _cfg, _query: _FakeStreamResponse(fake_lines))

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "selector": {"runtime_names": ["bioreactor-telemetry"], "provider_ids": ["bread0"]},
        "resolution": {"mode": "raw_event"},
        "format": "csv",
    }

    result = svc.execute_csv_spooled_query(request, request_id="req-spool", requester_id="operator-a")
    try:
        assert result.row_count == 2
        assert result.content_length > 0
        assert result.manifest["request_id"] == "req-spool"
        assert result.manifest["requester_id"] == "operator-a"
        assert result.export_id
        assert result.manifest_hash.startswith("sha256:")
        assert svc.get_manifest(result.export_id) is not None
        csv_text = result.path.read_text(encoding="utf-8")
        assert "runtime_name" in csv_text
        assert "bioreactor-telemetry" in csv_text
        assert "tc1_temp" in csv_text
    finally:
        result.path.unlink(missing_ok=True)


def test_execute_query_from_query_does_not_revalidate(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)
    svc = module.ExportService(cfg)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }
    parsed = module.validate_query_request(request, cfg.limits)

    monkeypatch.setattr(
        module, "validate_query_request", lambda _body, _limits: (_ for _ in ()).throw(AssertionError())
    )
    monkeypatch.setattr(module, "influx_query_csv", lambda _cfg, _query: _sample_csv_rows())

    status, payload = svc.execute_query_from_query(parsed, request_id="req-from-query", requester_id="operator-a")
    assert status == 200
    assert payload["format"] == "json"
    assert payload["manifest"]["request_id"] == "req-from-query"


def test_execute_csv_spooled_query_from_query_does_not_revalidate(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)
    svc = module.ExportService(cfg)

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "format": "csv",
    }
    parsed = module.validate_query_request(request, cfg.limits)

    class _FakeStreamResponse:
        def __init__(self, lines: list[str]):
            self._lines = lines

        def iter_lines(self, decode_unicode: bool = True):
            assert decode_unicode is True
            for line in self._lines:
                yield line

        def close(self):
            return None

    fake_lines = [
        ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
        ",,0,2026-04-01T00:00:01Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.5,,,,",
    ]

    monkeypatch.setattr(
        module, "validate_query_request", lambda _body, _limits: (_ for _ in ()).throw(AssertionError())
    )
    monkeypatch.setattr(module, "influx_query_csv_stream", lambda _cfg, _query: _FakeStreamResponse(fake_lines))

    result = svc.execute_csv_spooled_query_from_query(parsed, request_id="req-spooled", requester_id="operator-a")
    try:
        assert result.row_count == 1
        assert result.manifest["request_id"] == "req-spooled"
    finally:
        result.path.unlink(missing_ok=True)


def test_execute_spooled_query_supports_ndjson(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    cfg = _test_config(module)
    svc = module.ExportService(cfg)

    class _FakeStreamResponse:
        def __init__(self, lines: list[str]):
            self._lines = lines

        def iter_lines(self, decode_unicode: bool = True):
            assert decode_unicode is True
            for line in self._lines:
                yield line

        def close(self):
            return None

    fake_lines = [
        ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
        ",,0,2026-04-01T00:00:01Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.5,,,,",
        ",,0,2026-04-01T00:00:02Z,bioreactor-telemetry,bread0,rlht0,tc1_temp,OK,23.6,,,,",
    ]

    monkeypatch.setattr(module, "influx_query_csv_stream", lambda _cfg, _query: _FakeStreamResponse(fake_lines))
    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "format": "ndjson",
    }

    result = svc.execute_spooled_query(request, request_id="req-ndjson", requester_id="operator-a")
    try:
        assert result.fmt == "ndjson"
        assert result.content_type.startswith("application/x-ndjson")
        body = result.path.read_text(encoding="utf-8")
        lines = [line for line in body.splitlines() if line.strip()]
        assert len(lines) == 2
        first = json.loads(lines[0])
        assert first["runtime_name"] == "bioreactor-telemetry"
        assert first["signal_id"] == "tc1_temp"
    finally:
        result.path.unlink(missing_ok=True)


def test_execute_spooled_query_ndjson_is_not_limited_by_max_response_bytes(
    monkeypatch: pytest.MonkeyPatch,
):
    module = _load_module()
    base_cfg = _test_config(module)
    strict_cfg = module.AppConfig(
        server=base_cfg.server,
        influx=base_cfg.influx,
        limits=module.LimitConfig(
            max_span_seconds=base_cfg.limits.max_span_seconds,
            max_rows=base_cfg.limits.max_rows,
            max_response_bytes=64,
            max_selector_items=base_cfg.limits.max_selector_items,
            request_timeout_seconds=base_cfg.limits.request_timeout_seconds,
            max_request_bytes=base_cfg.limits.max_request_bytes,
        ),
        authorization=base_cfg.authorization,
    )
    svc = module.ExportService(strict_cfg)

    class _FakeStreamResponse:
        def __init__(self, lines: list[str]):
            self._lines = lines

        def iter_lines(self, decode_unicode: bool = True):
            assert decode_unicode is True
            for line in self._lines:
                yield line

        def close(self):
            return None

    fake_lines = [
        ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
    ]
    for i in range(10):
        fake_lines.append(
            f",,0,2026-04-01T00:00:{i:02d}Z,bioreactor-telemetry,bread0,dcmt0,motor.rpm,OK,{120 + i / 10.0},,,,"
        )

    monkeypatch.setattr(module, "influx_query_csv_stream", lambda _cfg, _query: _FakeStreamResponse(fake_lines))

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "format": "ndjson",
    }

    result = svc.execute_spooled_query(request, request_id="req-ndjson-big", requester_id="operator-a")
    try:
        assert result.fmt == "ndjson"
        assert result.row_count == 10
        assert result.content_length > strict_cfg.limits.max_response_bytes
    finally:
        result.path.unlink(missing_ok=True)


def test_execute_spooled_query_json_enforces_max_response_bytes(monkeypatch: pytest.MonkeyPatch):
    module = _load_module()
    base_cfg = _test_config(module)
    strict_cfg = module.AppConfig(
        server=base_cfg.server,
        influx=base_cfg.influx,
        limits=module.LimitConfig(
            max_span_seconds=base_cfg.limits.max_span_seconds,
            max_rows=base_cfg.limits.max_rows,
            max_response_bytes=128,
            max_selector_items=base_cfg.limits.max_selector_items,
            request_timeout_seconds=base_cfg.limits.request_timeout_seconds,
            max_request_bytes=base_cfg.limits.max_request_bytes,
        ),
        authorization=base_cfg.authorization,
    )
    svc = module.ExportService(strict_cfg)

    class _FakeStreamResponse:
        def __init__(self, lines: list[str]):
            self._lines = lines

        def iter_lines(self, decode_unicode: bool = True):
            assert decode_unicode is True
            for line in self._lines:
                yield line

        def close(self):
            return None

    fake_lines = [
        ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
    ]
    for i in range(10):
        fake_lines.append(
            f",,0,2026-04-01T00:00:{i:02d}Z,bioreactor-telemetry,bread0,dcmt0,motor.rpm,OK,{120 + i / 10.0},,,,"
        )

    monkeypatch.setattr(module, "influx_query_csv_stream", lambda _cfg, _query: _FakeStreamResponse(fake_lines))

    request = {
        "time_range": {"start": "2026-04-01T00:00:00Z", "end": "2026-04-01T00:10:00Z"},
        "resolution": {"mode": "raw_event"},
        "format": "json",
    }

    with pytest.raises(module.ApiError) as exc_info:
        svc.execute_spooled_query(request, request_id="req-json-big", requester_id="operator-a")

    assert exc_info.value.status == 413
    assert exc_info.value.code == "limit_exceeded"


def test_iter_influx_csv_rows_supports_multiline_values_from_raw_stream():
    module = _load_module()

    csv_text = "\n".join(
        [
            "#group,false,false,true,true,true,true,false,false,false,false,false,false,false",
            "#datatype,string,long,dateTime:RFC3339,string,string,string,string,string,double,long,unsignedLong,string,string",
            ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
            ',,0,2026-04-01T00:00:01Z,bioreactor-telemetry,bread0,rlht0,note,OK,,,,,"line1',
            'line2"',
        ]
    )

    class _FakeRawResponse:
        def __init__(self, payload: str):
            self.raw = io.BytesIO(payload.encode("utf-8"))

    rows = list(module.iter_influx_csv_rows(_FakeRawResponse(csv_text)))
    assert len(rows) == 1
    assert rows[0]["signal_id"] == "note"
    assert rows[0]["value_string"] == "line1\nline2"


def test_parse_influx_csv_rows_supports_multiline_values():
    module = _load_module()

    csv_text = "\n".join(
        [
            "#group,false,false,true,true,true,true,false,false,false,false,false,false,false",
            "#datatype,string,long,dateTime:RFC3339,string,string,string,string,string,double,long,unsignedLong,string,string",
            ",result,table,_time,runtime_name,provider_id,device_id,signal_id,quality,value_double,value_int,value_uint,value_bool,value_string",
            ',,0,2026-04-01T00:00:01Z,bioreactor-telemetry,bread0,rlht0,note,OK,,,,,"line1',
            'line2"',
        ]
    )

    rows = module.parse_influx_csv_rows(csv_text)
    assert len(rows) == 1
    assert rows[0]["signal_id"] == "note"
    assert rows[0]["value_string"] == "line1\nline2"
