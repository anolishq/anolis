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

    # Catalog should not advertise custom provider support in v1
    catalog, sc = get("/api/catalog")
    assert sc == 200
    provider_kinds = {entry.get("kind") for entry in catalog.get("providers", [])}
    assert "custom" not in provider_kinds, provider_kinds
    print("GET /api/catalog      OK  (custom provider hidden in v1)")

    # Save should reject custom provider kind explicitly
    custom_payload = dict(sys_obj)
    custom_payload["topology"] = dict(sys_obj.get("topology", {}))
    custom_payload["topology"]["runtime"] = dict(sys_obj["topology"].get("runtime", {}))
    custom_payload["topology"]["providers"] = dict(sys_obj["topology"].get("providers", {}))
    custom_payload["paths"] = dict(sys_obj.get("paths", {}))
    custom_payload["paths"]["providers"] = dict(sys_obj["paths"].get("providers", {}))

    custom_payload["topology"]["runtime"]["providers"] = list(sys_obj["topology"]["runtime"].get("providers", [])) + [
        {
            "id": "custom0",
            "kind": "custom",
            "timeout_ms": 5000,
            "hello_timeout_ms": 2000,
            "ready_timeout_ms": 10000,
            "restart_policy": {"enabled": False},
        }
    ]
    custom_payload["topology"]["providers"]["custom0"] = {"kind": "custom", "args": ["--foo", "bar"]}
    custom_payload["paths"]["providers"]["custom0"] = {"executable": "../custom-provider/build/provider"}
    req = urllib.request.Request(
        "http://localhost:3002/api/projects/smoke-p4",
        data=json.dumps(custom_payload).encode(),
        headers={"Content-Type": "application/json"},
        method="PUT",
    )
    try:
        with urllib.request.urlopen(req):
            raise AssertionError("Expected HTTP 400 for unsupported custom provider kind")
    except urllib.error.HTTPError as exc:
        assert exc.code == 400
        body = json.loads(exc.read())
        assert body.get("code") == "validation_failed", body
        messages = [str(err.get("message", "")) for err in body.get("errors", [])]
        assert any("not supported by Composer contract v1" in msg for msg in messages), body
        print("PUT /api/projects    OK  (custom provider rejected in v1)")

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
