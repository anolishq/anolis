"""launcher.py — Process management for the Anolis System Composer.

Preflight checks, process launch, stop, restart, and SSE log streaming.
All public functions are called from server.py HTTP handlers.
"""

import json
import pathlib
import queue
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

_CATALOG_PATH = pathlib.Path("tools/system-composer/catalog/providers.json")
_SYSTEMS_DIR = pathlib.Path("systems")

_state: dict = {
    "project": None,    # active project name
    "process": None,    # subprocess.Popen handle
    "log_file": None,   # open file handle for latest.log
    "log_lines": [],    # ring buffer (last 200 lines) for SSE reconnect
}
_state_lock = threading.Lock()

_sse_subscribers: list = []   # list[queue.Queue]
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
    return _SYSTEMS_DIR / name / "running.json"


# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------

def get_status() -> dict:
    """Return serialisable status dict for GET /api/status."""
    with _state_lock:
        proc = _state["process"]
        running = proc is not None and proc.poll() is None
        project = _state["project"] if running else None
        pid = proc.pid if running else None
    return {
        "version": 1,
        "active_project": project,
        "running": running,
        "pid": pid,
    }


# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

def preflight(name: str, system: dict, project_dir: pathlib.Path) -> dict:
    """
    Run preflight checks and return {"ok": bool, "checks": [...]}.
    Re-renders YAML to disk before running binary checks.
    """
    from backend import renderer      # local import avoids any circular at import time
    from backend import validator

    checks: list[dict] = []
    catalog = _load_catalog()

    # Re-render YAML to disk first so --check-config sees current state
    try:
        renders = renderer.render(system, name)
        for rel_path, content in renders.items():
            out = project_dir / rel_path
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(content, encoding="utf-8")
    except Exception as exc:
        checks.append({
            "name": "Render YAML to disk",
            "ok": False,
            "error": str(exc),
            "hint": None,
        })
        return {"ok": False, "checks": checks}

    # Check 1: Runtime binary
    runtime_exe = pathlib.Path(system["paths"]["runtime_executable"])
    _exists_check(checks, "Runtime binary exists", runtime_exe,
                  hint=_build_binary_hint(runtime_exe, kind="runtime", repo="anolis", docs="docs/"))

    # Check 2: Provider binaries
    providers = system.get("topology", {}).get("providers", {})
    for pid, pcfg in providers.items():
        exe = pathlib.Path(system["paths"]["providers"][pid]["executable"])
        kind = pcfg.get("kind", "")
        kind_info = catalog.get(kind, {})
        repo = kind_info.get("repo")
        docs = kind_info.get("build_docs")
        _exists_check(checks, f"Provider {pid} binary exists", exe,
                      hint=_build_binary_hint(exe, kind=kind, repo=repo, docs=docs))

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
            checks.append({"name": "System-level validation", "ok": False,
                           "error": err, "hint": None})
    else:
        checks.append({"name": "System-level validation", "ok": True,
                       "error": None, "hint": None})

    # Check 5: Runtime --check-config
    checks.append(_check_config_binary(
        "Runtime --check-config",
        runtime_exe,
        project_dir / "anolis-runtime.yaml",
    ))

    # Check 6+: Provider --check-config (only for kinds with check_config_flag)
    for pid, pcfg in providers.items():
        kind = pcfg.get("kind", "")
        if not catalog.get(kind, {}).get("check_config_flag"):
            continue
        exe = pathlib.Path(system["paths"]["providers"][pid]["executable"])
        yaml_path = project_dir / "providers" / f"{pid}.yaml"
        checks.append(_check_config_binary(
            f"Provider {pid} --check-config", exe, yaml_path,
        ))

    ok = all(c.get("ok") is not False for c in checks)
    return {"ok": ok, "checks": checks}


# ---------------------------------------------------------------------------
# Platform-aware build hints
# ---------------------------------------------------------------------------

# Preset names per kind, keyed by platform ('win32' or 'other')
_PRESETS: dict[str, dict[str, str]] = {
    "win32": {
        "runtime": "dev-windows-release",
        "sim":     "dev-windows-release",
        "bread":   "dev-linux-hardware-release",
        "ezo":     "dev-linux-hardware-release",
        "custom":  "dev-release",
    },
    "other": {
        "runtime": "dev-release",
        "sim":     "dev-release",
        "bread":   "dev-linux-hardware-release",
        "ezo":     "dev-linux-hardware-release",
        "custom":  "dev-release",
    },
}


def _build_binary_hint(exe: pathlib.Path, kind: str,
                       repo: str | None, docs: str | None) -> str:
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
    return {"name": f"Runtime port {port} available", "ok": True,
            "error": None, "hint": None}


def _exists_check(checks: list, name: str, path: pathlib.Path,
                  hint: str | None = None) -> None:
    exists = path.exists()
    checks.append({
        "name": name,
        "ok": exists,
        "error": None if exists else f"File not found: {path}",
        "hint": hint if not exists else None,
    })


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


def _check_config_binary(check_name: str, exe: pathlib.Path,
                         yaml_path: pathlib.Path) -> dict:
    if not exe.exists():
        return {"name": check_name, "ok": None, "note": "Binary missing — skipped"}
    if not yaml_path.exists():
        return {"name": check_name, "ok": None, "note": "Config not yet rendered"}
    try:
        result = subprocess.run(
            [str(exe), "--check-config", str(yaml_path)],
            capture_output=True, text=True, timeout=10,
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
    from backend import renderer

    with _state_lock:
        proc = _state["process"]
        if proc is not None and proc.poll() is None:
            raise RuntimeError("A system is already running.")

    # Re-render YAML to disk
    renders = renderer.render(system, name)
    for rel_path, content in renders.items():
        out = project_dir / rel_path
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(content, encoding="utf-8")

    # Prepare log file (overwrite)
    log_path = project_dir / "logs" / "latest.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = open(log_path, "w", buffering=1, encoding="utf-8")  # noqa: WPS515

    # Build command
    runtime_exe = str(system["paths"]["runtime_executable"])
    runtime_config = f"systems/{name}/anolis-runtime.yaml"
    cmd = [runtime_exe, "--config", runtime_config]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(pathlib.Path.cwd()),
        bufsize=1,
        text=True,
    )

    # Write running.json
    running_data = {
        "pid": proc.pid,
        "project": name,
        "started": datetime.now(timezone.utc).isoformat(),
    }
    (project_dir / "running.json").write_text(
        json.dumps(running_data), encoding="utf-8"
    )

    with _state_lock:
        _state.update({
            "project": name,
            "process": proc,
            "log_file": log_file,
            "log_lines": [],
        })
    threading.Thread(
        target=_read_logs, args=(proc, log_file), daemon=True
    ).start()


# ---------------------------------------------------------------------------
# Stop
# ---------------------------------------------------------------------------

def stop() -> None:
    """Gracefully terminate the running process."""
    with _state_lock:
        proc = _state["process"]
    if proc is None or proc.poll() is not None:
        return  # Nothing to stop
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ---------------------------------------------------------------------------
# Restart
# ---------------------------------------------------------------------------

def restart(name: str, project_dir: pathlib.Path) -> None:
    """Kill and relaunch using the already-rendered YAML on disk.

    Does NOT re-render from current system.json — uses the YAML already on disk.
    """
    stop()
    time.sleep(0.5)  # Allow log reader thread to flush and clean up

    system = json.loads((project_dir / "system.json").read_text(encoding="utf-8"))

    log_path = project_dir / "logs" / "latest.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = open(log_path, "w", buffering=1, encoding="utf-8")  # noqa: WPS515

    runtime_exe = str(system["paths"]["runtime_executable"])
    runtime_config = f"systems/{name}/anolis-runtime.yaml"
    cmd = [runtime_exe, "--config", runtime_config]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(pathlib.Path.cwd()),
        bufsize=1,
        text=True,
    )

    running_data = {
        "pid": proc.pid,
        "project": name,
        "started": datetime.now(timezone.utc).isoformat(),
    }
    (project_dir / "running.json").write_text(
        json.dumps(running_data), encoding="utf-8"
    )

    with _state_lock:
        _state.update({
            "project": name,
            "process": proc,
            "log_file": log_file,
            "log_lines": [],
        })
    threading.Thread(
        target=_read_logs, args=(proc, log_file), daemon=True
    ).start()


# ---------------------------------------------------------------------------
# Log reader thread
# ---------------------------------------------------------------------------

def _read_logs(proc: subprocess.Popen, log_file) -> None:
    for line in proc.stdout:
        log_file.write(line)
        with _state_lock:
            _state["log_lines"].append(line)
            if len(_state["log_lines"]) > 200:
                _state["log_lines"].pop(0)
        _notify_sse_subscribers(line)
    _notify_sse_subscribers(None)   # sentinel: process stdout closed
    _cleanup_after_exit(proc)


def _notify_sse_subscribers(line) -> None:
    with _sse_lock:
        for q in list(_sse_subscribers):
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

def handle_log_stream(handler) -> None:
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
        buffered = list(_state["log_lines"])
    for line in buffered:
        try:
            handler.wfile.write(f"data: {line.rstrip()}\n\n".encode("utf-8"))
            handler.wfile.flush()
        except OSError:
            return

    # Subscribe to live output
    q: queue.Queue = queue.Queue(maxsize=500)
    with _sse_lock:
        _sse_subscribers.append(q)

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

            if line is None:   # sentinel: process exited
                try:
                    handler.wfile.write(b"data: [process exited]\n\n")
                    handler.wfile.flush()
                except OSError:
                    pass
                return

            try:
                handler.wfile.write(
                    f"data: {line.rstrip()}\n\n".encode("utf-8")
                )
                handler.wfile.flush()
            except OSError:
                return

    finally:
        with _sse_lock:
            try:
                _sse_subscribers.remove(q)
            except ValueError:
                pass
