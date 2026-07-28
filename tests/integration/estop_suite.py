"""End-to-end software safe-state (POST /v0/estop) integration checks."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import requests

from tests.support.runtime_fixture import RuntimeFixture


class EstopTester:
    """Drives a live runtime + sim provider through the e-stop latch cycle."""

    def __init__(self, runtime_path: Path, provider_path: Path, port: int = 8080, timeout: float = 30.0):
        self.port = port
        self.base_url = f"http://127.0.0.1:{port}"
        self.timeout = timeout

        fixture_config = Path(__file__).parent / "fixtures" / "provider-sim-default.yaml"
        fixture_config_str = str(fixture_config).replace("\\", "/")
        config = {
            "runtime": {},
            "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
            "providers": [
                {
                    "id": "sim0",
                    "command": str(provider_path),
                    "args": ["--config", fixture_config_str],
                    "timeout_ms": 5000,
                }
            ],
            "polling": {"interval_ms": 500},
            "telemetry": {"enabled": False},
            "logging": {"level": "info"},
            # zero_is_safe makes the ladder choose the "zero" rung so the check
            # also exercises the safe-state actions, not just the latch.
            "safety": {"safe_state": {"zero_is_safe": True}},
        }
        self.fixture = RuntimeFixture(runtime_path, provider_path, http_port=port, config_dict=config)

    def cleanup(self) -> None:
        self.fixture.cleanup()

    def start_runtime(self) -> None:
        if not self.fixture.start(
            wait_for_ready=True,
            provider_id="sim0",
            min_device_count=1,
            startup_timeout=min(self.timeout, 30.0),
        ):
            raise AssertionError("Runtime failed to start")

    def http_get(self, path: str, timeout: float = 5.0) -> Dict[str, Any]:
        resp = requests.get(f"{self.base_url}{path}", timeout=timeout)
        try:
            body: Dict[str, Any] = resp.json()
        except ValueError:
            body = {"raw": resp.text}
        return {"status_code": resp.status_code, "body": body}

    def http_post(self, path: str, data: Optional[Dict[str, Any]] = None, timeout: float = 5.0) -> Dict[str, Any]:
        resp = requests.post(f"{self.base_url}{path}", json=data if data is not None else {}, timeout=timeout)
        try:
            body: Dict[str, Any] = resp.json()
        except ValueError:
            body = {"raw": resp.text}
        return {"status_code": resp.status_code, "body": body}

    def resolve_function_id(self, device_id: str, function_name: str) -> int:
        result = self.http_get(f"/v0/devices/sim0/{device_id}/capabilities")
        assert result["status_code"] == 200, result
        for entry in result["body"].get("capabilities", {}).get("functions", []):
            if entry.get("name") == function_name:
                return int(entry["function_id"])
        raise AssertionError(f"function '{function_name}' not found on {device_id}")

    def _actuating_call(self) -> Dict[str, Any]:
        return {
            "provider_id": "sim0",
            "device_id": "motorctl0",
            "function_id": self.resolve_function_id("motorctl0", "set_motor_duty"),
            "args": {
                "motor_index": {"type": "int64", "int64": 1},
                "duty": {"type": "double", "double": 0.5},
            },
        }

    def check_estop_latch_cycle(self) -> None:
        # Baseline: not latched; the ladder would run the "zero" rung.
        status = self.http_get("/v0/runtime/status")
        assert status["status_code"] == 200, status
        estop = status["body"]["estop"]
        assert estop["latched"] is False, estop
        assert estop["software_safe_state"] == "zero", estop

        # Engage the software e-stop.
        engaged = self.http_post("/v0/estop")
        assert engaged["status_code"] == 200, engaged
        assert engaged["body"]["latched"] is True, engaged
        assert engaged["body"]["software_safe_state"] == "zero", engaged

        # Status reflects the latch.
        assert self.http_get("/v0/runtime/status")["body"]["estop"]["latched"] is True

        # An actuating call is refused while latched.
        blocked = self.http_post("/v0/call", self._actuating_call())
        assert blocked["status_code"] == 409, blocked

        # Clear the latch (does not change mode).
        cleared = self.http_post("/v0/estop/clear")
        assert cleared["status_code"] == 200, cleared
        assert cleared["body"]["latched"] is False, cleared

        # The same actuating call now succeeds.
        allowed = self.http_post("/v0/call", self._actuating_call())
        assert allowed["status_code"] == 200, allowed


EstopCheck = Tuple[str, Callable[[EstopTester], None]]

ESTOP_CHECKS: List[EstopCheck] = [
    ("estop_latch_cycle", EstopTester.check_estop_latch_cycle),
]
