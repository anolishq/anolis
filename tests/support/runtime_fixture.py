"""
Test Fixtures for Anolis Integration Tests

Provides RuntimeFixture for safe, scoped process management:
- Process-group scoped termination (no global pkill/taskkill)
- Proper cleanup on test failure (try/finally safety)
- Cross-platform support (Windows and Linux)
- Timeout and escalation (SIGTERM -> SIGKILL)
"""

import copy
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from queue import Empty, Queue
from typing import List, Optional

import requests


@dataclass
class ProcessInfo:
    """Information about a spawned process."""

    pid: int
    process_group_id: int
    process: subprocess.Popen


class OutputCapture:
    """Thread-safe output capture with timeout support."""

    def __init__(self, process: subprocess.Popen):
        self.process = process
        self.lines: List[str] = []
        self.lock = threading.Lock()
        self.queue: Queue = Queue()
        self.stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self):
        """Start capturing output in background thread."""
        self._thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._thread.start()

    def _capture_loop(self):
        """Background thread that reads merged process output."""
        try:
            stream = self.process.stdout
            if stream is None:
                return

            while not self.stop_event.is_set():
                if self.process.poll() is not None:
                    # Process ended, read remaining output
                    remaining = stream.read()
                    if remaining:
                        for line in remaining.splitlines():
                            self._add_line(line)
                    break

                line = stream.readline()
                if line:
                    self._add_line(line.rstrip("\n\r"))
        except Exception as e:
            self._add_line(f"[CAPTURE ERROR] {e}")

    def _add_line(self, line: str):
        """Add line to buffer and queue."""
        with self.lock:
            self.lines.append(line)
        self.queue.put(line)

    def wait_for_marker(self, marker: str, timeout: float = 10.0) -> bool:
        """Wait for a specific marker to appear in output."""
        deadline = time.time() + timeout

        # First check existing lines
        with self.lock:
            for line in self.lines:
                if marker in line:
                    return True

        # Wait for new lines
        while time.time() < deadline:
            try:
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                line = self.queue.get(timeout=min(remaining, 0.5))
                if marker in line:
                    return True
            except Empty:
                continue

        return False

    def wait_for_pattern(self, pattern: str, timeout: float = 10.0):
        """Wait for a regex pattern to appear in output. Returns Match object or None."""
        import re

        deadline = time.time() + timeout
        regex = re.compile(pattern)

        # First check existing lines
        with self.lock:
            for line in self.lines:
                match = regex.search(line)
                if match:
                    return match

        # Wait for new lines
        while time.time() < deadline:
            try:
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                line = self.queue.get(timeout=min(remaining, 0.5))
                match = regex.search(line)
                if match:
                    return match
            except Empty:
                continue

        return None

    def get_all_output(self) -> str:
        """Get all captured output as a single string."""
        with self.lock:
            return "\n".join(self.lines)

    def get_recent_output(self, num_lines: int = 50) -> str:
        """Get the most recent N lines of output."""
        with self.lock:
            recent = self.lines[-num_lines:] if len(self.lines) > num_lines else self.lines
            return "\n".join(recent)

    def stop(self):
        """Stop the capture thread."""
        self.stop_event.set()
        if self._thread:
            self._thread.join(timeout=2.0)


class RuntimeFixture:
    """
    Fixture for managing anolis-runtime process lifecycle.

    Provides:
    - Process-group scoped termination (no collateral damage)
    - Automatic cleanup on test failure
    - Proper timeout and escalation
    - Output capture for debugging

    Usage:
        fixture = RuntimeFixture(runtime_path, provider_path)
        try:
            if not fixture.start():
                return False  # startup failed
            # ... run tests ...
        finally:
            fixture.cleanup()  # Always runs, even on exception
    """

    def __init__(
        self,
        runtime_path: Path,
        provider_path: Path,
        http_port: int = 8080,
        verbose: bool = False,
        config_dict: Optional[dict] = None,
    ):
        self.runtime_path = runtime_path
        self.provider_path = provider_path
        self.http_port = http_port
        self.verbose = verbose
        self.config_dict = config_dict

        self.process_info: Optional[ProcessInfo] = None
        self.capture: Optional[OutputCapture] = None
        self.config_path: Optional[Path] = None
        # The runtime's data_dir, unless config_dict sets one. Each fixture gets
        # its own: the software stop's latch is kept there across restarts
        # (#482), so a shared one would carry a stop from one test into the next.
        self.data_dir: Optional[Path] = None

    def start(
        self,
        *,
        wait_for_ready: bool = False,
        provider_id: Optional[str] = None,
        min_device_count: Optional[int] = None,
        startup_timeout: float = 15.0,
    ) -> bool:
        """
        Start the runtime process.

        Args:
            wait_for_ready: If True, block until HTTP/API readiness checks pass.
            provider_id: Optional provider ID that must be AVAILABLE before success.
            min_device_count: Optional minimum device count required before success.
            startup_timeout: Max seconds to wait for readiness when wait_for_ready=True.

        Returns:
            True if startup succeeded, False otherwise
        """
        # Create temporary config file
        if not self._create_config():
            return False

        # Spawn runtime in new process group
        try:
            # On Windows: CREATE_NEW_PROCESS_GROUP gives us a process group we can kill
            # On Linux: start_new_session=True makes the process a session leader
            if sys.platform == "win32":
                process = subprocess.Popen(
                    [str(self.runtime_path), f"--config={self.config_path}"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                )
                pgid = process.pid  # On Windows, PGID == PID for process group leaders
            else:
                process = subprocess.Popen(
                    [str(self.runtime_path), f"--config={self.config_path}"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    start_new_session=True,  # Creates new session, makes process session leader
                )
                pgid = os.getpgid(process.pid)  # Get process group ID

            self.process_info = ProcessInfo(
                pid=process.pid,
                process_group_id=pgid,
                process=process,
            )

            # Start output capture
            self.capture = OutputCapture(process)
            self.capture.start()

            if self.verbose:
                print(f"[RuntimeFixture] Started runtime PID={process.pid} PGID={pgid}")

            if wait_for_ready and not self.wait_until_ready(
                provider_id=provider_id,
                min_device_count=min_device_count,
                timeout=startup_timeout,
            ):
                if self.verbose:
                    print("[RuntimeFixture] Startup readiness wait timed out or process exited")
                return False

            return True

        except Exception as e:
            print(f"[RuntimeFixture] ERROR: Failed to start runtime: {e}")
            return False

    def wait_until_ready(
        self,
        *,
        provider_id: Optional[str] = None,
        min_device_count: Optional[int] = None,
        timeout: float = 15.0,
        interval: float = 0.2,
    ) -> bool:
        """
        Wait until runtime API is responsive and optional provider/device checks pass.

        This is intentionally API-based (not log-marker-based), so tests do not depend
        on specific log text and are resilient to startup timing variance in CI.
        """
        deadline = time.time() + max(timeout, 0.1)
        while time.time() < deadline:
            if not self.is_running():
                return False

            try:
                status_resp = requests.get(f"{self.base_url}/v0/runtime/status", timeout=1.0)
                if status_resp.status_code != 200:
                    time.sleep(interval)
                    continue
                status = status_resp.json()
            except (requests.exceptions.RequestException, ValueError):
                time.sleep(interval)
                continue

            if provider_id is not None:
                providers = status.get("providers", [])
                provider_ready = any(
                    entry.get("provider_id") == provider_id and entry.get("state") == "AVAILABLE" for entry in providers
                )
                if not provider_ready:
                    time.sleep(interval)
                    continue

            if min_device_count is not None:
                try:
                    devices_resp = requests.get(f"{self.base_url}/v0/devices", timeout=1.0)
                    if devices_resp.status_code != 200:
                        time.sleep(interval)
                        continue
                    devices = devices_resp.json().get("devices", [])
                    if len(devices) < min_device_count:
                        time.sleep(interval)
                        continue
                except (requests.exceptions.RequestException, ValueError):
                    time.sleep(interval)
                    continue

            return True

        return False

    def cleanup(self):
        """
        Clean up runtime process and resources.

        Uses process-group scoped termination:
        1. Send SIGTERM/CTRL_BREAK to entire process group
        2. Wait up to 5 seconds for graceful shutdown
        3. If still running, send SIGKILL/TERMINATE to group
        4. Clean up temp files and output capture
        """
        if not self.process_info:
            return

        try:
            process = self.process_info.process
            pid = self.process_info.pid
            pgid = self.process_info.process_group_id

            if self.verbose:
                print(f"[RuntimeFixture] Cleaning up runtime PID={pid} PGID={pgid}")

            # Step 1: Try graceful shutdown
            if process.poll() is None:  # Process still running
                try:
                    if sys.platform == "win32":
                        # Windows: Send CTRL_BREAK_EVENT to process group
                        os.kill(pid, signal.CTRL_BREAK_EVENT)
                    else:
                        # Linux: Send SIGTERM to entire process group (negative PID)
                        os.killpg(pgid, signal.SIGTERM)

                    if self.verbose:
                        print("[RuntimeFixture] Sent SIGTERM to process group")

                except (ProcessLookupError, OSError) as e:
                    # Process already dead
                    if self.verbose:
                        print(f"[RuntimeFixture] Process already terminated: {e}")

            # Step 2: Wait for graceful shutdown
            try:
                process.wait(timeout=5.0)
                if self.verbose:
                    print("[RuntimeFixture] Process exited gracefully")
            except subprocess.TimeoutExpired:
                # Step 3: Force kill if still running
                if self.verbose:
                    print("[RuntimeFixture] Timeout expired, force killing process group")

                try:
                    if sys.platform == "win32":
                        # Windows: TerminateProcess
                        process.kill()
                    else:
                        # Linux: SIGKILL to entire process group
                        os.killpg(pgid, signal.SIGKILL)

                    process.wait(timeout=2.0)
                except (ProcessLookupError, OSError, subprocess.TimeoutExpired):
                    pass  # Already dead or unkillable

        except Exception as e:
            if self.verbose:
                print(f"[RuntimeFixture] WARNING: Cleanup error: {e}")

        # Step 4: Clean up resources
        if self.capture:
            self.capture.stop()

        if self.config_path and self.config_path.exists():
            try:
                self.config_path.unlink()
            except OSError:
                pass
        if self.data_dir is not None:
            shutil.rmtree(self.data_dir, ignore_errors=True)
            self.data_dir = None

        self.process_info = None

    def get_pid(self) -> Optional[int]:
        """Get the runtime process PID, or None if not started."""
        return self.process_info.pid if self.process_info else None

    def get_pgid(self) -> Optional[int]:
        """Get the runtime process group ID, or None if not started."""
        return self.process_info.process_group_id if self.process_info else None

    @property
    def base_url(self) -> str:
        """Base URL for the runtime HTTP API (e.g. 'http://127.0.0.1:8080')."""
        return f"http://127.0.0.1:{self.http_port}"

    def get_output_capture(self) -> Optional[OutputCapture]:
        """Get the output capture instance for log inspection."""
        return self.capture

    def is_running(self) -> bool:
        """Check if the runtime process is still running."""
        if not self.process_info:
            return False
        return self.process_info.process.poll() is None

    def _create_config(self) -> bool:
        """Create temporary configuration file."""
        if self.data_dir is None:
            self.data_dir = Path(tempfile.mkdtemp(prefix="anolis-test-data-"))
        data_dir_str = str(self.data_dir).replace("\\", "/")
        if self.config_dict:
            # Use custom config if provided
            import yaml

            config = copy.deepcopy(self.config_dict)
            runtime = config.get("runtime") or {}
            runtime.setdefault("data_dir", data_dir_str)
            config["runtime"] = runtime
            try:
                fd, path = tempfile.mkstemp(suffix=".yaml", prefix="anolis-test-")
                with os.fdopen(fd, "w") as f:
                    yaml.dump(config, f)
                self.config_path = Path(path)
                return True
            except Exception as e:
                print(f"[RuntimeFixture] ERROR: Failed to create config: {e}")
                return False

        # Default config
        # Get path to default provider config fixture
        fixture_config = Path(__file__).parent.parent / "integration" / "fixtures" / "provider-sim-default.yaml"
        fixture_config_str = str(fixture_config).replace("\\", "/")

        config_content = f"""
runtime:
  data_dir: "{data_dir_str}"

http:
  enabled: true
  bind: 127.0.0.1
  port: {self.http_port}

providers:
  - id: sim0
    command: {str(self.provider_path)}
    args: ["--config", "{fixture_config_str}"]
    timeout_ms: 5000

polling:
  interval_ms: 500

telemetry:
  enabled: false

logging:
  level: info
"""

        try:
            fd, path = tempfile.mkstemp(suffix=".yaml", prefix="anolis-test-")
            os.write(fd, config_content.encode("utf-8"))
            os.close(fd)
            self.config_path = Path(path)
            return True
        except Exception as e:
            print(f"[RuntimeFixture] ERROR: Failed to create config: {e}")
            return False


def verify_process_cleanup(pid: int, timeout: float = 2.0) -> bool:
    """
    Verify that a specific PID has been terminated.

    Args:
        pid: Process ID to check
        timeout: How long to wait for termination

    Returns:
        True if process terminated, False if still running
    """
    deadline = time.time() + timeout

    while time.time() < deadline:
        try:
            if sys.platform == "win32":
                # Windows: Check exit code - if process is running, this raises
                import ctypes

                kernel32 = ctypes.windll.kernel32
                PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
                STILL_ACTIVE = 259

                handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
                if handle == 0:
                    # Process doesn't exist
                    return True

                exit_code = ctypes.c_ulong()
                result = kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code))
                kernel32.CloseHandle(handle)

                if result and exit_code.value != STILL_ACTIVE:
                    # Process has exited
                    return True
                # else: Process still active, continue waiting

            else:
                # Linux: Send signal 0 (no-op but checks if process exists)
                os.kill(pid, 0)
                # If we get here, process still exists

        except (OSError, ProcessLookupError):
            # Process doesn't exist
            return True

        time.sleep(0.1)

    return False  # Timeout: process still exists
