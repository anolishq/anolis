"""Device-loss latch checks against a real runtime and provider-sim (#285, #427, #429).

A latched device whose first good read since a transport loss is the
post-call read of a manual call: the return edge is delivered on the thread
that holds the provider's lock. The safe-state re-assert must not run there,
or the call never returns (#427). These checks drive that path end to end:
provider-sim's inject_device_unreachable makes the device's reads fail with
CODE_UNAVAILABLE, which latches it, and a long poll interval leaves the
manual call's post-call read as the first good one after the fault clears.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Tuple

import requests

from tests.support.api_helpers import assert_http_available
from tests.support.runtime_fixture import RuntimeFixture

DEVICE = "relayio0"
HANDLE = f"sim0/{DEVICE}"
SET_RELAY_CH1 = 1  # relayio function id
INJECT_DEVICE_UNREACHABLE = 6  # chaos_control function ids
CLEAR_FAULTS = 5

# The poll loop reads the device once per interval. The check runs between
# one poll (the failed read that latches the device) and the next, so the
# interval bounds how long everything after the latch may take.
POLL_INTERVAL_S = 10.0
LATCH_TIMEOUT_S = POLL_INTERVAL_S + 5.0
# Well under the 5 s provider timeout and any HTTP client default: a call
# that returns at all returns in milliseconds.
CALL_DEADLINE_S = 3.0
REASSERT_TIMEOUT_S = 3.0
RETURN_LINE = f"[Runtime] {HANDLE} returned while latched"


class DeviceLossTester:
    """A machine whose software safe state opens relay 1 on relayio0."""

    def __init__(self, runtime_path: Path, provider_path: Path, port: int, timeout: float = 30.0):
        self.port = port
        self.base_url = f"http://127.0.0.1:{port}"
        self.timeout = timeout

        fixture_config = Path(__file__).parent / "fixtures" / "provider-sim-default.yaml"
        config: Dict[str, Any] = {
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
            "polling": {"interval_ms": int(POLL_INTERVAL_S * 1000)},
            "telemetry": {"enabled": False},
            "safety": {
                "safe_state": {
                    "hooks": [
                        {
                            "device_handle": HANDLE,
                            "function_name": "set_relay_ch1",
                            "args": {"enabled": False},
                        }
                    ]
                },
                "on_startup": "leave",
            },
            "logging": {"level": "info"},
        }
        self.fixture = RuntimeFixture(Path(runtime_path), Path(provider_path), http_port=port, config_dict=config)

    def start_runtime(self) -> None:
        if not self.fixture.start(
            wait_for_ready=True, provider_id="sim0", min_device_count=1, startup_timeout=min(self.timeout, 30.0)
        ):
            capture = self.fixture.get_output_capture()
            output_tail = capture.get_recent_output(80) if capture else "(no output capture)"
            raise AssertionError(f"Failed to start runtime process\n{output_tail}")
        if not assert_http_available(self.base_url, timeout=min(self.timeout, 30.0)):
            capture = self.fixture.get_output_capture()
            output_tail = capture.get_recent_output(120) if capture else "(no output capture)"
            raise AssertionError(f"Runtime HTTP endpoint did not become available\n{output_tail}")

    def cleanup(self) -> None:
        self.fixture.cleanup()

    def runtime_log(self) -> str:
        capture = self.fixture.get_output_capture()
        assert capture is not None
        return capture.get_all_output()

    def call(self, device_id: str, function_id: int, args: Dict[str, Any], timeout: float) -> Dict[str, Any]:
        resp = requests.post(
            f"{self.base_url}/v0/call",
            json={"provider_id": "sim0", "device_id": device_id, "function_id": function_id, "args": args},
            timeout=timeout,
        )
        assert resp.status_code == 200, resp.text
        body: Dict[str, Any] = resp.json()
        return body

    def latched(self) -> List[str]:
        resp = requests.get(f"{self.base_url}/v0/runtime/status", timeout=5)
        assert resp.status_code == 200, resp.text
        handles: List[str] = resp.json()["device_loss_latched"]
        return handles

    def values(self) -> List[Dict[str, Any]]:
        resp = requests.get(f"{self.base_url}/v0/state/sim0/{DEVICE}", timeout=5)
        assert resp.status_code == 200, resp.text
        values: List[Dict[str, Any]] = resp.json()["values"]
        return values

    def relay_ch1(self) -> Any:
        signal = next((s for s in self.values() if s["signal_id"] == "relay_ch1_state"), None)
        return None if signal is None else bool(signal["value"]["bool"])

    def wait_for_log(self, text: str, timeout: float) -> None:
        # The log reaches us through a reader thread, so a line can trail the
        # state it describes by a few milliseconds.
        deadline = time.monotonic() + timeout
        while text not in self.runtime_log():
            assert time.monotonic() < deadline, f"no {text!r} in the runtime log\n{self.runtime_log()[-3000:]}"
            time.sleep(0.02)


def check_manual_call_to_a_returning_latched_device_returns(tester: DeviceLossTester) -> None:
    """The manual call's post-call read is the return edge; the call returns and safe state is re-asserted once."""
    body = tester.call(
        "chaos_control",
        INJECT_DEVICE_UNREACHABLE,
        {"device_id": {"type": "string", "string": DEVICE}, "duration_ms": {"type": "int64", "int64": 60000}},
        timeout=5,
    )
    assert body["status"]["code"] == "OK", body

    # Precondition: the poll loop's read failed with a transport-level code
    # and latched the device. Without this the rest would pass vacuously.
    deadline = time.monotonic() + LATCH_TIMEOUT_S
    while HANDLE not in tester.latched():
        assert time.monotonic() < deadline, f"{HANDLE} never latched\n{tester.runtime_log()[-3000:]}"
        time.sleep(0.05)
    latched_at = time.monotonic()
    tester.wait_for_log("device unreachable (injected fault)", timeout=2.0)

    body = tester.call("chaos_control", CLEAR_FAULTS, {}, timeout=5)
    assert body["status"]["code"] == "OK", body

    # Ordering: still latched, and no read has returned the device yet. The
    # failed read cleared the device's cached values and only a good read
    # refills them, so empty values mean the manual call's post-call read is
    # the device's first good read. The next poll is a full interval after
    # the one that latched the device.
    assert HANDLE in tester.latched()
    assert tester.values() == [], "a read returned the device before the manual call"
    assert RETURN_LINE not in tester.runtime_log(), "a return edge was logged before the manual call"

    started = time.monotonic()
    try:
        body = tester.call(DEVICE, SET_RELAY_CH1, {"enabled": {"type": "bool", "bool": True}}, timeout=CALL_DEADLINE_S)
    except requests.exceptions.Timeout as err:
        raise AssertionError(
            f"the manual call to the returning latched device did not return within {CALL_DEADLINE_S} s (#427)"
        ) from err
    elapsed = time.monotonic() - started
    assert body["status"]["code"] == "OK", body
    print(f"  manual call returned in {elapsed:.3f} s")

    # Exactly one re-assert, opening relay 1 again after the manual call
    # closed it.
    deadline = time.monotonic() + REASSERT_TIMEOUT_S
    while tester.runtime_log().count(RETURN_LINE) < 1 or tester.relay_ch1() is not False:
        assert time.monotonic() < deadline, f"safe state was not re-asserted\n{tester.runtime_log()[-3000:]}"
        time.sleep(0.05)
    assert tester.runtime_log().count(RETURN_LINE) == 1, tester.runtime_log()[-3000:]
    assert f"{RETURN_LINE}; re-asserted 1 hooks safe-state call(s)" in tester.runtime_log()

    # All of it happened before the next poll could have read the device.
    since_latch = time.monotonic() - latched_at
    assert since_latch < POLL_INTERVAL_S - 2.0, f"took {since_latch:.1f} s; a poll may have read the device"
    assert HANDLE in tester.latched(), "a return does not re-arm; only MANUAL -> AUTO does"
    print(f"  re-asserted once, {since_latch:.2f} s after the latch")


DeviceLossCheck = Tuple[str, Callable[[DeviceLossTester], None]]

DEVICE_LOSS_CHECKS: List[DeviceLossCheck] = [
    ("manual_call_to_a_returning_latched_device_returns", check_manual_call_to_a_returning_latched_device_returns),
]
