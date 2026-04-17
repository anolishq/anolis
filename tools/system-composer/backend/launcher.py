"""launcher.py — Process management for the Anolis System Composer.

Preflight checks, process launch, stop, restart, and SSE log streaming.
All public functions are called from server.py HTTP handlers.
"""

import json
import os
import pathlib
import queue
import signal
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

from anolis_composer_backend import paths as paths_module

_CATALOG_PATH = paths_module.CATALOG_PATH

_state: dict = {
    "project": None,  # active project name
    "process": None,  # subprocess.Popen handle
    "log_file": None,  # open file handle for latest.log
    "log_lines_by_project": {},  # dict[str, list[str]] ring buffers (last 200 lines)
}
_state_lock = threading.Lock()

_sse_subscribers: dict[str, list] = {}  # dict[project_name, list[queue.Queue]]
_sse_lock = threading.Lock()

_catalog_cache: dict | None = None


def _load_catalog() -> dict:
    global _catalog_cache
    if _catalog_cache is None:
        with open(_CATALOG_PATH, encoding="utf-8") as f:
            data = json.load(f)
        _catalog_cache = {p["kind"]: p for p in data["providers"]}
    return _catalog_cache


def running_json_path(name: str) -> pathlib.Path:
    return paths_module.SYSTEMS_ROOT / name / "running.json"


def _resolve_executable_path(path_value: str | None) -> pathlib.Path | None:
    if not path_value:
        return None
    return paths_module.resolve_repo_path(path_value)


# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------


def get_status() -> dict:
    """Return serialisable status dict for GET /api/status."""
    runtime = _current_runtime_snapshot(clean_stale=True)
    return {
        "version": 1,
        "active_project": runtime["project"],
        "running": runtime["running"],
        "pid": runtime["pid"],
    }


def _pid_exists(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        import psutil

        return bool(psutil.pid_exists(pid))
    except ImportError:
        pass

    if os.name == "nt":
        import ctypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if handle == 0:
            return False
        ctypes.windll.kernel32.CloseHandle(handle)
        return True

    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False


def _terminate_pid(pid: int, timeout_s: float = 5.0) -> None:
    if pid <= 0:
        return

    try:
        import psutil
    except ImportError:
        psutil = None

    if psutil is not None:
        try:
            proc = psutil.Process(pid)
            proc.terminate()
            try:
                proc.wait(timeout=timeout_s)
            except psutil.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=timeout_s)
            return
        except psutil.Error:
            return

    if os.name == "nt":
        import ctypes

        PROCESS_TERMINATE = 0x0001
        SYNCHRONIZE = 0x00100000
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, False, pid)
        if handle == 0:
            return
        try:
            ctypes.windll.kernel32.TerminateProcess(handle, 1)
            ctypes.windll.kernel32.WaitForSingleObject(handle, int(timeout_s * 1000))
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
        return

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except PermissionError:
        return

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not _pid_exists(pid):
            return
        time.sleep(0.1)
    if _pid_exists(pid):
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            pass


def _discover_running_runtime(clean_stale: bool = False) -> dict | None:
    systems_dir = paths_module.SYSTEMS_ROOT
    if not systems_dir.exists():
        return None

    for project_dir in sorted(systems_dir.iterdir()):
        if not project_dir.is_dir():
            continue
        running_path = project_dir / "running.json"
        if not running_path.exists():
            continue

        try:
            data = json.loads(running_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            if clean_stale:
                try:
                    running_path.unlink(missing_ok=True)
                except OSError:
                    pass
            continue

        pid = data.get("pid")
        if not isinstance(pid, int):
            if clean_stale:
                try:
                    running_path.unlink(missing_ok=True)
                except OSError:
                    pass
            continue

        if _pid_exists(pid):
            return {
                "running": True,
                "project": project_dir.name,
                "pid": pid,
                "managed": False,
            }

        if clean_stale:
            try:
                running_path.unlink(missing_ok=True)
            except OSError:
                pass

    return None


def _current_runtime_snapshot(clean_stale: bool = False) -> dict:
    with _state_lock:
        proc = _state["process"]
        if proc is not None and proc.poll() is None:
            return {
                "running": True,
                "project": _state["project"],
                "pid": proc.pid,
                "managed": True,
            }

    discovered = _discover_running_runtime(clean_stale=clean_stale)
    if discovered is not None:
        return discovered

    return {"running": False, "project": None, "pid": None, "managed": False}


# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------


def preflight(name: str, system: dict, project_dir: pathlib.Path) -> dict:
    """
    Run preflight checks and return {"ok": bool, "checks": [...]}.
    Re-renders YAML to disk before running binary checks.
    """
    from anolis_composer_backend import (
        renderer,  # local import avoids any circular at import time
        validator,
    )

    checks: list[dict] = []
    catalog = _load_catalog()

    # Re-render YAML to disk first so --check-config sees current state
    try:
        renders = renderer.render(system, name, systems_dir_name=paths_module.SYSTEMS_ROOT.name)
        for rel_path, content in renders.items():
            out = project_dir / rel_path
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(content, encoding="utf-8")
    except Exception as exc:
        checks.append(
            {
                "name": "Render YAML to disk",
                "ok": False,
                "error": str(exc),
                "hint": None,
            }
        )
        return {"ok": False, "checks": checks}

    # Check 1: Runtime binary
    runtime_exe_value = system.get("paths", {}).get("runtime_executable", "")
    runtime_exe = _resolve_executable_path(runtime_exe_value)
    _exists_check(
        checks,
        "Runtime binary exists",
        runtime_exe,
        hint=_build_binary_hint(runtime_exe, kind="runtime", repo="anolis", docs="docs/"),
    )

    # Check 2: Provider binaries
    providers = system.get("topology", {}).get("providers", {})
    for pid, pcfg in providers.items():
        exe_str = system.get("paths", {}).get("providers", {}).get(pid, {}).get("executable", "")
        exe = _resolve_executable_path(exe_str)
        kind = pcfg.get("kind", "")
        kind_info = catalog.get(kind, {})
        repo = kind_info.get("repo")
        docs = kind_info.get("build_docs")
        _exists_check(
            checks,
            f"Provider {pid} binary exists",
            exe,
            hint=_build_binary_hint(exe, kind=kind, repo=repo, docs=docs) if exe is not None else None,
        )

    # Check 3: Output paths writable
    checks.append(_check_writable(project_dir))

    # Check 3b: Runtime port in use
    rt_port = system.get("topology", {}).get("runtime", {}).get("http_port")
    if rt_port is not None:
        checks.append(_check_port(rt_port))

    # Check 4: System-level validation
    errors = validator.validate_system(system)
    if errors:
        for err in errors:
            checks.append({"name": "System-level validation", "ok": False, "error": err, "hint": None})
    else:
        checks.append({"name": "System-level validation", "ok": True, "error": None, "hint": None})

    # Check 5: Runtime --check-config
    checks.append(
        _check_config_binary(
            "Runtime --check-config",
            runtime_exe,
            project_dir / "anolis-runtime.yaml",
        )
    )

    # Check 6+: Provider --check-config (only for kinds with check_config_flag)
    for pid, pcfg in providers.items():
        kind = pcfg.get("kind", "")
        if not catalog.get(kind, {}).get("check_config_flag"):
            continue
        exe_str = system.get("paths", {}).get("providers", {}).get(pid, {}).get("executable", "")
        exe = _resolve_executable_path(exe_str)
        yaml_path = project_dir / "providers" / f"{pid}.yaml"
        checks.append(
            _check_config_binary(
                f"Provider {pid} --check-config",
                exe,
                yaml_path,
            )
        )

    ok = all(c.get("ok") is not False for c in checks)
    return {"ok": ok, "checks": checks}


# ---------------------------------------------------------------------------
# Platform-aware build hints
# ---------------------------------------------------------------------------

# Preset names per kind, keyed by platform ('win32' or 'other')
_PRESETS: dict[str, dict[str, str]] = {
    "win32": {
        "runtime": "dev-windows-release",
        "sim": "dev-windows-release",
        "bread": "dev-windows-release",
        "ezo": "dev-windows-release",
        "custom": "dev-release",
    },
    "other": {
        "runtime": "dev-release",
        "sim": "dev-release",
        "bread": "dev-linux-hardware-release",
        "ezo": "dev-linux-hardware-release",
        "custom": "dev-release",
    },
}


def _build_binary_hint(exe: pathlib.Path | None, kind: str, repo: str | None, docs: str | None) -> str:
    """Return an actionable build hint for a missing binary."""
    platform_key = "win32" if sys.platform == "win32" else "other"
    preset = _PRESETS.get(platform_key, {}).get(kind, "dev-release")

    # Detect whether the sibling repo directory exists
    if repo:
        sibling = pathlib.Path("..") / repo
        if sibling.is_dir():
            repo_note = f"Repo '{repo}' found but not built."
        else:
            repo_note = f"Repo not found — clone '{repo}' as a sibling of this repo."
    else:
        repo_note = ""

    parts = []
    if repo_note:
        parts.append(repo_note)
    if sys.platform == "win32" and kind in ("bread", "ezo"):
        parts.append(
            "Windows note: use dev-windows-release for mock validation. "
            "Live I2C hardware validation is expected on Linux with device access."
        )
    if docs and repo:
        parts.append(f"Build docs: {repo}/{docs}")
    parts.append(f"CMake preset: {preset}")
    return "  ".join(parts)


def _check_port(port: int) -> dict:
    """Check whether a TCP port is already occupied."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        in_use = s.connect_ex(("127.0.0.1", port)) == 0
    if in_use:
        return {
            "name": f"Runtime port {port} available",
            "ok": False,
            "error": f"Port {port} is already in use.",
            "hint": "Stop the existing process or change the runtime port in the config.",
        }
    return {"name": f"Runtime port {port} available", "ok": True, "error": None, "hint": None}


def _exists_check(checks: list, name: str, path: pathlib.Path | None, hint: str | None = None) -> None:
    exists = path is not None and path.exists()
    checks.append(
        {
            "name": name,
            "ok": exists,
            "error": None if exists else f"File not found: {path if path is not None else '<missing>'}",
            "hint": hint if not exists else None,
        }
    )


def _check_writable(project_dir: pathlib.Path) -> dict:
    name = "Output paths writable"
    try:
        project_dir.mkdir(parents=True, exist_ok=True)
        test_file = project_dir / ".composer_write_test"
        test_file.write_text("x", encoding="utf-8")
        test_file.unlink()
        return {"name": name, "ok": True, "error": None, "hint": None}
    except OSError as exc:
        return {
            "name": name,
            "ok": False,
            "error": f"{exc} (path: {project_dir})",
            "hint": "Check directory permissions or available disk space.",
        }


def _check_config_binary(check_name: str, exe: pathlib.Path | None, yaml_path: pathlib.Path) -> dict:
    if exe is None or not exe.exists():
        return {"name": check_name, "ok": None, "note": "Binary missing — skipped"}
    if not yaml_path.exists():
        return {"name": check_name, "ok": None, "note": "Config not yet rendered"}
    try:
        result = subprocess.run(
            [str(exe), "--check-config", str(yaml_path)],
            cwd=str(paths_module.DATA_ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        if result.returncode == 0:
            return {"name": check_name, "ok": True, "error": None, "hint": None}
        stderr_lower = (result.stderr or "").lower()
        if any(kw in stderr_lower for kw in ("unknown", "unrecognized", "invalid option")):
            return {"name": check_name, "ok": None, "note": "Not yet available"}
        error = (result.stderr or result.stdout or "Non-zero exit").strip()[:200]
        return {"name": check_name, "ok": False, "error": error, "hint": None}
    except subprocess.TimeoutExpired:
        return {"name": check_name, "ok": False, "error": "Timed out (10s)", "hint": None}
    except OSError as exc:
        return {"name": check_name, "ok": False, "error": str(exc), "hint": None}


# ---------------------------------------------------------------------------
# Launch
# ---------------------------------------------------------------------------


def launch(name: str, system: dict, project_dir: pathlib.Path) -> None:
    """Start the anolis-runtime subprocess."""
    from anolis_composer_backend import renderer

    current = _current_runtime_snapshot(clean_stale=True)
    if current["running"]:
        raise RuntimeError("A system is already running.")

    # Re-render YAML to disk
    renders = renderer.render(system, name, systems_dir_name=paths_module.SYSTEMS_ROOT.name)
    for rel_path, content in renders.items():
        out = project_dir / rel_path
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(content, encoding="utf-8")

    # Prepare log file (overwrite)
    log_path = project_dir / "logs" / "latest.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = open(log_path, "w", buffering=1, encoding="utf-8")

    # Build command
    runtime_exe = _resolve_executable_path(system.get("paths", {}).get("runtime_executable", ""))
    if runtime_exe is None:
        raise RuntimeError("Runtime executable path is missing.")
    runtime_config = str((project_dir / "anolis-runtime.yaml").resolve())
    cmd = [str(runtime_exe), "--config", runtime_config]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(paths_module.DATA_ROOT),
        bufsize=1,
        text=True,
    )

    # Write running.json
    running_data = {
        "pid": proc.pid,
        "project": name,
        "started": datetime.now(timezone.utc).isoformat(),
    }
    (project_dir / "running.json").write_text(json.dumps(running_data), encoding="utf-8")

    with _state_lock:
        log_lines_by_project: dict = _state["log_lines_by_project"]
        log_lines_by_project[name] = []
        _state.update(
            {
                "project": name,
                "process": proc,
                "log_file": log_file,
            }
        )
    threading.Thread(target=_read_logs, args=(proc, log_file, name), daemon=True).start()


# ---------------------------------------------------------------------------
# Stop
# ---------------------------------------------------------------------------


def stop() -> None:
    """Gracefully terminate the running process."""
    with _state_lock:
        proc = _state["process"]
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        return

    detached = _discover_running_runtime(clean_stale=False)
    if detached is None:
        return

    _terminate_pid(detached["pid"])
    if not _pid_exists(detached["pid"]):
        try:
            running_json_path(detached["project"]).unlink(missing_ok=True)
        except OSError:
            pass
        _notify_sse_subscribers(detached["project"], None)


# ---------------------------------------------------------------------------
# Restart
# ---------------------------------------------------------------------------


def restart(name: str, project_dir: pathlib.Path) -> None:
    """Kill and relaunch using the already-rendered YAML on disk.

    Does NOT re-render from current system.json — uses the YAML already on disk.
    """
    current = _current_runtime_snapshot(clean_stale=True)
    if not current["running"]:
        raise RuntimeError(f"Cannot restart '{name}' because no project is running.")
    if current["project"] != name:
        raise RuntimeError(
            f"Cannot restart '{name}' while '{current['project']}' is running. Stop '{current['project']}' first."
        )

    stop()
    time.sleep(0.5)  # Allow log reader thread to flush and clean up

    system = json.loads((project_dir / "system.json").read_text(encoding="utf-8"))

    log_path = project_dir / "logs" / "latest.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = open(log_path, "w", buffering=1, encoding="utf-8")

    runtime_exe = _resolve_executable_path(system.get("paths", {}).get("runtime_executable", ""))
    if runtime_exe is None:
        raise RuntimeError("Runtime executable path is missing.")
    runtime_config = str((project_dir / "anolis-runtime.yaml").resolve())
    cmd = [str(runtime_exe), "--config", runtime_config]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(paths_module.DATA_ROOT),
        bufsize=1,
        text=True,
    )

    running_data = {
        "pid": proc.pid,
        "project": name,
        "started": datetime.now(timezone.utc).isoformat(),
    }
    (project_dir / "running.json").write_text(json.dumps(running_data), encoding="utf-8")

    with _state_lock:
        log_lines_by_project: dict = _state["log_lines_by_project"]
        log_lines_by_project[name] = []
        _state.update(
            {
                "project": name,
                "process": proc,
                "log_file": log_file,
            }
        )
    threading.Thread(target=_read_logs, args=(proc, log_file, name), daemon=True).start()


# ---------------------------------------------------------------------------
# Log reader thread
# ---------------------------------------------------------------------------


def _read_logs(proc: subprocess.Popen, log_file, project: str) -> None:
    if proc.stdout is None:
        return
    for line in proc.stdout:
        log_file.write(line)
        with _state_lock:
            lines = _state["log_lines_by_project"].setdefault(project, [])
            lines.append(line)
            if len(lines) > 200:
                lines.pop(0)
        _notify_sse_subscribers(project, line)
    _notify_sse_subscribers(project, None)  # sentinel: process stdout closed
    _cleanup_after_exit(proc)


def _notify_sse_subscribers(project: str, line) -> None:
    with _sse_lock:
        subscribers = list(_sse_subscribers.get(project, []))
    for q in subscribers:
        try:
            q.put_nowait(line)
        except Exception:
            pass


def _cleanup_after_exit(proc: subprocess.Popen) -> None:
    with _state_lock:
        # Guard: don't clobber state if a new process was already started
        if _state["process"] is not proc:
            return
        lf = _state["log_file"]
        project = _state["project"]
        _state.update({"process": None, "log_file": None, "project": None})
    if lf:
        try:
            lf.close()
        except OSError:
            pass
    if project:
        rj = running_json_path(project)
        try:
            rj.unlink(missing_ok=True)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# SSE log stream
# ---------------------------------------------------------------------------


def handle_log_stream(handler, project_name: str) -> None:
    """Stream log output to the browser via Server-Sent Events.

    ``handler`` is the BaseHTTPRequestHandler instance (provides .wfile).
    Blocks until the client disconnects or the process exits.
    """
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.send_header("X-Accel-Buffering", "no")
    handler.end_headers()

    # Replay buffered lines first (reconnect support)
    with _state_lock:
        buffered = list(_state["log_lines_by_project"].get(project_name, []))
    for line in buffered:
        try:
            handler.wfile.write(f"data: {line.rstrip()}\n\n".encode("utf-8"))
            handler.wfile.flush()
        except OSError:
            return

    # Subscribe to live output
    q: queue.Queue = queue.Queue(maxsize=500)
    with _sse_lock:
        _sse_subscribers.setdefault(project_name, []).append(q)

    try:
        while True:
            try:
                line = q.get(timeout=15)
            except queue.Empty:
                # Keepalive heartbeat (SSE comment — ignored by EventSource)
                try:
                    handler.wfile.write(b": keepalive\n\n")
                    handler.wfile.flush()
                except OSError:
                    return
                continue

            if line is None:  # sentinel: process exited
                try:
                    handler.wfile.write(b"data: [process exited]\n\n")
                    handler.wfile.flush()
                except OSError:
                    pass
                return

            try:
                handler.wfile.write(f"data: {line.rstrip()}\n\n".encode("utf-8"))
                handler.wfile.flush()
            except OSError:
                return

    finally:
        with _sse_lock:
            try:
                subscribers = _sse_subscribers.get(project_name, [])
                subscribers.remove(q)
                if not subscribers:
                    _sse_subscribers.pop(project_name, None)
            except ValueError:
                pass
