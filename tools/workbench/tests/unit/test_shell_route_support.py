"""Route and shell contract tests for tools/workbench."""

from __future__ import annotations

import json
import os
import pathlib
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

import pytest

_WB_DIR = pathlib.Path(__file__).resolve().parents[2]
_SERVER_SCRIPT = _WB_DIR / "backend" / "server.py"
_REPO_ROOT = _WB_DIR.parent.parent
_SYSTEMS_ROOT = _REPO_ROOT / "systems"


def _pick_free_port() -> int:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            return int(sock.getsockname()[1])
    except PermissionError as exc:
        pytest.skip(f"Socket creation is not permitted in this environment: {exc}")


def _http_json(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    body: dict[str, Any] | None = None,
    timeout_s: float = 5.0,
) -> tuple[int, dict[str, Any]]:
    payload = None
    headers: dict[str, str] = {}
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(
        f"{base_url}{path}",
        data=payload,
        headers=headers,
        method=method,
    )

    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            raw = response.read().decode("utf-8")
            data: dict[str, Any] = json.loads(raw) if raw else {}
            return int(response.status), data
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8")
        data = json.loads(raw) if raw else {}
        return int(exc.code), data


def _http_text(base_url: str, path: str, timeout_s: float = 5.0) -> tuple[int, str, str]:
    request = urllib.request.Request(f"{base_url}{path}", method="GET")
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        raw = response.read().decode("utf-8")
        content_type = response.headers.get("Content-Type", "")
        return int(response.status), raw, content_type


def _wait_for_ready(base_url: str, proc: subprocess.Popen[str], timeout_s: float = 10.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            stderr = proc.stderr.read() if proc.stderr is not None else ""
            raise AssertionError(
                f"Workbench exited before readiness check (code={proc.returncode}):\n{stderr}"
            )

        try:
            status, _ = _http_json(base_url, "/api/status", timeout_s=0.5)
            if status == 200:
                return
        except Exception:
            pass

        time.sleep(0.1)

    raise AssertionError("Workbench readiness timeout")


@pytest.fixture
def workbench_server(tmp_path: pathlib.Path) -> dict[str, Any]:
    port = _pick_free_port()
    env = os.environ.copy()
    env["ANOLIS_WORKBENCH_HOST"] = "127.0.0.1"
    env["ANOLIS_WORKBENCH_PORT"] = str(port)
    env["ANOLIS_WORKBENCH_OPEN_BROWSER"] = "0"

    proc = subprocess.Popen(
        [sys.executable, str(_SERVER_SCRIPT)],
        cwd=str(tmp_path),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )

    base_url = f"http://127.0.0.1:{port}"
    _wait_for_ready(base_url, proc)

    try:
        yield {"base_url": base_url, "port": port}
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def test_shell_routes_resolve_to_workbench_index(workbench_server: dict[str, Any]) -> None:
    base_url = workbench_server["base_url"]

    created_code, created_payload = _http_json(
        base_url,
        "/api/projects",
        method="POST",
        body={"name": "wb-route-test", "template": "sim-quickstart"},
    )
    assert created_code == 201, created_payload

    try:
        for path in (
            "/",
            "/projects/wb-route-test",
            "/projects/wb-route-test/compose",
            "/projects/wb-route-test/commission",
            "/projects/wb-route-test/operate",
        ):
            code, body, content_type = _http_text(base_url, path)
            assert code == 200, (path, code)
            assert "Anolis Workbench" in body, path
            assert "text/html" in content_type, (path, content_type)
    finally:
        _http_json(base_url, "/api/projects/wb-route-test", method="DELETE")


def test_status_and_static_assets_are_served(workbench_server: dict[str, Any]) -> None:
    base_url = workbench_server["base_url"]

    status_code, status_payload = _http_json(base_url, "/api/status")
    assert status_code == 200
    assert status_payload.get("version") == 1
    assert isinstance(status_payload.get("composer"), dict)
    assert status_payload["composer"].get("port") == workbench_server["port"]

    asset_code, asset_body, content_type = _http_text(base_url, "/js/app.js")
    assert asset_code == 200
    assert "application/javascript" in content_type
    assert "operate-workspace" in asset_body


def test_unknown_static_path_returns_not_found(workbench_server: dict[str, Any]) -> None:
    base_url = workbench_server["base_url"]
    status_code, payload = _http_json(base_url, "/missing.asset")
    assert status_code == 404
    assert payload.get("error") == "Not found"


def test_invalid_project_name_is_rejected(workbench_server: dict[str, Any]) -> None:
    base_url = workbench_server["base_url"]
    bad_name = urllib.parse.quote("bad$name", safe="")
    status, payload = _http_json(
        base_url,
        f"/api/projects/{bad_name}/preflight",
        method="POST",
        body={},
    )
    assert status == 400
    assert "Project name must be" in str(payload.get("error"))


def test_runtime_proxy_returns_503_when_runtime_stopped(workbench_server: dict[str, Any]) -> None:
    base_url = workbench_server["base_url"]
    status, payload = _http_json(base_url, "/v0/mode")
    assert status == 503
    assert "Runtime is not running" in str(payload.get("error"))


def test_launch_is_hard_blocked_when_another_project_is_running(workbench_server: dict[str, Any]) -> None:
    base_url = workbench_server["base_url"]
    requested = "wb-launch-requested"
    running = "wb-launch-running"

    for name in (requested, running):
        status, payload = _http_json(
            base_url,
            "/api/projects",
            method="POST",
            body={"name": name, "template": "sim-quickstart"},
        )
        assert status == 201, payload

    runner = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(60)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    running_path = _SYSTEMS_ROOT / running / "running.json"
    running_path.write_text(
        json.dumps({"pid": runner.pid, "project": running, "started": time.time()}),
        encoding="utf-8",
    )

    try:
        launch_status, launch_payload = _http_json(
            base_url,
            f"/api/projects/{urllib.parse.quote(requested)}/launch",
            method="POST",
            body={},
        )
        assert launch_status == 409, launch_payload
        assert "already running" in str(launch_payload.get("error")).lower()
    finally:
        _http_json(
            base_url,
            f"/api/projects/{urllib.parse.quote(running)}/stop",
            method="POST",
            body={},
        )
        if runner.poll() is None:
            runner.terminate()
            try:
                runner.wait(timeout=5)
            except subprocess.TimeoutExpired:
                runner.kill()
                runner.wait(timeout=5)
        for name in (requested, running):
            _http_json(base_url, f"/api/projects/{urllib.parse.quote(name)}", method="DELETE")
