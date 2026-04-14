"""Anolis System Composer — local HTTP backend.

Run from the anolis repo root:
    python tools/system-composer/backend/server.py
"""

import json
import pathlib
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Add tools/system-composer/ to sys.path so 'backend' is importable as a package.
_SC_DIR = str(pathlib.Path(__file__).parent.parent)
if _SC_DIR not in sys.path:
    sys.path.insert(0, _SC_DIR)

from backend import launcher as launcher_module  # noqa: E402
from backend import projects as projects_module  # noqa: E402

HOST = "127.0.0.1"
PORT = 3002
FRONTEND_DIR = pathlib.Path("tools/system-composer/frontend")

_MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css",
    ".js": "application/javascript",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def verify_repo_root() -> None:
    marker = pathlib.Path("tools/system-composer")
    if not marker.is_dir():
        print("ERROR: Must be run from the anolis repo root.", file=sys.stderr)
        print("  Expected to find: tools/system-composer/", file=sys.stderr)
        print("  Current CWD: " + str(pathlib.Path.cwd()), file=sys.stderr)
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
        else:
            self._not_found()

    # ------------------------------------------------------------------
    # API handlers
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
        except projects_module.ProjectValidationError as e:
            self._json(
                500,
                {
                    "error": "Template produced invalid system payload",
                    "code": "template_validation_failed",
                    "errors": e.errors,
                },
            )
        except FileNotFoundError as e:
            self._json(404, {"error": str(e)})
        except ValueError as e:
            self._json(409, {"error": str(e)})

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
        except Exception as e:
            self._json(500, {"error": str(e)})

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
        except ValueError as e:
            self._json(409, {"error": str(e)})

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
        except projects_module.ProjectValidationError as e:
            self._json(
                500,
                {
                    "error": "Duplicated project failed validation",
                    "code": "duplicate_validation_failed",
                    "errors": e.errors,
                },
            )
        except FileNotFoundError as e:
            self._json(404, {"error": str(e)})
        except ValueError as e:
            self._json(409, {"error": str(e)})

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
        self._json(200, launcher_module.get_status())

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
        except Exception as exc:
            self._json(500, {"error": str(exc)})

    def _stop_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        launcher_module.stop()
        self._json(200, {"ok": True})

    def _restart_project(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        project_dir = projects_module.project_dir(name)
        try:
            launcher_module.restart(name, project_dir)
            self._json(200, {"ok": True})
        except Exception as exc:
            self._json(500, {"error": str(exc)})

    def _log_stream(self, name: str) -> None:
        err = projects_module.validate_name(name)
        if err:
            self._json(400, {"error": err})
            return
        launcher_module.handle_log_stream(self)

    def _serve_catalog(self) -> None:
        path = pathlib.Path("tools/system-composer/catalog/providers.json")
        if not path.exists():
            self._json(404, {"error": "Catalog not found"})
            return
        self._json(200, json.loads(path.read_text(encoding="utf-8")))

    def _serve_templates(self) -> None:
        tpl_root = pathlib.Path("tools/system-composer/templates")
        if not tpl_root.exists():
            self._json(200, [])
            return
        result = []
        for d in sorted(tpl_root.iterdir()):
            if not d.is_dir():
                continue
            sj = d / "system.json"
            if not sj.exists():
                continue
            try:
                data = json.loads(sj.read_text(encoding="utf-8"))
                result.append({"id": d.name, "meta": data.get("meta", {})})
            except (json.JSONDecodeError, OSError):
                pass
        self._json(200, result)

    # ------------------------------------------------------------------
    # Static file serving
    # ------------------------------------------------------------------

    def _serve_static(self, path: str) -> None:
        if path == "/":
            path = "/index.html"
        rel = path.lstrip("/")
        # Reject path traversal
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
    verify_repo_root()
    projects_module.cleanup_stale_running_files()
    server = ThreadingHTTPServer((HOST, PORT), _Handler)
    url = f"http://localhost:{PORT}"
    print(f"Anolis System Composer is running at {url}")
    print("Close this window to stop.")
    threading.Thread(target=_open_browser, args=(url,), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
