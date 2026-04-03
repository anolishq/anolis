"""Render-and-validate tests for all bundled templates.

Run with:  python -m pytest tools/system-composer/tests/
Or standalone: python tools/system-composer/tests/test_templates.py
"""
import json
import pathlib
import subprocess
import sys
import tempfile

# Make 'backend' importable without installing
_SC_DIR = str(pathlib.Path(__file__).parent.parent)
if _SC_DIR not in sys.path:
    sys.path.insert(0, _SC_DIR)

import yaml  # noqa: E402 (PyYAML must be available for the renderer already uses it)
from backend import renderer  # noqa: E402

TEMPLATES_DIR = pathlib.Path(__file__).parent.parent / "templates"

# Platform-dependent binary paths (relative to repo root)
_WIN_RUNTIME = pathlib.Path("build/dev-windows-release/core/anolis-runtime.exe")
_LIN_RUNTIME = pathlib.Path("build/dev-release/core/anolis-runtime")
RUNTIME_BIN = _WIN_RUNTIME if sys.platform == "win32" else _LIN_RUNTIME


def load_template(name: str) -> dict:
    return json.loads((TEMPLATES_DIR / name / "system.json").read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# Template: sim-quickstart
# ---------------------------------------------------------------------------

def test_sim_quickstart_renders():
    system = load_template("sim-quickstart")
    outputs = renderer.render(system, "test-sim")
    assert "anolis-runtime.yaml" in outputs
    assert "providers/sim0.yaml" in outputs
    for key, content in outputs.items():
        parsed = yaml.safe_load(content)
        assert parsed is not None, f"Empty YAML for {key}"


def test_sim_quickstart_check_config():
    if not RUNTIME_BIN.exists():
        try:
            import pytest
            pytest.skip(f"Runtime binary not found at {RUNTIME_BIN} — build first")
        except ImportError:
            print(f"SKIP  test_sim_quickstart_check_config — binary missing")
            return
    system = load_template("sim-quickstart")
    outputs = renderer.render(system, "test-cc")
    with tempfile.TemporaryDirectory() as tmpdir:
        runtime_yaml = pathlib.Path(tmpdir) / "anolis-runtime.yaml"
        runtime_yaml.write_text(outputs["anolis-runtime.yaml"], encoding="utf-8")
        result = subprocess.run(
            [str(RUNTIME_BIN), "--check-config", str(runtime_yaml)],
            capture_output=True,
            timeout=10,
        )
        assert result.returncode == 0, result.stderr.decode(errors="replace")


# ---------------------------------------------------------------------------
# Template: mixed-bus-mock
# ---------------------------------------------------------------------------

def test_mixed_bus_mock_renders():
    system = load_template("mixed-bus-mock")
    outputs = renderer.render(system, "test-mixed")
    assert "anolis-runtime.yaml" in outputs
    assert "providers/bread0.yaml" in outputs
    assert "providers/ezo0.yaml" in outputs
    for key, content in outputs.items():
        parsed = yaml.safe_load(content)
        assert parsed is not None, f"Empty YAML for {key}"


# ---------------------------------------------------------------------------
# Template: bioreactor-manual
# ---------------------------------------------------------------------------

def test_bioreactor_manual_renders():
    system = load_template("bioreactor-manual")
    outputs = renderer.render(system, "test-bio")
    assert "anolis-runtime.yaml" in outputs
    assert "providers/bread0.yaml" in outputs
    assert "providers/ezo0.yaml" in outputs
    for key, content in outputs.items():
        parsed = yaml.safe_load(content)
        assert parsed is not None, f"Empty YAML for {key}"


# ---------------------------------------------------------------------------
# YAML structural assertions
# ---------------------------------------------------------------------------

def test_runtime_yaml_has_required_sections():
    """Runtime YAML must have 'http' and 'providers' sections."""
    for tpl in ("sim-quickstart", "mixed-bus-mock", "bioreactor-manual"):
        system = load_template(tpl)
        outputs = renderer.render(system, "test-struct")
        doc = yaml.safe_load(outputs["anolis-runtime.yaml"])
        assert "http" in doc, f"{tpl}: missing 'http' section"
        assert "providers" in doc, f"{tpl}: missing 'providers' section"
        assert isinstance(doc["providers"], list), f"{tpl}: 'providers' must be a list"
        assert len(doc["providers"]) > 0, f"{tpl}: 'providers' must not be empty"


def test_provider_yaml_has_command_field():
    """Each provider YAML is embedded via --config; the runtime entry must have a command."""
    system = load_template("sim-quickstart")
    rt_doc = yaml.safe_load(renderer.render(system, "test-cmd")["anolis-runtime.yaml"])
    for p in rt_doc["providers"]:
        assert "command" in p, f"Provider entry missing 'command': {p}"
        assert "args" in p, f"Provider entry missing 'args': {p}"


# ---------------------------------------------------------------------------
# Run standalone
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    tests = [
        test_sim_quickstart_renders,
        test_sim_quickstart_check_config,
        test_mixed_bus_mock_renders,
        test_bioreactor_manual_renders,
        test_runtime_yaml_has_required_sections,
        test_provider_yaml_has_command_field,
    ]
    passed = 0
    skipped = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except AssertionError as exc:
            print(f"  FAIL  {t.__name__}: {exc}")
        except Exception as exc:
            msg = str(exc)
            if "SKIP" in msg or "binary" in msg.lower():
                print(f"  SKIP  {t.__name__}: {msg}")
                skipped += 1
            else:
                print(f"  ERROR {t.__name__}: {exc}")

    total = len(tests) - skipped
    print(f"\n{passed}/{total} tests passed ({skipped} skipped).")
    sys.exit(0 if passed == total else 1)
