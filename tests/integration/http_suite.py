"""HTTP gateway integration checks (pytest-oriented)."""

from __future__ import annotations

import json
import threading
from pathlib import Path
from typing import Any, Callable, Dict, List, Tuple

import requests

from tests.support.api_helpers import assert_http_available, wait_for_condition
from tests.support.runtime_fixture import RuntimeFixture


class HttpGatewayTester:
    """HTTP API check harness."""

    def __init__(self, runtime_path: Path, provider_path: Path, port: int = 8080, timeout: float = 30.0):
        self.port = port
        self.base_url = f"http://127.0.0.1:{port}"
        self.timeout = timeout
        self.fixture = RuntimeFixture(runtime_path, provider_path, http_port=port)

    def cleanup(self) -> None:
        self.fixture.cleanup()

    def start_runtime(self) -> None:
        if not self.fixture.start(
            wait_for_ready=True,
            provider_id="sim0",
            min_device_count=1,
            startup_timeout=min(self.timeout, 30.0),
        ):
            raise AssertionError("Runtime failed to start\n" + self._output_tail())

    def _output_tail(self, lines: int = 80) -> str:
        capture = self.fixture.get_output_capture()
        if capture is None:
            return "(no output capture)"
        return capture.get_recent_output(lines)

    def http_get(self, path: str, timeout: float = 5.0) -> Dict[str, Any]:
        try:
            resp = requests.get(f"{self.base_url}{path}", timeout=timeout)
            body: Dict[str, Any] = {}
            try:
                body = resp.json()
            except ValueError:
                body = {"raw": resp.text}
            return {"status_code": resp.status_code, "body": body}
        except Exception as exc:
            return {"status_code": 0, "error": str(exc)}

    def http_post(self, path: str, data: Dict[str, Any], timeout: float = 5.0) -> Dict[str, Any]:
        try:
            resp = requests.post(f"{self.base_url}{path}", json=data, timeout=timeout)
            body: Dict[str, Any] = {}
            try:
                body = resp.json()
            except ValueError:
                body = {"raw": resp.text}
            return {"status_code": resp.status_code, "body": body}
        except Exception as exc:
            return {"status_code": 0, "error": str(exc)}

    def check_runtime_startup_and_status(self) -> None:
        assert assert_http_available(self.base_url, timeout=15), (
            "Could not connect to HTTP server\n" + self._output_tail()
        )

        result = self.http_get("/v0/runtime/status")
        assert result["status_code"] == 200, f"Expected 200 from /v0/runtime/status, got {result}"

        body = result["body"]
        assert body.get("status", {}).get("code") == "OK", f"Unexpected status payload: {body.get('status')}"
        assert body.get("mode") == "MANUAL", f"Expected MANUAL mode, got {body.get('mode')}"

        providers = body.get("providers", [])
        assert providers, f"Expected providers in runtime status, got: {body}"

    def check_devices_endpoint(self) -> None:
        result = self.http_get("/v0/devices")
        assert result["status_code"] == 200, f"Expected 200 from /v0/devices, got {result}"

        devices = result["body"].get("devices", [])
        assert len(devices) >= 2, f"Expected 2+ devices, got {len(devices)}"

        device_ids = [entry.get("device_id") for entry in devices]
        assert "tempctl0" in device_ids and "motorctl0" in device_ids, (
            f"Missing tempctl0 or motorctl0 in discovered devices: {device_ids}"
        )

    def check_capabilities_endpoint(self) -> None:
        result = self.http_get("/v0/devices/sim0/tempctl0/capabilities")
        assert result["status_code"] == 200, f"Expected 200 from capabilities endpoint, got {result}"

        caps = result["body"].get("capabilities", {})
        signals = caps.get("signals", [])
        functions = caps.get("functions", [])

        assert len(signals) >= 4, f"Expected at least 4 signals, got {len(signals)}"
        assert len(functions) >= 1, f"Expected at least 1 function, got {len(functions)}"

        signal_ids = [entry.get("signal_id") for entry in signals]
        assert "tc1_temp" in signal_ids, f"Expected tc1_temp in capabilities, got {signal_ids}"

    def check_state_collection_endpoint(self) -> None:
        def state_has_values_for_all_devices() -> bool:
            result = self.http_get("/v0/state")
            if result["status_code"] != 200:
                return False
            devices = result["body"].get("devices", [])
            if not devices:
                return False
            return all(len(dev.get("values", [])) > 0 for dev in devices)

        assert wait_for_condition(state_has_values_for_all_devices, timeout=5.0, description="state values"), (
            "State values never became available for all devices"
        )

        result = self.http_get("/v0/state")
        assert result["status_code"] == 200, f"Expected 200 from /v0/state, got {result}"

        devices = result["body"].get("devices", [])
        assert len(devices) >= 2, f"Expected at least 2 devices in /v0/state, got {len(devices)}"

        for dev in devices:
            values = dev.get("values", [])
            assert values, f"Expected non-empty values for {dev.get('device_id')}"
            first = values[0]
            assert "signal_id" in first and "value" in first and "quality" in first, (
                f"State value missing required fields: {first}"
            )

        first_quality = devices[0].get("quality")
        assert first_quality in ["OK", "STALE"], f"Unexpected device quality: {first_quality}"

    def check_single_device_state_endpoint(self) -> None:
        result = self.http_get("/v0/state/sim0/motorctl0")
        assert result["status_code"] == 200, f"Expected 200 from single-device state endpoint, got {result}"

        body = result["body"]
        assert body.get("device_id") == "motorctl0", f"Unexpected device id in response: {body.get('device_id')}"

        values = body.get("values", [])
        value_types = {entry.get("value", {}).get("type") for entry in values}
        assert "double" in value_types, f"Expected at least one double typed value, got {value_types}"

    def resolve_function_id(self, device_id: str, function_name: str) -> int:
        """Resolve a function_id by name from the device's capabilities.

        Keeps the suite convention-agnostic: ADPP numbers function_ids per-type
        from 1, but the *name* is the stable identifier, so the test must not pin a
        literal id (see anolis-provider-sim#58).
        """
        result = self.http_get(f"/v0/devices/sim0/{device_id}/capabilities")
        assert result["status_code"] == 200, f"Expected 200 resolving {device_id} capabilities, got {result}"
        functions = result["body"].get("capabilities", {}).get("functions", [])
        for entry in functions:
            if entry.get("name") == function_name:
                return int(entry["function_id"])
        raise AssertionError(f"function '{function_name}' not found in {device_id} capabilities: {functions}")

    def check_call_endpoint(self) -> None:
        call_data = {
            "provider_id": "sim0",
            "device_id": "motorctl0",
            "function_id": self.resolve_function_id("motorctl0", "set_motor_duty"),
            "args": {
                "motor_index": {"type": "int64", "int64": 1},
                "duty": {"type": "double", "double": 0.5},
            },
        }

        result = self.http_post("/v0/call", call_data)
        assert result["status_code"] == 200, f"Expected 200 from /v0/call, got {result}"
        body = result["body"]
        assert body.get("status", {}).get("code") == "OK", f"Call status not OK: {body.get('status')}"

        motor1_duty: Dict[str, float | None] = {"value": None}

        def duty_updated() -> bool:
            state = self.http_get("/v0/state/sim0/motorctl0")
            if state["status_code"] != 200:
                return False
            for val in state["body"].get("values", []):
                if val.get("signal_id") == "motor1_duty":
                    motor1_duty["value"] = val.get("value", {}).get("double")
                    break
            value = motor1_duty["value"]
            if value is None:
                return False
            return abs(value - 0.5) < 0.01

        assert wait_for_condition(duty_updated, timeout=3.0, interval=0.1, description="motor duty update"), (
            f"Expected motor1_duty ~= 0.5, got {motor1_duty['value']}"
        )

    def check_invalid_call_args(self) -> None:
        call_data = {
            "provider_id": "sim0",
            "device_id": "motorctl0",
            "function_id": self.resolve_function_id("motorctl0", "set_motor_duty"),
            "args": {},
        }

        result = self.http_post("/v0/call", call_data)
        assert result["status_code"] != 200, "Expected invalid call args to be rejected"

        status_code = result["body"].get("status", {}).get("code")
        assert status_code in ["INVALID_ARGUMENT", "INTERNAL"], f"Unexpected error code for bad call args: {result}"

    def check_404_handling(self) -> None:
        bad_device = self.http_get("/v0/devices/sim0/nonexistent/capabilities")
        assert bad_device["status_code"] == 404, f"Expected 404 for unknown device, got {bad_device}"

        bad_route = self.http_get("/v0/nonexistent")
        assert bad_route["status_code"] == 404, f"Expected 404 for unknown route, got {bad_route}"

    def check_value_types(self) -> None:
        result = self.http_get("/v0/state/sim0/tempctl0")
        assert result["status_code"] == 200, f"Expected 200 from tempctl state endpoint, got {result}"

        values = result["body"].get("values", [])
        found = {entry.get("value", {}).get("type") for entry in values}
        for expected in ["double", "bool", "string"]:
            assert expected in found, f"Expected value type '{expected}' in tempctl0 state, got {found}"

    def check_run_lifecycle(self) -> None:
        # Initially empty.
        listing = self.http_get("/v0/runs")
        assert listing["status_code"] == 200, f"GET /v0/runs failed: {listing}"
        assert isinstance(listing["body"].get("runs"), list), listing

        # Open a run.
        opened = self.http_post(
            "/v0/runs", {"experiment_label": "it-cycle", "tag_scope": {"provider_ids": ["sim0"]}}
        )
        assert opened["status_code"] == 200, f"open run failed: {opened}"
        run = opened["body"]["run"]
        run_id = run["run_id"]
        assert run["state"] == "open", run
        assert run["experiment_label"] == "it-cycle", run
        assert run["tag_scope"]["provider_ids"] == ["sim0"], run

        # The open run id is reflected on /v0/automation/status (when automation is enabled).
        status = self.http_get("/v0/automation/status")
        if status["status_code"] == 200:
            assert status["body"].get("run_id") == run_id, f"run_id not reflected: {status['body'].get('run_id')}"

        # A second open is rejected (at most one open run per runtime).
        second = self.http_post("/v0/runs", {})
        assert second["status_code"] != 200, f"second open should be rejected, got: {second}"

        # Get by id.
        got = self.http_get(f"/v0/runs/{run_id}")
        assert got["status_code"] == 200 and got["body"]["run"]["run_id"] == run_id, got

        # Close it (idempotently).
        closed = self.http_post(f"/v0/runs/{run_id}/close", {"close_reason": "completed"})
        assert closed["status_code"] == 200, f"close failed: {closed}"
        assert closed["body"]["run"]["state"] == "closed", closed
        assert closed["body"]["run"]["close_reason"] == "completed", closed

        # Listing now includes the closed run; another open is allowed.
        relist = self.http_get("/v0/runs")
        assert any(r["run_id"] == run_id for r in relist["body"]["runs"]), relist
        reopened = self.http_post("/v0/runs", {})
        assert reopened["status_code"] == 200, f"open after close should succeed: {reopened}"
        self.http_post(f"/v0/runs/{reopened['body']['run']['run_id']}/close", {})

    def check_sse_endpoint(self) -> None:
        try:
            resp = requests.get(
                f"{self.base_url}/v0/events?provider_id=sim0&device_id=tempctl0",
                stream=True,
                timeout=2,
                headers={"Accept": "text/event-stream"},
            )
        except requests.exceptions.Timeout:
            resp = None

        if resp is not None:
            content_type = resp.headers.get("Content-Type", "")
            resp.close()
            assert "text/event-stream" in content_type, f"Expected text/event-stream content-type, got '{content_type}'"

        events_received: List[str] = []
        stop_event = threading.Event()

        def collect_events() -> None:
            try:
                with requests.get(
                    f"{self.base_url}/v0/events?provider_id=sim0&device_id=tempctl0",
                    stream=True,
                    timeout=10,
                    headers={"Accept": "text/event-stream"},
                ) as stream_resp:
                    buffer = ""
                    for chunk in stream_resp.iter_content(chunk_size=1, decode_unicode=True):
                        if stop_event.is_set():
                            break
                        if not chunk:
                            continue
                        buffer += chunk
                        while "\n\n" in buffer:
                            event_str, buffer = buffer.split("\n\n", 1)
                            if event_str.strip():
                                events_received.append(event_str)
                                if len(events_received) >= 3:
                                    stop_event.set()
                                    return
            except Exception:
                return

        collector = threading.Thread(target=collect_events, daemon=True)
        collector.start()
        collector.join(timeout=3.0)
        stop_event.set()

        assert len(events_received) >= 1, "Expected at least one SSE event in 3 seconds"

        first_event = events_received[0]
        assert "event: state_update" in first_event, f"Missing event type in SSE payload: {first_event}"
        assert "id: " in first_event, f"Missing event id in SSE payload: {first_event}"
        assert "data: " in first_event, f"Missing data field in SSE payload: {first_event}"

        data_line = [line for line in first_event.split("\n") if line.startswith("data: ")][0]
        data_json = json.loads(data_line[6:])
        for field in ["provider_id", "device_id", "signal_id", "value", "quality", "timestamp_ms"]:
            assert field in data_json, f"Missing '{field}' in SSE data payload: {data_json}"
        assert "type" in data_json.get("value", {}), f"Missing typed value metadata in SSE payload: {data_json}"

        try:
            filtered = requests.get(
                f"{self.base_url}/v0/events?provider_id=sim0&device_id=nonexistent",
                stream=True,
                timeout=1.5,
                headers={"Accept": "text/event-stream"},
            )
            filtered.close()
        except requests.exceptions.Timeout:
            pass


HttpCheck = Tuple[str, Callable[[HttpGatewayTester], None]]

HTTP_CHECKS: List[HttpCheck] = [
    ("runtime_startup_and_status", HttpGatewayTester.check_runtime_startup_and_status),
    ("devices_endpoint", HttpGatewayTester.check_devices_endpoint),
    ("capabilities_endpoint", HttpGatewayTester.check_capabilities_endpoint),
    ("state_collection_endpoint", HttpGatewayTester.check_state_collection_endpoint),
    ("single_device_state_endpoint", HttpGatewayTester.check_single_device_state_endpoint),
    ("call_endpoint", HttpGatewayTester.check_call_endpoint),
    ("invalid_call_args", HttpGatewayTester.check_invalid_call_args),
    ("error_404_handling", HttpGatewayTester.check_404_handling),
    ("value_types", HttpGatewayTester.check_value_types),
    ("run_lifecycle", HttpGatewayTester.check_run_lifecycle),
    ("sse_endpoint", HttpGatewayTester.check_sse_endpoint),
]
