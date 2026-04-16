"""Anolis Workbench — unified commissioning shell backend.

Run from any working directory:
    python /path/to/anolis/tools/workbench/backend/server.py
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Ensure tools/system-composer/ is importable as `backend`.
_WB_DIR = pathlib.Path(__file__).resolve().parent.parent
_SC_DIR = _WB_DIR.parent / "system-composer"
if str(_SC_DIR) not in sys.path:
    sys.path.insert(0, str(_SC_DIR))

from backend import launcher as launcher_module  # noqa: E402
from backend import paths as paths_module  # noqa: E402
from backend import projects as projects_module  # noqa: E402


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError:
        print(f"WARNING: Invalid integer for {name}: {raw!r}; using {default}", file=sys.stderr)
        return default


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    value = raw.strip().lower()
    if value in {"1", "true", "yes", "on"}:
        return True
    if value in {"0", "false", "no", "off"}:
        return False
    print(f"WARNING: Invalid boolean for {name}: {raw!r}; using {default}", file=sys.stderr)
    return default


HOST = os.getenv("ANOLIS_WORKBENCH_HOST", "127.0.0.1")
PORT = _env_int("ANOLIS_WORKBENCH_PORT", 3010)
OPERATOR_UI_BASE = os.getenv("ANOLIS_OPERATOR_UI_BASE", "http://localhost:3000").rstrip("/")
OPEN_BROWSER = _env_bool("ANOLIS_WORKBENCH_OPEN_BROWSER", True)
FRONTEND_DIR = _WB_DIR / "frontend"

_WORKSPACE_ROUTE_RE = re.compile(r"^/projects/[^/]+(?:/(?:compose|commission|operate))?/?$")

_MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css",
    ".js": "application/javascript",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
}

_HOP_BY_HOP_HEADERS = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailers",
    "transfer-encoding",
    "upgrade",
}


def verify_environment() -> None:
    if not FRONTEND_DIR.is_dir():
        print("ERROR: Workbench frontend directory not found.", file=sys.stderr)
        print(f"  Expected: {FRONTEND_DIR}", file=sys.stderr)
        sys.exit(1)
    if not paths_module.CATALOG_PATH.is_file():
        print("ERROR: Composer catalog not found.", file=sys.stderr)
        print(f"  Expected: {paths_module.CATALOG_PATH}", file=sys.stderr)
        sys.exit(1)


def _open_browser(url: str) -> None:
    time.sleep(0.3)
    webbrowser.open(url)


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # suppress noisy per-request logs
        pass

    # ------------------------------------------------------------------
    # HTTP verb dispatch
    # ------------------------------------------------------------------

    def do_GET(self) -> None:
        path = self.path.split("?")[0]
        if path == "/api/projects":
            self._list_projects()
        elif path.startswith("/api/projects/"):
            name, sub = self._parse_project_path(path)
            if sub is None:
                self._get_project(name)
            elif sub == "logs":
                self._log_stream(name)
            else:
                self._not_found()
        elif path == "/api/status":
            self._status()
        elif path == "/api/catalog":
            self._serve_catalog()
        elif path == "/api/templates":
            self._serve_templates()
        elif path.startswith("/v0/"):
            self._proxy_runtime("GET", self.path)
        else:
            self._serve_static(path)

    def do_POST(self) -> None:
        path = self.path.split("?")[0]
        if path == "/api/projects":
            self._create_project()
        elif path.startswith("/api/projects/"):
            name, sub = self._parse_project_path(path)
            if sub == "rename":
                self._rename_project(name)
            elif sub == "duplicate":
                self._duplicate_project(name)
            elif sub == "preflight":
                self._preflight(name)
            elif sub == "launch":
                self._launch_project(name)
            elif sub == "stop":
                self._stop_project(name)
            elif sub == "restart":
                self._restart_project(name)
            else:
                self._not_found()
        elif path.startswith("/v0/"):
            self._proxy_runtime("POST", self.path)
        else:
            self._not_found()

    def do_PUT(self) -> None:
        path = self.path.split("?")[0]
        if path.startswith("/api/projects/"):
            name, sub = self._parse_project_path(path)
            if sub is None:
                self._save_project(name)
            else:
                self._not_found()
        elif path.startswith("/v0/"):
            self._proxy_runtime("PUT", self.path)
        else:
            self._not_found()

    def do_DELETE(self) -> None:
        path = self.path.split("?")[0]
        if path.startswith("/api/projects/"):
            name, sub = self._parse_project_path(path)
            if sub is None:
                self._delete_project(name)
            else:
                self._not_found()
        elif path.startswith("/v0/"):
            self._proxy_runtime("DELETE", self.path)
        else:
            self._not_found()

    # ------------------------------------------------------------------
    # API handlers (composer-control semantics preserved)
    # ------------------------------------------------------------------

    def _list_projects(self) -> None:
        self._json(200, projects_module.list_projects())

    def _create_project(self) -> None:
        body = self._body_json()
        if body is None:
            return
        name = body.get("name") or ""
        template = body.get("template") or ""
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        if not template:
            self._json(400, {"error": "template required"})
            return
        try:
            system = projects_module.create_project_from_template(name, template)
            self._json(201, system)
        except projects_module.ProjectValidationError as exc:
            self._json(
                500,
                {
                    "error": "Template produced invalid system payload",
                    "code": "template_validation_failed",
                    "errors": exc.errors,
                },
            )
        except FileNotFoundError as exc:
            self._json(404, {"error": str(exc)})
        except ValueError as exc:
            self._json(409, {"error": str(exc)})

    def _get_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        try:
            self._json(200, projects_module.get_project(name))
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})

    def _save_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        system = self._body_json()
        if system is None:
            return
        try:
            projects_module.save_project(name, system)
            self._json(200, {"ok": True})
        except projects_module.ProjectValidationError as exc:
            self._json(
                400,
                {
                    "error": "Project validation failed",
                    "code": "validation_failed",
                    "errors": exc.errors,
                },
            )
        except Exception as exc:  # noqa: BLE001 - HTTP boundary
            self._json(500, {"error": str(exc)})

    def _rename_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        body = self._body_json()
        if body is None:
            return
        new_name = body.get("new_name") or ""
        err = projects_module.validate_name(new_name)
        if err:
            self._json(400, {"error": err or "new_name required"})
            return
        if projects_module.is_running(name):
            self._json(409, {"error": "Cannot rename a running project"})
            return
        try:
            projects_module.rename_project(name, new_name)
            self._json(200, {"ok": True})
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})
        except ValueError as exc:
            self._json(409, {"error": str(exc)})

    def _duplicate_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        body = self._body_json()
        if body is None:
            return
        new_name = body.get("new_name") or ""
        err = projects_module.validate_name(new_name)
        if err:
            self._json(400, {"error": err or "new_name required"})
            return
        try:
            system = projects_module.duplicate_project(name, new_name)
            self._json(201, system)
        except projects_module.ProjectValidationError as exc:
            self._json(
                500,
                {
                    "error": "Duplicated project failed validation",
                    "code": "duplicate_validation_failed",
                    "errors": exc.errors,
                },
            )
        except FileNotFoundError as exc:
            self._json(404, {"error": str(exc)})
        except ValueError as exc:
            self._json(409, {"error": str(exc)})

    def _delete_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        if projects_module.is_running(name):
            self._json(409, {"error": "Cannot delete a running project"})
            return
        try:
            projects_module.delete_project(name)
            self._json(200, {"ok": True})
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})

    def _status(self) -> None:
        status = launcher_module.get_status()
        status["composer"] = {
            "host": HOST,
            "port": PORT,
            "operator_ui_base": OPERATOR_UI_BASE,
        }
        status["workbench"] = {
            "version": 1,
        }
        self._json(200, status)

    def _preflight(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        try:
            system = projects_module.get_project(name)
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})
            return
        project_dir = projects_module.project_dir(name)
        result = launcher_module.preflight(name, system, project_dir)
        self._json(200, result)

    def _launch_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        try:
            system = projects_module.get_project(name)
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})
            return
        project_dir = projects_module.project_dir(name)
        try:
            launcher_module.launch(name, system, project_dir)
            self._json(200, {"ok": True})
        except RuntimeError as exc:
            self._json(409, {"error": str(exc)})
        except Exception as exc:  # noqa: BLE001 - HTTP boundary
            self._json(500, {"error": str(exc)})

    def _stop_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        try:
            projects_module.get_project(name)
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})
            return
        launcher_module.stop()
        self._json(200, {"ok": True})

    def _restart_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        try:
            projects_module.get_project(name)
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})
            return
        project_dir = projects_module.project_dir(name)
        try:
            launcher_module.restart(name, project_dir)
            self._json(200, {"ok": True})
        except RuntimeError as exc:
            self._json(409, {"error": str(exc)})
        except Exception as exc:  # noqa: BLE001 - HTTP boundary
            self._json(500, {"error": str(exc)})

    def _log_stream(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        try:
            projects_module.get_project(name)
        except FileNotFoundError:
            self._json(404, {"error": f"Project '{name}' not found"})
            return
        launcher_module.handle_log_stream(self, name)

    def _serve_catalog(self) -> None:
        path = paths_module.CATALOG_PATH
        if not path.exists():
            self._json(404, {"error": "Catalog not found"})
            return
        self._json(200, json.loads(path.read_text(encoding="utf-8")))

    def _serve_templates(self) -> None:
        tpl_root = paths_module.TEMPLATES_ROOT
        if not tpl_root.exists():
            self._json(200, [])
            return
        result = []
        for directory in sorted(tpl_root.iterdir()):
            if not directory.is_dir():
                continue
            sj = directory / "system.json"
            if not sj.exists():
                continue
            try:
                data = json.loads(sj.read_text(encoding="utf-8"))
                result.append({"id": directory.name, "meta": data.get("meta", {})})
            except (json.JSONDecodeError, OSError):
                pass
        self._json(200, result)

    # ------------------------------------------------------------------
    # Runtime proxy (/v0/*, /v0/events)
    # ------------------------------------------------------------------

    def _runtime_base(self) -> tuple[str | None, str | None, int]:
        status = launcher_module.get_status()
        active_project = status.get("active_project")
        if not status.get("running") or not isinstance(active_project, str) or active_project == "":
            return None, "Runtime is not running", 503

        try:
            system = projects_module.get_project(active_project)
        except FileNotFoundError:
            return None, f"Running project '{active_project}' not found", 500

        runtime_cfg = system.get("topology", {}).get("runtime", {})
        bind = runtime_cfg.get("http_bind", "127.0.0.1")
        port = runtime_cfg.get("http_port", 8080)

        if not isinstance(bind, str) or bind == "":
            bind = "127.0.0.1"
        if not isinstance(port, int):
            return None, "Runtime port is invalid", 500

        return f"http://{bind}:{port}", None, 200

    def _proxy_runtime(self, method: str, raw_path: str) -> None:
        runtime_base, error, status_code = self._runtime_base()
        if runtime_base is None:
            self._json(status_code, {"error": error or "Runtime unavailable"})
            return

        target_url = f"{runtime_base}{raw_path}"
        body: bytes | None = None
        content_length = int(self.headers.get("Content-Length", "0") or "0")
        if content_length > 0:
            body = self.rfile.read(content_length)

        headers: dict[str, str] = {}
        for key in ("Content-Type", "Accept", "Last-Event-ID"):
            value = self.headers.get(key)
            if value:
                headers[key] = value

        req = urllib.request.Request(target_url, data=body, headers=headers, method=method)
        wants_stream = raw_path.split("?")[0] == "/v0/events"

        try:
            if wants_stream:
                response_ctx = urllib.request.urlopen(req)
            else:
                response_ctx = urllib.request.urlopen(req, timeout=15)

            with response_ctx as response:
                self.send_response(int(response.status))
                for key, value in response.headers.items():
                    lower = key.lower()
                    if lower in _HOP_BY_HOP_HEADERS:
                        continue
                    if lower == "content-length" and wants_stream:
                        continue
                    self.send_header(key, value)
                self.end_headers()

                if wants_stream:
                    while True:
                        chunk = response.read(4096)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        self.wfile.flush()
                    return

                data = response.read()
                self.wfile.write(data)
        except urllib.error.HTTPError as exc:
            body_data = exc.read()
            self.send_response(int(exc.code))
            for key, value in exc.headers.items():
                lower = key.lower()
                if lower in _HOP_BY_HOP_HEADERS:
                    continue
                if lower == "content-length":
                    continue
                self.send_header(key, value)
            self.send_header("Content-Length", str(len(body_data)))
            self.end_headers()
            if body_data:
                self.wfile.write(body_data)
        except urllib.error.URLError as exc:
            self._json(502, {"error": f"Runtime proxy failed: {exc}"})

    # ------------------------------------------------------------------
    # Static file serving
    # ------------------------------------------------------------------

    def _serve_static(self, path: str) -> None:
        decoded = urllib.parse.unquote(path)
        if decoded == "/" or _WORKSPACE_ROUTE_RE.match(decoded):
            self._serve_index()
            return

        rel = decoded.lstrip("/")
        if ".." in rel:
            self._json(400, {"error": "Bad request"})
            return

        file_path = FRONTEND_DIR / rel
        if not file_path.is_file():
            self._json(404, {"error": "Not found"})
            return

        content_type = _MIME.get(file_path.suffix.lower(), "application/octet-stream")
        data = file_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_index(self) -> None:
        index_path = FRONTEND_DIR / "index.html"
        data = index_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _parse_project_path(path: str):
        """Return (name, sub) from /api/projects/<name>[/<sub>], sub may be None."""
        tail = path[len("/api/projects/") :]
        parts = tail.split("/", 1)
        name = parts[0]
        sub = parts[1] if len(parts) > 1 else None
        return name, sub

    def _body_json(self):
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            self._json(400, {"error": "Empty body"})
            return None
        if length > 1_048_576:  # 1 MiB max
            self._json(400, {"error": "Request body too large"})
            return None
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            self._json(400, {"error": "Invalid JSON"})
            return None

    def _json(self, status: int, data) -> None:
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _not_found(self) -> None:
        self._json(404, {"error": "Not found"})


def main() -> None:
    verify_environment()
    projects_module.cleanup_stale_running_files()
    server = ThreadingHTTPServer((HOST, PORT), _Handler)
    url = f"http://{HOST}:{PORT}"
    print(f"Anolis Workbench is running at {url}")
    print("Close this window to stop.")
    if OPEN_BROWSER:
        threading.Thread(target=_open_browser, args=(url,), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
