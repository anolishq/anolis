"""Bearer auth on loopback, through a running runtime.

With http.auth_enabled true, a request from this host needs the token unless
http.auth_exempt_loopback is set to true explicitly.
"""

from __future__ import annotations

import time
from pathlib import Path

import requests

from tests.support.runtime_fixture import RuntimeFixture

FAKE_TOKEN = "fake-test-token-not-a-secret"
STATUS = "/v0/runtime/status"


UI_ORIGIN = "http://ui.example.test"
OTHER_ORIGIN = "https://evil.example"
CORS_HEADERS = (
    "Access-Control-Allow-Origin",
    "Access-Control-Allow-Methods",
    "Access-Control-Allow-Headers",
    "Access-Control-Allow-Credentials",
)


def _config(provider_path: Path, port: int, *, exempt: bool | None, origins: list[str] | None = None) -> dict:
    fixture_config = Path(__file__).parent / "fixtures" / "provider-sim-default.yaml"
    http: dict = {
        "enabled": True,
        "bind": "127.0.0.1",
        "port": port,
        "auth_enabled": True,
        "auth_token": FAKE_TOKEN,
    }
    if exempt is not None:
        http["auth_exempt_loopback"] = exempt
    if origins is not None:
        http["cors_allowed_origins"] = origins
    return {
        "runtime": {},
        "http": http,
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_path).replace("\\", "/"),
                "args": ["--config", str(fixture_config).replace("\\", "/")],
                "timeout_ms": 5000,
            }
        ],
        "polling": {"interval_ms": 500},
        "telemetry": {"enabled": False},
    }


def _bearer(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def _start(
    runtime_path: Path, provider_path: Path, port: int, *, exempt: bool | None, origins: list[str] | None = None
) -> RuntimeFixture:
    fixture = RuntimeFixture(
        runtime_path,
        provider_path,
        http_port=port,
        config_dict=_config(provider_path, port, exempt=exempt, origins=origins),
    )
    assert fixture.start(), "Runtime failed to start"
    deadline = time.time() + 30.0
    while time.time() < deadline:
        assert fixture.is_running(), "Runtime exited:\n" + _output(fixture)
        try:
            if requests.get(f"{fixture.base_url}{STATUS}", headers=_bearer(FAKE_TOKEN), timeout=1.0).status_code == 200:
                return fixture
        except requests.exceptions.RequestException:
            pass
        time.sleep(0.2)
    fixture.cleanup()
    raise AssertionError("Runtime HTTP never answered 200 with the token")


def _output(fixture: RuntimeFixture) -> str:
    capture = fixture.get_output_capture()
    return capture.get_all_output() if capture is not None else "(no output capture)"


def test_loopback_requires_token_by_default(runtime_path: Path, provider_path: Path, port: int) -> None:
    fixture = _start(runtime_path, provider_path, port, exempt=None)
    try:
        url = f"{fixture.base_url}{STATUS}"
        no_token = requests.get(url, timeout=5.0)
        assert no_token.status_code == 401, f"loopback without a token: {no_token.status_code} {no_token.text}"
        assert no_token.headers.get("WWW-Authenticate") == "Bearer"

        wrong = requests.get(url, headers=_bearer("wrong-token"), timeout=5.0)
        assert wrong.status_code == 401, f"loopback with a wrong token: {wrong.status_code}"

        good = requests.get(url, headers=_bearer(FAKE_TOKEN), timeout=5.0)
        assert good.status_code == 200, f"loopback with the token: {good.status_code} {good.text}"

        output = _output(fixture)
        assert "auth_exempt_loopback" not in output, output
        assert FAKE_TOKEN not in output, "token leaked into the runtime log"
    finally:
        fixture.cleanup()


def test_explicit_exemption_still_exempts_and_warns(runtime_path: Path, provider_path: Path, port: int) -> None:
    fixture = _start(runtime_path, provider_path, port, exempt=True)
    try:
        resp = requests.get(f"{fixture.base_url}{STATUS}", timeout=5.0)
        assert resp.status_code == 200, f"exempt loopback without a token: {resp.status_code} {resp.text}"

        output = _output(fixture)
        warn = [line for line in output.splitlines() if "WARN" in line and "auth_exempt_loopback" in line]
        assert warn, "expected a startup WARN naming auth_exempt_loopback:\n" + output
        assert FAKE_TOKEN not in output, "token leaked into the runtime log"
    finally:
        fixture.cleanup()


def _header_lines(resp: requests.Response, name: str) -> list[str]:
    """Each line of a response header as sent; `resp.headers` joins repeats."""
    return list(resp.raw.headers.getlist(name))


def test_cross_origin_browser_client_with_auth(runtime_path: Path, provider_path: Path, port: int) -> None:
    """#460: a page on an allowed origin calls the authenticated runtime as a browser would."""
    fixture = _start(runtime_path, provider_path, port, exempt=None, origins=[UI_ORIGIN])
    try:
        base = fixture.base_url

        # The preflight carries no token and must not be refused for it.
        preflight = requests.options(
            f"{base}/v0/call",
            headers={
                "Origin": UI_ORIGIN,
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "authorization, content-type",
            },
            timeout=5.0,
        )
        assert preflight.status_code == 204, f"preflight: {preflight.status_code} {preflight.text}"
        assert preflight.content == b"", preflight.text
        assert _header_lines(preflight, "Access-Control-Allow-Origin") == [UI_ORIGIN]
        assert len(_header_lines(preflight, "Access-Control-Allow-Methods")) == 1
        allow_headers = _header_lines(preflight, "Access-Control-Allow-Headers")
        assert len(allow_headers) == 1, allow_headers
        assert "authorization" in [h.strip().lower() for h in allow_headers[0].split(",")], allow_headers

        # The request the preflight allowed.
        good = requests.get(
            f"{base}/v0/devices",
            headers={"Origin": UI_ORIGIN, "Authorization": f"Bearer {FAKE_TOKEN}"},
            timeout=5.0,
        )
        assert good.status_code == 200, f"authenticated cross-origin GET: {good.status_code} {good.text}"
        assert "devices" in good.json()
        assert _header_lines(good, "Access-Control-Allow-Origin") == [UI_ORIGIN]

        # A refusal the page can read.
        refused = requests.get(f"{base}/v0/devices", headers={"Origin": UI_ORIGIN}, timeout=5.0)
        assert refused.status_code == 401, f"cross-origin GET without a token: {refused.status_code}"
        assert _header_lines(refused, "Access-Control-Allow-Origin") == [UI_ORIGIN]

        # Another origin's preflight is still refused, with nothing to act on.
        other = requests.options(
            f"{base}/v0/call",
            headers={"Origin": OTHER_ORIGIN, "Access-Control-Request-Method": "POST"},
            timeout=5.0,
        )
        assert other.status_code == 401, f"preflight from another origin: {other.status_code} {other.text}"
        assert other.json() == {"error": "unauthorized"}
        for name in CORS_HEADERS:
            assert name not in other.headers, f"{name}: {other.headers[name]}"

        # An OPTIONS that is not a preflight still needs the token.
        bare = requests.options(f"{base}/v0/call", headers={"Origin": UI_ORIGIN}, timeout=5.0)
        assert bare.status_code == 401, f"OPTIONS without Access-Control-Request-Method: {bare.status_code}"
    finally:
        fixture.cleanup()
