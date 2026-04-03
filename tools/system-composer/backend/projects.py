"""Project CRUD and filesystem helpers for Anolis System Composer."""
import json
import os
import pathlib
import re
import shutil
from datetime import datetime, timezone

from backend import renderer

SYSTEMS_ROOT = pathlib.Path("tools/system-composer/systems")
TEMPLATES_ROOT = pathlib.Path("tools/system-composer/templates")

NAME_RE = re.compile(r'^[a-zA-Z0-9_-]{1,64}$')


def validate_name(name: str) -> "str | None":
    if not NAME_RE.match(name or ""):
        return "Project name must be 1-64 characters: letters, digits, hyphens, underscores only."
    return None


def project_dir(name: str) -> pathlib.Path:
    return SYSTEMS_ROOT / name


def system_json_path(name: str) -> pathlib.Path:
    return project_dir(name) / "system.json"


def runtime_yaml_path(name: str) -> pathlib.Path:
    return project_dir(name) / "anolis-runtime.yaml"


def provider_yaml_path(name: str, provider_id: str) -> pathlib.Path:
    return project_dir(name) / "providers" / f"{provider_id}.yaml"


def running_json_path(name: str) -> pathlib.Path:
    return project_dir(name) / "running.json"


def log_path(name: str) -> pathlib.Path:
    return project_dir(name) / "logs" / "latest.log"


# ---------------------------------------------------------------------------
# PID existence check (cross-platform, no required external deps)
# ---------------------------------------------------------------------------

def _pid_exists(pid: int) -> bool:
    try:
        import psutil  # type: ignore
        return psutil.pid_exists(pid)
    except ImportError:
        pass
    if os.name == "nt":
        import ctypes
        PROCESS_QUERY_INFORMATION = 0x0400
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_INFORMATION, False, pid)
        if handle == 0:
            return False
        ctypes.windll.kernel32.CloseHandle(handle)
        return True
    # Unix
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False
    except OSError:
        return False


def is_running(name: str) -> bool:
    rj = running_json_path(name)
    if not rj.exists():
        return False
    try:
        data = json.loads(rj.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return False
    pid = data.get("pid")
    if pid is None:
        return False
    return _pid_exists(pid)


def cleanup_stale_running_files() -> None:
    if not SYSTEMS_ROOT.exists():
        return
    for project in SYSTEMS_ROOT.iterdir():
        if not project.is_dir():
            continue
        rj = project / "running.json"
        if rj.exists() and not is_running(project.name):
            print(f"[projects] Cleaning stale running.json for '{project.name}'")
            rj.unlink()


# ---------------------------------------------------------------------------
# Core CRUD
# ---------------------------------------------------------------------------

def list_projects() -> list:
    if not SYSTEMS_ROOT.exists():
        return []
    result = []
    for d in sorted(SYSTEMS_ROOT.iterdir()):
        if not d.is_dir():
            continue
        sj = d / "system.json"
        if not sj.exists():
            continue
        try:
            data = json.loads(sj.read_text(encoding="utf-8"))
            result.append({"name": d.name, "meta": data.get("meta", {})})
        except (json.JSONDecodeError, OSError):
            pass
    return result


def get_project(name: str) -> dict:
    path = system_json_path(name)
    if not path.exists():
        raise FileNotFoundError(f"Project '{name}' not found")
    return json.loads(path.read_text(encoding="utf-8"))


def save_project(name: str, system: dict) -> None:
    pdir = project_dir(name)
    pdir.mkdir(parents=True, exist_ok=True)
    system_json_path(name).write_text(
        json.dumps(system, indent=2), encoding="utf-8"
    )
    outputs = renderer.render(system, name)
    for rel, content in outputs.items():
        out_path = pdir / rel
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(content, encoding="utf-8")


def create_project_from_template(name: str, template: str) -> dict:
    if project_dir(name).exists():
        raise ValueError(f"Project '{name}' already exists")
    tpl_path = TEMPLATES_ROOT / template / "system.json"
    if not tpl_path.exists():
        raise FileNotFoundError(f"Template '{template}' not found")
    system = json.loads(tpl_path.read_text(encoding="utf-8"))
    system["meta"]["name"] = name
    system["meta"]["created"] = datetime.now(timezone.utc).isoformat()
    system["meta"]["template"] = template
    save_project(name, system)
    return system


def rename_project(old_name: str, new_name: str) -> None:
    if not project_dir(old_name).exists():
        raise FileNotFoundError(f"Project '{old_name}' not found")
    if is_running(old_name):
        raise ValueError(f"Project '{old_name}' is running")
    if project_dir(new_name).exists():
        raise ValueError(f"Project '{new_name}' already exists")
    project_dir(old_name).rename(project_dir(new_name))


def duplicate_project(source_name: str, new_name: str) -> dict:
    src = project_dir(source_name)
    if not src.exists():
        raise FileNotFoundError(f"Project '{source_name}' not found")
    if project_dir(new_name).exists():
        raise ValueError(f"Project '{new_name}' already exists")
    shutil.copytree(
        src,
        project_dir(new_name),
        ignore=shutil.ignore_patterns("running.json", "logs"),
    )
    system = get_project(new_name)
    system["meta"]["name"] = new_name
    system["meta"]["created"] = datetime.now(timezone.utc).isoformat()
    save_project(new_name, system)
    return system


def delete_project(name: str) -> None:
    if not project_dir(name).exists():
        raise FileNotFoundError(f"Project '{name}' not found")
    if is_running(name):
        raise ValueError(f"Project '{name}' is running")
    shutil.rmtree(project_dir(name))
