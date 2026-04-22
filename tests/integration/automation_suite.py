"""Automation integration checks (pytest-oriented)."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable, Dict, List, Tuple, cast

import requests

from tests.support.api_helpers import assert_http_available, wait_for_condition
from tests.support.runtime_fixture import RuntimeFixture


class AutomationTester:
    """Automation API check harness."""

    def __init__(
        self,
        runtime_path: Path | str,
        provider_path: Path | str,
        port: int,
        timeout: float = 30.0,
        automation_enabled: bool = True,
        manual_gating_policy: str = "BLOCK",
    ):
        self.port = port
        self.base_url = f"http://127.0.0.1:{port}"
        self.timeout = timeout
        self.repo_root = Path(__file__).resolve().parents[2]

        fixture_config = Path(__file__).parent / "fixtures" / "provider-sim-default.yaml"
        config = {
            "runtime": {},
            "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
            "providers": [
                {
                    "id": "sim0",
                    "command": str(provider_path).replace("\\", "/"),
                    "args": ["--config", str(fixture_config).replace("\\", "/")],
                    "timeout_ms": 5000,
                }
            ],
            "polling": {"interval_ms": 500},
            "telemetry": {"enabled": False},
            "automation": {
                "enabled": automation_enabled,
                "behavior_tree": str(self.repo_root / "tests" / "integration" / "fixtures" / "behaviors" / "demo.xml"),
                "tick_rate_hz": 10,
                "manual_gating_policy": manual_gating_policy,
                "parameters": [
                    {
                        "name": "temp_setpoint",
                        "type": "double",
                        "default": 25.0,
                        "min": 10.0,
                        "max": 50.0,
                    },
                    {
                        "name": "motor_duty_cycle",
                        "type": "int64",
                        "default": 50,
                        "min": 0,
                        "max": 100,
                    },
                    {"name": "control_enabled", "type": "bool", "default": True},
                    {
                        "name": "operating_mode",
                        "type": "string",
                        "default": "normal",
                        "allowed_values": ["normal", "test", "emergency"],
                    },
                ],
            },
            "logging": {"level": "info"},
        }

        self.fixture = RuntimeFixture(
            Path(runtime_path),
            Path(provider_path),
            http_port=port,
            config_dict=config,
        )

    def start_runtime(self) -> None:
        if not self.fixture.start(
            wait_for_ready=True,
            provider_id="sim0",
            min_device_count=1,
            startup_timeout=min(self.timeout, 30.0),
        ):
            capture = self.fixture.get_output_capture()
            output_tail = capture.get_recent_output(80) if capture else "(no output capture)"
            raise AssertionError(f"Failed to start runtime process\n{output_tail}")

        if not assert_http_available(self.base_url, timeout=min(self.timeout, 30.0)):
            capture = self.fixture.get_output_capture()
            output_tail = capture.get_recent_output(120) if capture else "(no output capture)"
            if not self.fixture.is_running():
                raise AssertionError(f"Runtime process terminated\n{output_tail}")
            raise AssertionError(f"Runtime HTTP endpoint did not become available\n{output_tail}")

    def cleanup(self) -> None:
        self.fixture.cleanup()

    def get_mode(self) -> Dict[str, Any]:
        try:
            resp = requests.get(f"{self.base_url}/v0/mode", timeout=2)
        except requests.RequestException as err:
            raise AssertionError(f"GET /v0/mode failed: {err}") from err
        return cast(Dict[str, Any], resp.json())

    def set_mode(self, mode: str) -> Dict[str, Any]:
        try:
            resp = requests.post(f"{self.base_url}/v0/mode", json={"mode": mode}, timeout=2)
        except requests.RequestException as err:
            raise AssertionError(f"POST /v0/mode failed for mode={mode}: {err}") from err
        return cast(Dict[str, Any], resp.json())

    def _wait_for_mode(self, expected_mode: str, timeout: float = 3.0) -> None:
        ok = wait_for_condition(
            lambda: self.get_mode().get("mode") == expected_mode,
            timeout=timeout,
            description=f"mode {expected_mode}",
        )
        assert ok, f"Expected mode {expected_mode}, got {self.get_mode()}"

    def test_get_mode_when_automation_enabled(self) -> None:
        result = self.get_mode()
        assert "mode" in result, f"Response missing mode: {result}"
        assert result["mode"] == "IDLE", f"Expected IDLE safe default, got {result['mode']}"

    def test_mode_transition_manual_to_auto(self) -> None:
        result = self.set_mode("MANUAL")
        assert result.get("mode") == "MANUAL", f"Failed to transition to MANUAL: {result}"
        self._wait_for_mode("MANUAL")

        result = self.set_mode("AUTO")
        assert result.get("mode") == "AUTO", f"Expected AUTO, got {result}"

    def test_mode_transition_auto_to_manual(self) -> None:
        self.set_mode("MANUAL")
        self._wait_for_mode("MANUAL")

        self.set_mode("AUTO")
        self._wait_for_mode("AUTO")

        result = self.set_mode("MANUAL")
        assert result.get("mode") == "MANUAL", f"Expected MANUAL, got {result}"

    def test_mode_transition_manual_to_idle(self) -> None:
        self.set_mode("MANUAL")
        self._wait_for_mode("MANUAL")

        result = self.set_mode("IDLE")
        assert result.get("mode") == "IDLE", f"Expected IDLE, got {result}"

    def test_mode_transition_to_fault(self) -> None:
        self.set_mode("MANUAL")
        self._wait_for_mode("MANUAL")

        result = self.set_mode("FAULT")
        assert result.get("mode") == "FAULT", f"Expected FAULT, got {result}"

    def test_invalid_transition_fault_to_auto(self) -> None:
        self.set_mode("FAULT")
        self._wait_for_mode("FAULT")

        result = self.set_mode("AUTO")
        status = cast(Dict[str, Any], result.get("status", {}))
        assert status.get("code") == "FAILED_PRECONDITION", (
            f"Expected FAILED_PRECONDITION for FAULT->AUTO, got {result}"
        )

    def test_recovery_path_fault_to_manual_to_auto(self) -> None:
        self.set_mode("FAULT")
        self._wait_for_mode("FAULT")

        result = self.set_mode("MANUAL")
        assert result.get("mode") == "MANUAL", f"Failed to recover to MANUAL: {result}"

        result = self.set_mode("AUTO")
        assert result.get("mode") == "AUTO", f"Failed MANUAL->AUTO after recovery: {result}"

    def test_invalid_mode_string(self) -> None:
        result = self.set_mode("INVALID")
        status = cast(Dict[str, Any], result.get("status", {}))
        assert status.get("code") == "INVALID_ARGUMENT", f"Expected INVALID_ARGUMENT, got {result}"

    def test_get_parameters(self) -> None:
        try:
            resp = requests.get(f"{self.base_url}/v0/parameters", timeout=2)
        except requests.RequestException as err:
            raise AssertionError(f"GET /v0/parameters failed: {err}") from err

        assert resp.status_code == 200, f"Expected 200 from /v0/parameters, got {resp.status_code}: {resp.text}"

        body = cast(Dict[str, Any], resp.json())
        assert "parameters" in body, f"Response missing 'parameters': {body}"

        names = [entry.get("name") for entry in cast(List[Dict[str, Any]], body.get("parameters", []))]
        assert "temp_setpoint" in names, f"Expected temp_setpoint in parameters, got {names}"

    def test_update_parameter_valid(self) -> None:
        try:
            resp = requests.post(
                f"{self.base_url}/v0/parameters",
                json={"name": "temp_setpoint", "value": 30.0},
                timeout=2,
            )
        except requests.RequestException as err:
            raise AssertionError(f"POST /v0/parameters failed: {err}") from err

        assert resp.status_code == 200, f"Expected 200 from valid parameter update, got {resp.status_code}: {resp.text}"

        body = cast(Dict[str, Any], resp.json())
        value = body.get("parameter", {}).get("value")
        assert value == 30.0, f"Expected updated value 30.0, got {value}"

    def test_update_parameter_out_of_range(self) -> None:
        try:
            resp = requests.post(
                f"{self.base_url}/v0/parameters",
                json={"name": "temp_setpoint", "value": 100.0},
                timeout=2,
            )
        except requests.RequestException as err:
            raise AssertionError(f"POST /v0/parameters failed: {err}") from err

        assert resp.status_code != 200, "Expected out-of-range parameter update to be rejected"

        body = cast(Dict[str, Any], resp.json())
        status = cast(Dict[str, Any], body.get("status", {}))
        assert status.get("code") == "INVALID_ARGUMENT", f"Expected INVALID_ARGUMENT, got {body}"

    def test_idle_blocks_control_operations(self) -> None:
        self.set_mode("IDLE")
        self._wait_for_mode("IDLE")

        try:
            resp = requests.post(
                f"{self.base_url}/v0/call",
                json={
                    "provider_id": "sim0",
                    "device_id": "tempctl0",
                    "function_id": 3,
                    "args": {
                        "relay_index": {"type": "int64", "int64": 1},
                        "state": {"type": "bool", "bool": True},
                    },
                },
                timeout=5,
            )
        except requests.RequestException as err:
            raise AssertionError(f"Control operation request failed in IDLE: {err}") from err

        body = cast(Dict[str, Any], resp.json())
        status = cast(Dict[str, Any], body.get("status", {}))
        assert status.get("code") == "FAILED_PRECONDITION", (
            f"Expected FAILED_PRECONDITION for control call in IDLE mode, got: {body}"
        )

    def test_idle_allows_read_operations(self) -> None:
        self.set_mode("IDLE")
        self._wait_for_mode("IDLE")

        status_resp = requests.get(f"{self.base_url}/v0/runtime/status", timeout=5)
        assert status_resp.status_code == 200, f"Runtime status query failed in IDLE: {status_resp.status_code}"

        devices_resp = requests.get(f"{self.base_url}/v0/devices", timeout=5)
        assert devices_resp.status_code == 200, f"Device list query failed in IDLE: {devices_resp.status_code}"

        state_resp = requests.get(f"{self.base_url}/v0/state/sim0/tempctl0", timeout=5)
        assert state_resp.status_code == 200, f"State query failed in IDLE: {state_resp.status_code}"

        state_body = cast(Dict[str, Any], state_resp.json())
        assert "values" in state_body, f"State query missing values in IDLE: {state_body}"


AutomationCheck = Tuple[str, Callable[[AutomationTester], None]]

AUTOMATION_CHECKS: List[AutomationCheck] = [
    ("get_mode_when_automation_enabled", AutomationTester.test_get_mode_when_automation_enabled),
    ("mode_transition_manual_to_auto", AutomationTester.test_mode_transition_manual_to_auto),
    ("mode_transition_auto_to_manual", AutomationTester.test_mode_transition_auto_to_manual),
    ("mode_transition_manual_to_idle", AutomationTester.test_mode_transition_manual_to_idle),
    ("idle_blocks_control_operations", AutomationTester.test_idle_blocks_control_operations),
    ("idle_allows_read_operations", AutomationTester.test_idle_allows_read_operations),
    ("mode_transition_to_fault", AutomationTester.test_mode_transition_to_fault),
    ("invalid_transition_fault_to_auto", AutomationTester.test_invalid_transition_fault_to_auto),
    ("recovery_path_fault_to_manual_to_auto", AutomationTester.test_recovery_path_fault_to_manual_to_auto),
    ("invalid_mode_string", AutomationTester.test_invalid_mode_string),
    ("get_parameters", AutomationTester.test_get_parameters),
    ("update_parameter_valid", AutomationTester.test_update_parameter_valid),
    ("update_parameter_out_of_range", AutomationTester.test_update_parameter_out_of_range),
]


def assert_automation_disabled_returns_unavailable(
    runtime_path: Path,
    provider_path: Path,
    port: int,
    timeout: float = 30.0,
) -> None:
    """Validate mode APIs surface UNAVAILABLE when automation is disabled."""
    tester = AutomationTester(
        runtime_path,
        provider_path,
        port=port,
        timeout=timeout,
        automation_enabled=False,
    )
    try:
        tester.start_runtime()
        result = tester.get_mode()
        status = cast(Dict[str, Any], result.get("status", {}))
        assert status.get("code") == "UNAVAILABLE", f"Expected UNAVAILABLE when automation disabled, got {result}"
    finally:
        tester.cleanup()
