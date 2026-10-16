"""End-to-end software safe-state (POST /v0/estop) integration checks."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import requests

from tests.integration.automation_suite import AutomationTester
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
        assert isinstance(engaged["body"]["safe_state_achieved"], bool), engaged

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


class StartupSafeStateTester(EstopTester):
    """A runtime whose safe state differs from what the sim powers on with (#362).

    The sim's relays power on open, so a safe state that only zeroed outputs
    would leave it exactly as it booted. Closing relay 1 makes the ladder's
    effect visible through GET /v0/state. `hook_function` None declares no
    safe state; a function the sim does not have makes the run fail.
    """

    def __init__(
        self,
        runtime_path: Path,
        provider_path: Path,
        port: int,
        on_startup: Optional[str],
        hook_function: Optional[str] = "set_relay_ch1",
    ):
        super().__init__(runtime_path, provider_path, port=port)
        safety: Dict[str, Any] = {}
        if hook_function is not None:
            safety["safe_state"] = {
                "hooks": [
                    {
                        "device_handle": "sim0/relayio0",
                        "function_name": hook_function,
                        "args": {"enabled": True},
                    }
                ]
            }
        if on_startup is not None:
            safety["on_startup"] = on_startup
        config = self.fixture.config_dict
        assert config is not None
        config["safety"] = safety

    def relay_ch1_closed(self) -> bool:
        state = self.http_get("/v0/state/sim0/relayio0")
        assert state["status_code"] == 200, state
        signal = next(s for s in state["body"]["values"] if s["signal_id"] == "relay_ch1_state")
        return bool(signal["value"]["bool"])

    def log_index(self, marker: str) -> int:
        capture = self.fixture.get_output_capture()
        assert capture is not None
        lines = capture.get_all_output().splitlines()
        hits = [i for i, line in enumerate(lines) if marker in line]
        assert hits, f"'{marker}' not in the runtime log"
        return hits[0]

    def startup_status(self) -> Dict[str, Any]:
        status = self.http_get("/v0/runtime/status")
        assert status["status_code"] == 200, status
        startup: Dict[str, Any] = status["body"]["startup_safe_state"]
        return startup


def check_startup_runs_safe_state(tester: StartupSafeStateTester) -> None:
    """With no safety.on_startup, the safe state runs once at startup."""
    deadline = time.time() + 3.0
    while not tester.relay_ch1_closed():
        assert time.time() < deadline, "safe state did not run at startup (relay 1 still open)"
        time.sleep(0.1)
    # Ran without engaging the e-stop latch.
    status = tester.http_get("/v0/runtime/status")["body"]
    assert status["estop"]["latched"] is False, status
    assert tester.startup_status() == {
        "on_startup": "safe_state",
        "ran": True,
        "software_safe_state": "hooks",
        "safe_state_achieved": True,
        "failed_actions": [],
    }


def check_startup_safe_state_failed(tester: StartupSafeStateTester) -> None:
    """A startup run that fails is logged at ERROR and reported; the runtime still starts."""
    startup = tester.startup_status()
    assert startup["ran"] is True, startup
    assert startup["software_safe_state"] == "hooks", startup
    assert startup["safe_state_achieved"] is False, startup
    assert len(startup["failed_actions"]) == 1, startup
    failed = startup["failed_actions"][0]
    assert failed["device_handle"] == "sim0/relayio0" and failed["function"] == "no_such_function", failed
    assert failed["success"] is False and failed.get("error"), failed
    assert tester.http_get("/v0/runtime/status")["body"]["estop"]["latched"] is False
    tester.log_index("[ERROR] [Runtime] safety.on_startup=safe_state did not drive sim0/relayio0/no_such_function")
    tester.log_index("[ERROR] [Runtime] safety.on_startup=safe_state: ladder=hooks actions=1 safe_state_achieved=false")


def check_startup_no_safe_state(tester: StartupSafeStateTester) -> None:
    """With no safe state declared there is nothing to run, and the status says it did not run."""
    assert tester.startup_status() == {
        "on_startup": "safe_state",
        "ran": False,
        "software_safe_state": "none",
        "safe_state_achieved": False,
        "failed_actions": [],
    }
    tester.log_index("no software safe state is declared, nothing to run")


def check_startup_leave(tester: StartupSafeStateTester) -> None:
    """With safety.on_startup: leave, outputs stay as they were."""
    time.sleep(1.5)  # three poll intervals
    assert tester.relay_ch1_closed() is False, "on_startup: leave still ran the safe state"
    tester.log_index("outputs were left as they were")
    assert tester.startup_status() == {
        "on_startup": "leave",
        "ran": False,
        "software_safe_state": "none",
        "safe_state_achieved": False,
        "failed_actions": [],
    }


def check_startup_safe_state_order(tester: StartupSafeStateTester) -> None:
    """The startup safe state runs after provider discovery and before HTTP opens.

    Read from the log: the state cache can lag the call, so the API cannot
    tell "before HTTP" from "just after".
    """
    discovered = tester.log_index("[Runtime] All providers started")
    ran = tester.log_index("[Runtime] safety.on_startup=safe_state")
    http_open = tester.log_index("[Runtime] HTTP server started")
    assert discovered < ran < http_open, (discovered, ran, http_open)


# (id, safety.on_startup, hook function or None for no safe state, check)
StartupCheck = Tuple[str, Optional[str], Optional[str], Callable[[StartupSafeStateTester], None]]

STARTUP_CHECKS: List[StartupCheck] = [
    ("default_runs_safe_state", None, "set_relay_ch1", check_startup_runs_safe_state),
    ("failed", None, "no_such_function", check_startup_safe_state_failed),
    ("no_safe_state", None, None, check_startup_no_safe_state),
    ("leave", "leave", "set_relay_ch1", check_startup_leave),
    ("order", None, "set_relay_ch1", check_startup_safe_state_order),
]


class FaultHookEstopTester(AutomationTester):
    """An automation machine whose only `-> FAULT` hook closes relay 1 (#251).

    The sim's relays power on open, so the hook's effect shows in GET
    /v0/state. `safe_state` None declares no safety.safe_state, so the
    ladder plans rung none. A `hook_function` the sim does not have makes
    the hook fail. `hook_site` is the mode_transition_hooks group the hook
    is declared in.
    """

    def __init__(
        self,
        runtime_path: Path,
        provider_path: Path,
        port: int,
        safe_state: Optional[Dict[str, Any]] = None,
        hook_function: str = "set_relay_ch1",
        hook_site: str = "before_transition",
    ):
        super().__init__(runtime_path, provider_path, port=port)
        config = self.fixture.config_dict
        assert config is not None
        config["automation"]["mode_transition_hooks"] = {
            hook_site: [
                {
                    "from": "*",
                    "to": "FAULT",
                    "fail_on_error": False,
                    "calls": [
                        {
                            "device_handle": "sim0/relayio0",
                            "function_name": hook_function,
                            "args": {"enabled": True},
                        }
                    ],
                }
            ]
        }
        if safe_state is not None:
            config["safety"] = {"safe_state": safe_state, "on_startup": "leave"}

    def relay_ch1_closed(self) -> bool:
        resp = requests.get(f"{self.base_url}/v0/state/sim0/relayio0", timeout=5)
        assert resp.status_code == 200, resp.text
        signal = next(s for s in resp.json()["values"] if s["signal_id"] == "relay_ch1_state")
        return bool(signal["value"]["bool"])

    def runtime_log(self) -> str:
        capture = self.fixture.get_output_capture()
        assert capture is not None
        return capture.get_all_output()

    def estop(self) -> Dict[str, Any]:
        resp = requests.post(f"{self.base_url}/v0/estop", json={}, timeout=10)
        assert resp.status_code == 200, resp.text
        body: Dict[str, Any] = resp.json()
        return body


def check_estop_runs_fault_hooks_without_a_rung(tester: FaultHookEstopTester) -> None:
    """Rung none: the e-stop's FAULT entry runs the -> FAULT hook."""
    tester.set_mode("MANUAL")
    tester._wait_for_mode("MANUAL")
    assert tester.relay_ch1_closed() is False

    body = tester.estop()
    assert body["software_safe_state"] == "none", body
    assert body["actions"] == [], body
    assert body["safe_state_achieved"] is False, body
    assert body["latched"] is True, body
    assert body["fault_engaged"] is True, body
    tester._wait_for_mode("FAULT")

    deadline = time.time() + 3.0
    while not tester.relay_ch1_closed():
        assert time.time() < deadline, "the -> FAULT hook did not run on the e-stop (relay 1 still open)"
        time.sleep(0.1)
    assert requests.get(f"{tester.base_url}/v0/runtime/status", timeout=5).json()["estop"]["latched"] is True
    assert "Before-transition hook (MANUAL -> FAULT): sim0/relayio0 set_relay_ch1 OK" in tester.runtime_log()


def check_estop_runs_after_transition_fault_hooks_without_a_rung(tester: FaultHookEstopTester) -> None:
    """Rung none: an after_transition * -> FAULT hook runs on the e-stop too."""
    tester.set_mode("MANUAL")
    tester._wait_for_mode("MANUAL")

    body = tester.estop()
    assert body["software_safe_state"] == "none", body
    assert body["safe_state_achieved"] is False, body
    tester._wait_for_mode("FAULT")

    deadline = time.time() + 3.0
    while not tester.relay_ch1_closed():
        assert time.time() < deadline, "the after_transition -> FAULT hook did not run on the e-stop"
        time.sleep(0.1)
    assert "After-transition hook (MANUAL -> FAULT): sim0/relayio0 set_relay_ch1 OK" in tester.runtime_log()


def check_estop_refuses_fault_hooks_under_a_rung(tester: FaultHookEstopTester) -> None:
    """Any other rung: the ladder stays the only authority, the hook is refused."""
    tester.set_mode("MANUAL")
    tester._wait_for_mode("MANUAL")

    body = tester.estop()
    assert body["software_safe_state"] == "hooks", body
    assert body["safe_state_achieved"] is True, body
    tester._wait_for_mode("FAULT")

    time.sleep(1.5)  # three poll intervals
    assert tester.relay_ch1_closed() is False, "the -> FAULT hook ran through the latch under rung hooks"
    log = tester.runtime_log()
    assert "Before-transition hook failed (MANUAL -> FAULT): sim0/relayio0 set_relay_ch1" in log, log[-2000:]
    assert "Actuation latched by e-stop" in log


def check_estop_reports_fault_hook_not_driven(tester: FaultHookEstopTester) -> None:
    """Rung none: a -> FAULT hook that fails is logged like a ladder action not driven."""
    tester.set_mode("MANUAL")
    tester._wait_for_mode("MANUAL")

    body = tester.estop()
    assert body["software_safe_state"] == "none", body
    assert body["safe_state_achieved"] is False, body
    log = tester.runtime_log()
    assert "[WARN]  [SafeState] E-stop did not drive sim0/relayio0/no_such_function: " in log, log[-2000:]


def check_ordinary_fault_entry_runs_fault_hooks(tester: FaultHookEstopTester) -> None:
    """No e-stop: entering FAULT runs the hook, as it always has."""
    tester.set_mode("MANUAL")
    tester._wait_for_mode("MANUAL")
    tester.set_mode("FAULT")
    tester._wait_for_mode("FAULT")

    deadline = time.time() + 3.0
    while not tester.relay_ch1_closed():
        assert time.time() < deadline, "the -> FAULT hook did not run on an ordinary FAULT entry"
        time.sleep(0.1)


# A safety.safe_state whose hook drives a different device than the -> FAULT
# hook, so what each one did stays distinguishable.
_MOTOR_SAFE_STATE: Dict[str, Any] = {
    "hooks": [
        {
            "device_handle": "sim0/motorctl0",
            "function_name": "set_motor_duty",
            "args": {"motor_index": 1, "duty": 0.0},
        }
    ]
}

# (id, FaultHookEstopTester keyword arguments, check)
FaultHookCheck = Tuple[str, Dict[str, Any], Callable[[FaultHookEstopTester], None]]

FAULT_HOOK_CHECKS: List[FaultHookCheck] = [
    ("estop_runs_fault_hooks_without_a_rung", {}, check_estop_runs_fault_hooks_without_a_rung),
    (
        "estop_runs_after_transition_fault_hooks_without_a_rung",
        {"hook_site": "after_transition"},
        check_estop_runs_after_transition_fault_hooks_without_a_rung,
    ),
    (
        "estop_reports_fault_hook_not_driven",
        {"hook_function": "no_such_function"},
        check_estop_reports_fault_hook_not_driven,
    ),
    (
        "estop_refuses_fault_hooks_under_a_rung",
        {"safe_state": _MOTOR_SAFE_STATE},
        check_estop_refuses_fault_hooks_under_a_rung,
    ),
    ("ordinary_fault_entry_runs_fault_hooks", {}, check_ordinary_fault_entry_runs_fault_hooks),
]


EstopCheck = Tuple[str, Callable[[EstopTester], None]]

ESTOP_CHECKS: List[EstopCheck] = [
    ("estop_latch_cycle", EstopTester.check_estop_latch_cycle),
]
