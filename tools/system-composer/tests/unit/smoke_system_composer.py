"""System Composer smoke test — run from the anolis repo root."""

import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

proc = subprocess.Popen(
    [sys.executable, "tools/system-composer/backend/server.py"],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.PIPE,
)
time.sleep(1.5)


def get(path):
    with urllib.request.urlopen(f"http://localhost:3002{path}") as r:
        return json.loads(r.read()), r.status


def post(path, body=None):
    data = json.dumps(body or {}).encode()
    req = urllib.request.Request(
        f"http://localhost:3002{path}",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read()), r.status


def put(path, body=None):
    data = json.dumps(body or {}).encode()
    req = urllib.request.Request(
        f"http://localhost:3002{path}",
        data=data,
        headers={"Content-Type": "application/json"},
        method="PUT",
    )
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read()), r.status


def delete(path):
    req = urllib.request.Request(f"http://localhost:3002{path}", method="DELETE")
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


try:
    # /api/status extended (task 4.14)
    status, _ = get("/api/status")
    assert status["version"] == 1
    assert "active_project" in status
    assert "running" in status
    assert "pid" in status
    print(f"GET /api/status       OK  running={status['running']}")

    # Create project
    sys_obj, sc = post("/api/projects", {"name": "smoke-p4", "template": "sim-quickstart"})
    assert sc == 201
    print("POST /api/projects    OK")

    # Save validation error should be structured and deterministic
    invalid_payload = {"schema_version": 1}
    req = urllib.request.Request(
        "http://localhost:3002/api/projects/smoke-p4",
        data=json.dumps(invalid_payload).encode(),
        headers={"Content-Type": "application/json"},
        method="PUT",
    )
    try:
        with urllib.request.urlopen(req):
            raise AssertionError("Expected HTTP 400 for invalid payload")
    except urllib.error.HTTPError as exc:
        assert exc.code == 400
        body = json.loads(exc.read())
        assert body.get("code") == "validation_failed", body
        assert isinstance(body.get("errors"), list) and body["errors"], body
        print("PUT /api/projects    OK  (validation failure is structured)")

    # Save valid payload still succeeds
    _, sc = put("/api/projects/smoke-p4", sys_obj)
    assert sc == 200
    print("PUT /api/projects    OK  (valid payload)")

    # Preflight (task 4.5)
    pf, sc = post("/api/projects/smoke-p4/preflight")
    assert sc == 200, f"Expected 200, got {sc}"
    assert "ok" in pf
    assert "checks" in pf
    assert isinstance(pf["checks"], list)
    assert len(pf["checks"]) > 0
    names = [c["name"] for c in pf["checks"]]
    assert "Runtime binary exists" in names, f"checks: {names}"
    assert "System-level validation" in names, f"checks: {names}"
    print(f"POST /preflight       OK  ok={pf['ok']} checks={len(pf['checks'])}")

    # Launch — binary won't exist, should return error gracefully
    req = urllib.request.Request(
        "http://localhost:3002/api/projects/smoke-p4/launch",
        data=b"{}",
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req) as r:
            ldata = json.loads(r.read())
        print(f"POST /launch          OK  (ok={ldata.get('ok')})")
    except urllib.error.HTTPError as exc:
        body = json.loads(exc.read())
        print(f"POST /launch          OK  (expected error: {body.get('error', '?')[:60]})")

    # Stop (no-op when nothing running) (task 4.7)
    stop_data, sc = post("/api/projects/smoke-p4/stop")
    assert sc == 200
    print("POST /stop            OK  (no-op)")

    # New frontend files served (tasks 4.10-4.12)
    with urllib.request.urlopen("http://localhost:3002/js/launch.js") as r:
        ct = r.headers.get("Content-Type")
        src = r.read().decode()
    assert "javascript" in ct
    assert "restoreRunningState" in src
    print("GET /js/launch.js     OK")

    with urllib.request.urlopen("http://localhost:3002/js/log-pane.js") as r:
        src = r.read().decode()
    assert "connect" in src
    print("GET /js/log-pane.js   OK")

    with urllib.request.urlopen("http://localhost:3002/js/health.js") as r:
        src = r.read().decode()
    assert "startPolling" in src
    print("GET /js/health.js     OK")

    # Session persistence endpoint
    with urllib.request.urlopen("http://localhost:3002/js/app.js") as r:
        src = r.read().decode()
    assert "restoreRunningState" in src
    print("GET /js/app.js        OK  (session persistence present)")

    # Cleanup
    delete("/api/projects/smoke-p4")

    print()
    print("All System Composer smoke tests passed.")

finally:
    proc.terminate()
