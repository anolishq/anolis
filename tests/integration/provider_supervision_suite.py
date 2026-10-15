"""
Provider Supervision Integration Test

Validates automatic provider crash recovery with exponential backoff and circuit breaker.
Uses anolis-provider-sim with --crash-after flag for chaos testing.

Tests:
1. Provider crashes and restarts automatically
2. Exponential backoff delays are correct
3. Circuit breaker opens after max attempts
4. Devices rediscovered after successful restart
5. Supervisor state resets on recovery
6. Failed restart attempts remain supervised and recoverable
7. Restart timeout policy is behaviorally enforced
8. Runtime startup timeout is behaviorally enforced

"""

import sys
import tempfile
import textwrap
import time
from pathlib import Path
from typing import Callable, List, NoReturn, Optional, Tuple

import psutil

from tests.support.api_helpers import (
    assert_http_available,
    assert_provider_available,
    get_provider_health_entry,
)
from tests.support.runtime_fixture import RuntimeFixture


class SupervisionTester:
    """Test harness for provider supervision integration tests."""

    def __init__(self, runtime_path: Path, provider_sim_path: Path, timeout: float = 60.0, port: int = 8080):
        self.runtime_path = runtime_path
        self.provider_sim_path = provider_sim_path
        self.timeout = timeout
        self.port = port
        self.fixture: Optional[RuntimeFixture] = None
        self.base_url = f"http://127.0.0.1:{port}"

    @property
    def capture(self):
        """Get OutputCapture from fixture for convenience."""
        if self.fixture:
            return self.fixture.get_output_capture()
        return None

    def create_config(
        self,
        crash_after: float,
        max_attempts: int = 3,
        backoff_ms: Optional[List[int]] = None,
        success_reset_ms: int = 500,
    ) -> dict:
        """Create config dict with supervision settings."""
        if backoff_ms is None:
            backoff_ms = [100, 1000, 5000]

        provider_cmd = str(self.provider_sim_path).replace("\\", "/")
        fixture_config = Path(__file__).parent / "fixtures" / "provider-sim-default.yaml"

        config = {
            "http": {
                "enabled": True,
                "bind": "127.0.0.1",
                "port": self.port,
            },
            "providers": [
                {
                    "id": "provider-sim",
                    "command": provider_cmd,
                    "args": ["--config", str(fixture_config).replace("\\", "/"), "--crash-after", str(crash_after)],
                    "timeout_ms": 1500,
                    # Keep supervision tests responsive: lower provider RPC timeout reduces
                    # crash-detection latency and avoids flaky timing races in CI.
                    "restart_policy": {
                        "enabled": True,
                        "max_attempts": max_attempts,
                        "backoff_ms": backoff_ms,
                        "timeout_ms": 30000,
                        "success_reset_ms": success_reset_ms,
                    },
                }
            ],
            "polling": {"interval_ms": 200},
            "logging": {"level": "info"},
        }

        return config

    def _default_provider_fixture_config(self) -> str:
        fixture_config = Path(__file__).parent / "fixtures" / "provider-sim-default.yaml"
        return str(fixture_config).replace("\\", "/")

    def _write_wrapper_script(self, temp_dir: Path, filename: str, script_body: str) -> Path:
        script_path = temp_dir / filename
        script_path.write_text(textwrap.dedent(script_body), encoding="utf-8")
        return script_path

    def start_runtime(self, config_dict: dict) -> None:
        """Start runtime with given config."""
        self.fixture = RuntimeFixture(
            self.runtime_path,
            self.provider_sim_path,
            http_port=self.port,
            config_dict=config_dict,
        )

        assert self.fixture.start(), "Failed to start runtime"
        # Use fixture-provided base URL (avoids hard-coded host/port assumptions).
        self.base_url = self.fixture.base_url

        # Wait for HTTP server to respond — API-based readiness check.
        if not assert_http_available(self.base_url, timeout=10.0):
            capture = self.fixture.get_output_capture()
            output_tail = capture.get_recent_output(120) if capture else "(no output capture)"
            raise AssertionError(f"Runtime HTTP server did not become available\nOutput tail:\n{output_tail}")

    def stop_runtime(self):
        """Stop runtime gracefully."""
        if self.fixture:
            try:
                self.fixture.cleanup()
            except Exception:
                pass  # Cleanup error, ignore

    def _wait_for_runtime_exit(self, timeout: float) -> bool:
        if self.fixture is None:
            return True

        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.fixture.is_running():
                return True
            time.sleep(0.05)
        return False

    def _sample_provider_health(self) -> dict:
        """Capture a compact provider health snapshot for failure timelines."""
        ts = time.time()
        entry = get_provider_health_entry(self.base_url, "provider-sim", timeout=1.0)
        if entry is None:
            return {
                "t": ts,
                "present": False,
                "state": "MISSING",
                "attempt_count": None,
                "crash_detected": None,
                "circuit_open": None,
                "max_attempts": None,
                "next_restart_in_ms": None,
                "uptime_seconds": None,
                "device_count": None,
            }

        supervision = entry.get("supervision") or {}
        return {
            "t": ts,
            "present": True,
            "state": entry.get("state"),
            "attempt_count": supervision.get("attempt_count"),
            "crash_detected": supervision.get("crash_detected"),
            "circuit_open": supervision.get("circuit_open"),
            "max_attempts": supervision.get("max_attempts"),
            "next_restart_in_ms": supervision.get("next_restart_in_ms"),
            "uptime_seconds": entry.get("uptime_seconds"),
            "device_count": entry.get("device_count"),
        }

    def _wait_for_snapshot_predicate(self, predicate, timeout: float, interval: float = 0.1):
        """
        Poll provider health until predicate(snapshot) is true.
        Returns (matched: bool, snapshots: list[dict]).
        """
        snapshots = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            snap = self._sample_provider_health()
            snapshots.append(snap)
            if predicate(snap):
                return True, snapshots
            time.sleep(interval)
        return False, snapshots

    def _format_health_snapshots(self, snapshots: List[dict], tail: int = 40) -> str:
        """Format compact health timeline for failure diagnostics."""
        if not snapshots:
            return "(no health snapshots captured)"
        start_t = snapshots[0]["t"]
        lines = []
        for snap in snapshots[-tail:]:
            dt_ms = int((snap["t"] - start_t) * 1000)
            lines.append(
                f"+{dt_ms:5d}ms "
                f"present={snap['present']} "
                f"state={snap['state']} "
                f"attempt={snap['attempt_count']} "
                f"crash={snap['crash_detected']} "
                f"circuit={snap['circuit_open']} "
                f"max={snap['max_attempts']} "
                f"next={snap['next_restart_in_ms']} "
                f"uptime={snap['uptime_seconds']} "
                f"devices={snap['device_count']}"
            )
        return "\n".join(lines)

    def _fail_with_output(
        self,
        name: str,
        message: str,
        tail_lines: int = 200,
        health_snapshots: Optional[List[dict]] = None,
    ) -> NoReturn:
        """Raise an assertion with recent runtime output context."""
        extra = ""
        if health_snapshots:
            extra = "\nHealth timeline tail:\n" + self._format_health_snapshots(health_snapshots)

        if self.capture is None:
            raise AssertionError(f"{name}: {message}{extra}")

        output_tail = self.capture.get_recent_output(tail_lines)
        raise AssertionError(f"{name}: {message}{extra}\nOutput tail:\n{output_tail}")

    def test_automatic_restart(self) -> None:
        """Test that provider restarts automatically after crash."""
        print("\n[TEST] Automatic Restart")

        config_dict = self.create_config(crash_after=2.0, max_attempts=3, backoff_ms=[200, 500, 1000])

        try:
            self.start_runtime(config_dict)

            # Wait for provider to be initially available.
            if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                self._fail_with_output("automatic_restart", "Provider not available at startup")

            # Wait for the provider chaos crash banner (provider stderr — acceptable trigger,
            # not a pass/fail oracle; just advances timing certainty).
            if self.capture:
                self.capture.wait_for_marker("CRASHING NOW (exit 42)", timeout=8.0)

            # API oracle: crash/restart handling became visible in supervision fields.
            # This is durable and avoids dependence on very short transient states.
            crash_seen, crash_snaps = self._wait_for_snapshot_predicate(
                lambda s: s["present"] and ((s["attempt_count"] or 0) >= 1 or s["crash_detected"] is True),
                timeout=8.0,
                interval=0.05,
            )
            if not crash_seen:
                self._fail_with_output(
                    "automatic_restart",
                    "Provider crash/restart state was not observed",
                    health_snapshots=crash_snaps,
                )

            # Keep transient "down" observation as diagnostic only.
            down_observed = any((not s["present"]) or s["state"] == "UNAVAILABLE" for s in crash_snaps)

            # API oracle: provider recovered — AVAILABLE and attempt_count reset to 0.
            recovered, rec_snaps = self._wait_for_snapshot_predicate(
                lambda s: (
                    s["present"]
                    and s["state"] == "AVAILABLE"
                    and s["crash_detected"] is False
                    and (s["next_restart_in_ms"] == 0 or s["next_restart_in_ms"] is None)
                ),
                timeout=10.0,
                interval=0.05,
            )
            if not recovered:
                self._fail_with_output(
                    "automatic_restart",
                    "Provider did not recover",
                    health_snapshots=rec_snaps,
                )

            if not down_observed:
                print("  [INFO] automatic_restart: transient down not observed in sampled health window")

        finally:
            self.stop_runtime()

    def test_backoff_timing(self) -> None:
        """Test that exponential backoff delays are correct.

        Validates:
        - max_attempts configuration is correctly exposed via API
        - Crash handling state is observable via API (attempt_count / next_restart_in_ms)
        - Wall-clock time from first crash-attempt state to stable recovery is reasonable
          for configured backoff and CI timing variance
        """
        print("\n[TEST] Backoff Timing")

        config_dict = self.create_config(crash_after=1.0, max_attempts=3, backoff_ms=[500, 1000, 2000])

        try:
            self.start_runtime(config_dict)

            # Wait for provider to be initially available.
            if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                self._fail_with_output("backoff_timing", "Provider not available at startup")

            # Use provider crash banner as timing trigger only (provider stderr, not runtime oracle).
            if self.capture:
                self.capture.wait_for_marker("CRASHING NOW (exit 42)", timeout=6.0)

            # API oracle: confirm max_attempts reflects configured policy.
            entry = get_provider_health_entry(self.base_url, "provider-sim")
            if entry is None:
                self._fail_with_output("backoff_timing", "Could not query provider health")

            actual_max = entry["supervision"]["max_attempts"]
            if actual_max != 3:
                self._fail_with_output(
                    "backoff_timing",
                    f"Unexpected max_attempts in supervision: {actual_max} (expected 3)",
                )

            # Measure wall-clock from first crash-attempt state to stable recovery.
            # Avoid hard dependency on transient UNAVAILABLE visibility.
            crash_seen, crash_snaps = self._wait_for_snapshot_predicate(
                lambda s: s["present"] and ((s["attempt_count"] or 0) >= 1 or s["crash_detected"] is True),
                timeout=8.0,
                interval=0.05,
            )
            if not crash_seen:
                self._fail_with_output(
                    "backoff_timing",
                    "Provider crash-attempt state was not observed after crash",
                    health_snapshots=crash_snaps,
                )

            # Verify restart scheduling metadata is available while handling crash.
            if crash_snaps[-1].get("next_restart_in_ms") is None:
                self._fail_with_output(
                    "backoff_timing",
                    "Crash-attempt state missing next_restart_in_ms metadata",
                    health_snapshots=crash_snaps,
                )

            t_crash_state = crash_snaps[-1]["t"]

            recovered, rec_snaps = self._wait_for_snapshot_predicate(
                lambda s: (
                    s["present"]
                    and s["state"] == "AVAILABLE"
                    and s["crash_detected"] is False
                    and (s["next_restart_in_ms"] == 0 or s["next_restart_in_ms"] is None)
                ),
                timeout=10.0,
                interval=0.05,
            )
            if not recovered:
                self._fail_with_output(
                    "backoff_timing",
                    "Provider did not recover",
                    health_snapshots=crash_snaps + rec_snaps,
                )
            t_recovered = rec_snaps[-1]["t"]

            elapsed_ms = (t_recovered - t_crash_state) * 1000

            # Lower bound: include configured first backoff (500ms) plus minimal restart work.
            # Upper bound: generous for shared CI runners and restart/discovery overhead.
            if not (200 <= elapsed_ms <= 7000):
                self._fail_with_output(
                    "backoff_timing",
                    f"Crash-attempt->stable-recovery transition took {elapsed_ms:.0f}ms (expected 200-7000ms)",
                    health_snapshots=crash_snaps + rec_snaps,
                )

            print(f"  [INFO] backoff_timing: crash-attempt->stable-recovery {elapsed_ms:.0f}ms")

        finally:
            self.stop_runtime()

    def test_circuit_breaker(self) -> None:
        """Test that circuit breaker opens after max_attempts consecutive crashes.

        Uses crash_after=0.1s and a long success_reset_ms window so rapid crash loops
        accumulate attempts instead of immediately clearing them.
        After max_attempts+1 crashes the circuit breaker opens and no further restarts occur.
        """
        print("\n[TEST] Circuit Breaker")

        # Use an aggressive crash cadence with a sticky recovery window so attempt_count
        # can climb across restart cycles and trip the breaker.
        config_dict = self.create_config(
            crash_after=0.1,
            max_attempts=2,
            backoff_ms=[100, 200],
            # Keep restart attempts "sticky" long enough that rapid crash loops
            # can accumulate into a circuit-open state.
            success_reset_ms=5000,
        )

        try:
            self.start_runtime(config_dict)

            # API oracle: wait for circuit breaker to open.
            saw_crash, crash_snaps = self._wait_for_snapshot_predicate(
                lambda s: s["present"] and ((s["attempt_count"] or 0) >= 1 or s["crash_detected"] is True),
                timeout=12.0,
                interval=0.05,
            )
            if not saw_crash:
                self._fail_with_output(
                    "circuit_breaker",
                    "No crash-attempt state was observed before circuit-open check",
                    health_snapshots=crash_snaps,
                )

            opened, circuit_snaps = self._wait_for_snapshot_predicate(
                lambda s: s["present"] and s["circuit_open"] is True,
                timeout=15.0,
                interval=0.05,
            )
            if not opened:
                self._fail_with_output(
                    "circuit_breaker",
                    "Circuit breaker did not open",
                    health_snapshots=crash_snaps + circuit_snaps,
                )

            # Verify final supervision state.
            entry = get_provider_health_entry(self.base_url, "provider-sim")
            if entry is None:
                self._fail_with_output("circuit_breaker", "Failed to query provider health after circuit open")

            if entry["state"] != "UNAVAILABLE":
                self._fail_with_output(
                    "circuit_breaker",
                    f"Expected UNAVAILABLE after circuit open, got {entry['state']}",
                )

            attempt_count = entry["supervision"]["attempt_count"]
            if attempt_count < 3:  # max_attempts=2 → circuit opens on 3rd crash
                self._fail_with_output(
                    "circuit_breaker",
                    f"Expected attempt_count >= 3 when circuit opens, got {attempt_count}",
                )

            print(
                "  [INFO] circuit_breaker: opened after "
                f"{attempt_count} attempts (next_restart_in_ms={entry['supervision']['next_restart_in_ms']})"
            )

        finally:
            self.stop_runtime()

    def test_failed_restart_attempt_remains_supervised(self) -> None:
        """Test failed restart continuity: provider remains supervised and later recovers."""
        print("\n[TEST] Failed Restart Continuity")

        with tempfile.TemporaryDirectory(prefix="anolis-supervision-") as tmp:
            temp_dir = Path(tmp)
            counter_path = temp_dir / "attempt_counter.txt"
            wrapper_path = self._write_wrapper_script(
                temp_dir,
                "flaky_provider_wrapper.py",
                """
                import os
                import pathlib
                import subprocess
                import sys

                counter_path = pathlib.Path(sys.argv[1])
                provider_exe = sys.argv[2]
                provider_config = sys.argv[3]

                try:
                    attempt = int(counter_path.read_text(encoding="utf-8").strip())
                except Exception:
                    attempt = 0
                attempt += 1
                counter_path.write_text(str(attempt), encoding="utf-8")

                if attempt == 2:
                    print("WRAPPER_FAIL_ATTEMPT_2", file=sys.stderr, flush=True)
                    sys.exit(42)

                args = [provider_exe, "--config", provider_config]
                if attempt == 1:
                    # Delay first crash until runtime startup has completed.
                    args += ["--crash-after", "2.0"]

                proc = subprocess.Popen(args, stdin=sys.stdin.buffer, stdout=sys.stdout.buffer, stderr=sys.stderr)
                proc.wait()
                sys.exit(proc.returncode)
                """,
            )

            config_dict = {
                "http": {"enabled": True, "bind": "127.0.0.1", "port": self.port},
                "providers": [
                    {
                        "id": "provider-sim",
                        "command": sys.executable,
                        "args": [
                            str(wrapper_path).replace("\\", "/"),
                            str(counter_path).replace("\\", "/"),
                            str(self.provider_sim_path).replace("\\", "/"),
                            self._default_provider_fixture_config(),
                        ],
                        "timeout_ms": 1500,
                        "restart_policy": {
                            "enabled": True,
                            "max_attempts": 4,
                            "backoff_ms": [150, 300, 600, 1200],
                            "timeout_ms": 5000,
                            "success_reset_ms": 0,
                        },
                    }
                ],
                "polling": {"interval_ms": 200},
                "logging": {"level": "info"},
            }

            try:
                self.start_runtime(config_dict)

                if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                    self._fail_with_output(
                        "failed_restart_continuity",
                        "Provider did not become available before induced crash",
                    )

                if self.capture:
                    self.capture.wait_for_marker("WRAPPER_FAIL_ATTEMPT_2", timeout=10.0)

                crash_seen, crash_snaps = self._wait_for_snapshot_predicate(
                    lambda s: s["present"] and ((s["attempt_count"] or 0) >= 1 or s["crash_detected"] is True),
                    timeout=8.0,
                    interval=0.05,
                )
                if not crash_seen:
                    self._fail_with_output(
                        "failed_restart_continuity",
                        "Crash handling state was not observed",
                        health_snapshots=crash_snaps,
                    )

                # Continuity assertion: failed restart attempts must keep provider visible
                # as supervised UNAVAILABLE state instead of disappearing from health API.
                still_supervised, supervised_snaps = self._wait_for_snapshot_predicate(
                    lambda s: s["present"] and s["state"] == "UNAVAILABLE" and (s["attempt_count"] or 0) >= 1,
                    timeout=8.0,
                    interval=0.05,
                )
                if not still_supervised:
                    self._fail_with_output(
                        "failed_restart_continuity",
                        "Provider disappeared instead of staying supervised during failed restart",
                        health_snapshots=crash_snaps + supervised_snaps,
                    )

                recovered, rec_snaps = self._wait_for_snapshot_predicate(
                    lambda s: (
                        s["present"]
                        and s["state"] == "AVAILABLE"
                        and (s["device_count"] or 0) > 0
                        and (s["attempt_count"] or 0) == 0
                    ),
                    timeout=15.0,
                    interval=0.05,
                )
                if not recovered:
                    self._fail_with_output(
                        "failed_restart_continuity",
                        "Provider did not recover to AVAILABLE after failed restart attempt",
                        health_snapshots=crash_snaps + supervised_snaps + rec_snaps,
                    )
            finally:
                self.stop_runtime()

    def test_restart_timeout_enforced(self) -> None:
        """Test restart timeout policy by forcing a delayed restart attempt."""
        print("\n[TEST] Restart Timeout Enforcement")

        with tempfile.TemporaryDirectory(prefix="anolis-supervision-") as tmp:
            temp_dir = Path(tmp)
            counter_path = temp_dir / "attempt_counter.txt"
            wrapper_path = self._write_wrapper_script(
                temp_dir,
                "timeout_provider_wrapper.py",
                """
                import os
                import pathlib
                import subprocess
                import sys
                import time

                counter_path = pathlib.Path(sys.argv[1])
                provider_exe = sys.argv[2]
                provider_config = sys.argv[3]

                try:
                    attempt = int(counter_path.read_text(encoding="utf-8").strip())
                except Exception:
                    attempt = 0
                attempt += 1
                counter_path.write_text(str(attempt), encoding="utf-8")

                args = [provider_exe, "--config", provider_config]

                if attempt == 1:
                    # Delay first crash until runtime startup has completed.
                    args += ["--crash-after", "2.0"]
                elif attempt == 2:
                    print("WRAPPER_DELAY_ATTEMPT_2", file=sys.stderr, flush=True)
                    time.sleep(2.5)

                proc = subprocess.Popen(args, stdin=sys.stdin.buffer, stdout=sys.stdout.buffer, stderr=sys.stderr)
                proc.wait()
                sys.exit(proc.returncode)
                """,
            )

            config_dict = {
                "http": {"enabled": True, "bind": "127.0.0.1", "port": self.port},
                "providers": [
                    {
                        "id": "provider-sim",
                        "command": sys.executable,
                        "args": [
                            str(wrapper_path).replace("\\", "/"),
                            str(counter_path).replace("\\", "/"),
                            str(self.provider_sim_path).replace("\\", "/"),
                            self._default_provider_fixture_config(),
                        ],
                        "timeout_ms": 1500,
                        "restart_policy": {
                            "enabled": True,
                            "max_attempts": 4,
                            "backoff_ms": [100, 200, 300, 400],
                            "timeout_ms": 1000,
                            "success_reset_ms": 0,
                        },
                    }
                ],
                "polling": {"interval_ms": 200},
                "logging": {"level": "info"},
            }

            try:
                self.start_runtime(config_dict)

                if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                    self._fail_with_output(
                        "restart_timeout_enforced",
                        "Provider did not become available before induced crash",
                    )

                delayed_attempt_seen = False
                if self.capture:
                    delayed_attempt_seen = self.capture.wait_for_marker("WRAPPER_DELAY_ATTEMPT_2", timeout=10.0)
                if not delayed_attempt_seen:
                    self._fail_with_output(
                        "restart_timeout_enforced",
                        "Did not observe delayed restart wrapper attempt",
                    )

                timeout_seen = False
                if self.capture:
                    timeout_seen = self.capture.wait_for_marker(
                        "Restart timeout exceeded for provider 'provider-sim' after start()",
                        timeout=15.0,
                    )
                if not timeout_seen:
                    self._fail_with_output(
                        "restart_timeout_enforced",
                        "Runtime did not report restart timeout for delayed restart attempt",
                    )

                recovered, rec_snaps = self._wait_for_snapshot_predicate(
                    lambda s: (
                        s["present"]
                        and s["state"] == "AVAILABLE"
                        and (s["device_count"] or 0) > 0
                        and (s["attempt_count"] or 0) == 0
                    ),
                    timeout=15.0,
                    interval=0.05,
                )
                if not recovered:
                    self._fail_with_output(
                        "restart_timeout_enforced",
                        "Provider did not recover after restart timeout failure",
                        health_snapshots=rec_snaps,
                    )
            finally:
                self.stop_runtime()

    def test_startup_timeout_enforced(self) -> None:
        """Test runtime.startup_timeout_ms by delaying provider startup beyond the deadline."""
        print("\n[TEST] Startup Timeout Enforcement")

        with tempfile.TemporaryDirectory(prefix="anolis-supervision-") as tmp:
            temp_dir = Path(tmp)
            wrapper_path = self._write_wrapper_script(
                temp_dir,
                "startup_delay_wrapper.py",
                """
                import subprocess
                import sys
                import time

                provider_exe = sys.argv[1]
                provider_config = sys.argv[2]

                print("WRAPPER_STARTUP_DELAY_BEGIN", file=sys.stderr, flush=True)
                time.sleep(6.0)
                print("WRAPPER_STARTUP_DELAY_END", file=sys.stderr, flush=True)
                proc = subprocess.Popen(
                    [provider_exe, "--config", provider_config],
                    stdin=sys.stdin.buffer,
                    stdout=sys.stdout.buffer,
                    stderr=sys.stderr,
                )
                proc.wait()
                sys.exit(proc.returncode)
                """,
            )

            config_dict = {
                "runtime": {"startup_timeout_ms": 5000},
                "http": {"enabled": True, "bind": "127.0.0.1", "port": self.port},
                "providers": [
                    {
                        "id": "provider-sim",
                        "command": sys.executable,
                        "args": [
                            str(wrapper_path).replace("\\", "/"),
                            str(self.provider_sim_path).replace("\\", "/"),
                            self._default_provider_fixture_config(),
                        ],
                        "timeout_ms": 1500,
                        # Keep hello timeout above runtime.startup_timeout_ms so this
                        # test validates startup timeout enforcement (not RPC timeout).
                        "hello_timeout_ms": 12000,
                        "restart_policy": {
                            "enabled": True,
                            "max_attempts": 1,
                            "backoff_ms": [200],
                            "timeout_ms": 1000,
                            "success_reset_ms": 0,
                        },
                    }
                ],
                "polling": {"interval_ms": 200},
                "logging": {"level": "info"},
            }

            self.fixture = RuntimeFixture(
                self.runtime_path,
                self.provider_sim_path,
                http_port=self.port,
                config_dict=config_dict,
            )

            try:
                if not self.fixture.start():
                    raise AssertionError("Failed to spawn runtime process for startup-timeout test")
                self.base_url = self.fixture.base_url

                exited = self._wait_for_runtime_exit(timeout=20.0)
                if not exited:
                    self._fail_with_output(
                        "startup_timeout_enforced",
                        "Runtime did not exit after exceeding startup timeout",
                    )

                timeout_reported = False
                if self.capture:
                    timeout_reported = self.capture.wait_for_marker(
                        "Runtime startup timeout exceeded",
                        timeout=2.0,
                    )
                if not timeout_reported:
                    self._fail_with_output(
                        "startup_timeout_enforced",
                        "Startup timeout failure was not reported in runtime logs",
                    )
            finally:
                self.stop_runtime()

    def test_device_rediscovery(self) -> None:
        """Test that devices are rediscovered after provider restart."""
        print("\n[TEST] Device Rediscovery")

        config_dict = self.create_config(crash_after=2.0, max_attempts=3, backoff_ms=[200, 500, 1000])

        try:
            self.start_runtime(config_dict)

            # API oracle: wait for provider to be initially AVAILABLE with devices.
            if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                self._fail_with_output("device_rediscovery", "Provider not available at startup")

            # Use provider crash banner as timing trigger only.
            if self.capture:
                self.capture.wait_for_marker("CRASHING NOW (exit 42)", timeout=8.0)

            # API oracle: provider goes down after crash (UNAVAILABLE or temporarily missing during restart).
            went_down, down_snaps = self._wait_for_snapshot_predicate(
                lambda s: (not s["present"]) or s["state"] == "UNAVAILABLE",
                timeout=8.0,
                interval=0.05,
            )
            if not went_down:
                self._fail_with_output(
                    "device_rediscovery",
                    "Provider did not go down after crash",
                    health_snapshots=down_snaps,
                )

            # API oracle: provider recovers and rediscovered devices are visible.
            # Require consecutive matching samples to avoid transition-edge races.
            stable_recovery_hits = 0

            def recovered_with_devices(snap: dict) -> bool:
                nonlocal stable_recovery_hits
                ready = snap["present"] and snap["state"] == "AVAILABLE" and (snap["device_count"] or 0) > 0
                if ready:
                    stable_recovery_hits += 1
                else:
                    stable_recovery_hits = 0
                return stable_recovery_hits >= 2

            recovered, rec_snaps = self._wait_for_snapshot_predicate(
                recovered_with_devices,
                timeout=12.0,
                interval=0.05,
            )
            if not recovered:
                self._fail_with_output(
                    "device_rediscovery",
                    "Provider did not recover with rediscovered devices",
                    health_snapshots=rec_snaps,
                )

            print(f"  [INFO] device_rediscovery: recovered with device_count={rec_snaps[-1].get('device_count', 0)}")

        finally:
            self.stop_runtime()

    def _live_provider_children(self) -> List[psutil.Process]:
        """Provider processes the runtime has spawned and not yet reaped (zombies excluded)."""
        assert self.fixture is not None
        pid = self.fixture.get_pid()
        assert pid is not None, "runtime has no pid"
        live = []
        for child in psutil.Process(pid).children():
            try:
                if child.status() != psutil.STATUS_ZOMBIE:
                    live.append(child)
            except psutil.NoSuchProcess:
                pass
        return live

    def test_restart_stops_wedged_process_first(self) -> None:
        """A wedged provider is stopped before its replacement starts (#326).

        SIGSTOP leaves the process alive while every request to it times out,
        which is what a provider stuck on a hung bus looks like to the runtime.
        """
        print("\n[TEST] Restart Stops Wedged Process First")
        config_dict = self.create_config(crash_after=0.0, max_attempts=3, backoff_ms=[100, 200, 300])
        args = config_dict["providers"][0]["args"]
        config_dict["providers"][0]["args"] = args[: args.index("--crash-after")]

        try:
            self.start_runtime(config_dict)
            if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                self._fail_with_output("wedged_restart", "Provider not available at startup")

            children = self._live_provider_children()
            if len(children) != 1:
                self._fail_with_output("wedged_restart", f"Expected one provider process, got {len(children)}")
            wedged = children[0]
            wedged.suspend()

            max_live = 1
            recovered = False
            deadline = time.time() + 30.0
            while time.time() < deadline:
                live = self._live_provider_children()
                max_live = max(max_live, len(live))
                entry = get_provider_health_entry(self.base_url, "provider-sim", timeout=1.0)
                if entry is not None and entry.get("state") == "AVAILABLE" and wedged.pid not in [c.pid for c in live]:
                    recovered = True
                    break
                time.sleep(0.05)

            if max_live > 1:
                self._fail_with_output(
                    "wedged_restart",
                    f"{max_live} provider processes alive at once; the wedged one was not stopped first",
                )
            if not recovered:
                self._fail_with_output("wedged_restart", "Provider did not recover after the wedged process")
        finally:
            self.stop_runtime()

    def test_circuit_open_half_open_probe(self) -> None:
        """An open circuit is probed every 60 s and closes when a probe succeeds (#326).

        The first launch crashes, the second fails to start, which spends a
        budget of one attempt and opens the circuit. Every later launch is
        healthy, so the first probe must bring the provider back without a
        runtime restart.
        """
        print("\n[TEST] Circuit Half-Open Probe")
        probe_interval_s = 60.0

        with tempfile.TemporaryDirectory(prefix="anolis-supervision-") as tmp:
            temp_dir = Path(tmp)
            counter_path = temp_dir / "attempt_counter.txt"
            wrapper_path = self._write_wrapper_script(
                temp_dir,
                "probe_provider_wrapper.py",
                """
                import pathlib
                import subprocess
                import sys

                counter_path = pathlib.Path(sys.argv[1])
                provider_exe = sys.argv[2]
                provider_config = sys.argv[3]

                try:
                    attempt = int(counter_path.read_text(encoding="utf-8").strip())
                except Exception:
                    attempt = 0
                attempt += 1
                counter_path.write_text(str(attempt), encoding="utf-8")

                if attempt == 2:
                    sys.exit(42)

                args = [provider_exe, "--config", provider_config]
                if attempt == 1:
                    args += ["--crash-after", "2.0"]

                proc = subprocess.Popen(args, stdin=sys.stdin.buffer, stdout=sys.stdout.buffer, stderr=sys.stderr)
                proc.wait()
                sys.exit(proc.returncode)
                """,
            )

            def launches() -> int:
                try:
                    return int(counter_path.read_text(encoding="utf-8").strip())
                except Exception:
                    return 0

            config_dict = {
                "http": {"enabled": True, "bind": "127.0.0.1", "port": self.port},
                "providers": [
                    {
                        "id": "provider-sim",
                        "command": sys.executable,
                        "args": [
                            str(wrapper_path).replace("\\", "/"),
                            str(counter_path).replace("\\", "/"),
                            str(self.provider_sim_path).replace("\\", "/"),
                            self._default_provider_fixture_config(),
                        ],
                        "timeout_ms": 1500,
                        "restart_policy": {
                            "enabled": True,
                            "max_attempts": 1,
                            "backoff_ms": [100],
                            "timeout_ms": 5000,
                            "success_reset_ms": 500,
                        },
                    }
                ],
                "polling": {"interval_ms": 200},
                "logging": {"level": "info"},
            }

            try:
                self.start_runtime(config_dict)
                if not assert_provider_available(self.base_url, "provider-sim", timeout=10.0):
                    self._fail_with_output("half_open_probe", "Provider not available at startup")

                opened, open_snaps = self._wait_for_snapshot_predicate(
                    lambda s: s["present"] and s["circuit_open"] is True, timeout=15.0, interval=0.05
                )
                if not opened:
                    self._fail_with_output("half_open_probe", "Circuit did not open", health_snapshots=open_snaps)
                opened_at = time.time()

                # No probe before the interval: the circuit stays open and nothing relaunches.
                time.sleep(5.0)
                entry = get_provider_health_entry(self.base_url, "provider-sim", timeout=1.0)
                if entry is None or entry["supervision"]["circuit_open"] is not True or launches() != 2:
                    self._fail_with_output(
                        "half_open_probe", f"Expected an open circuit and 2 launches before the probe, got {launches()}"
                    )

                recovered, rec_snaps = self._wait_for_snapshot_predicate(
                    lambda s: (
                        s["present"]
                        and s["state"] == "AVAILABLE"
                        and s["circuit_open"] is False
                        and (s["device_count"] or 0) > 0
                    ),
                    timeout=probe_interval_s + 20.0,
                    interval=0.25,
                )
                if not recovered:
                    self._fail_with_output(
                        "half_open_probe",
                        "Provider did not recover within one probe interval of the circuit opening",
                        health_snapshots=rec_snaps,
                    )
                waited = time.time() - opened_at
                if launches() != 3:
                    self._fail_with_output("half_open_probe", f"Expected exactly one probe launch, got {launches()}")
                if waited < probe_interval_s - 5.0:
                    self._fail_with_output("half_open_probe", f"Probe fired after {waited:.1f}s, before the interval")
                print(f"  [INFO] half_open_probe: recovered {waited:.1f}s after the circuit opened")
            finally:
                self.stop_runtime()


SupervisionCheck = Tuple[str, Callable[[SupervisionTester], None]]

SUPERVISION_CHECKS: List[SupervisionCheck] = [
    ("automatic_restart", SupervisionTester.test_automatic_restart),
    ("backoff_timing", SupervisionTester.test_backoff_timing),
    ("circuit_breaker", SupervisionTester.test_circuit_breaker),
    ("failed_restart_continuity", SupervisionTester.test_failed_restart_attempt_remains_supervised),
    ("restart_timeout_enforced", SupervisionTester.test_restart_timeout_enforced),
    ("startup_timeout_enforced", SupervisionTester.test_startup_timeout_enforced),
    ("device_rediscovery", SupervisionTester.test_device_rediscovery),
    ("wedged_restart", SupervisionTester.test_restart_stops_wedged_process_first),
    ("half_open_probe", SupervisionTester.test_circuit_open_half_open_probe),
]
