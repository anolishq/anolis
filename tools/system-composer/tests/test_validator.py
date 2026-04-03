"""Validator unit tests (pytest-compatible).

Run with:  python -m pytest tools/system-composer/tests/
Or standalone: python tools/system-composer/tests/test_validator.py
"""
import json
import pathlib
import sys

_SC_DIR = str(pathlib.Path(__file__).parent.parent)
if _SC_DIR not in sys.path:
    sys.path.insert(0, _SC_DIR)

from backend import validator  # noqa: E402

TEMPLATES_DIR = pathlib.Path(__file__).parent.parent / "templates"


def load_template(name: str) -> dict:
    return json.loads(
        (TEMPLATES_DIR / name / "system.json").read_text(encoding="utf-8")
    )


# ---------------------------------------------------------------------------
# Clean templates produce no errors
# ---------------------------------------------------------------------------

def test_clean_sim_is_valid():
    system = load_template("sim-quickstart")
    assert validator.validate_system(system) == []


def test_clean_mixed_is_valid():
    system = load_template("mixed-bus-mock")
    assert validator.validate_system(system) == []


def test_clean_bioreactor_is_valid():
    system = load_template("bioreactor-manual")
    assert validator.validate_system(system) == []


# ---------------------------------------------------------------------------
# Duplicate provider IDs
# ---------------------------------------------------------------------------

def test_duplicate_provider_id():
    system = load_template("sim-quickstart")
    # Duplicate the first provider entry
    system["topology"]["runtime"]["providers"].append(
        dict(system["topology"]["runtime"]["providers"][0])
    )
    errors = validator.validate_system(system)
    assert any("Duplicate provider IDs" in e for e in errors), errors


# ---------------------------------------------------------------------------
# Composer port collision
# ---------------------------------------------------------------------------

def test_composer_port_conflict():
    system = load_template("sim-quickstart")
    system["topology"]["runtime"]["http_port"] = 3002
    errors = validator.validate_system(system)
    assert any("3002" in e for e in errors), errors


# ---------------------------------------------------------------------------
# I2C address conflict on the same bus
# ---------------------------------------------------------------------------

def test_shared_bus_address_conflict():
    system = load_template("mixed-bus-mock")
    # Give ezo the same address as the first bread device, on the same bus
    bread_addr = system["topology"]["providers"]["bread0"]["devices"][0]["address"]
    bread_bus = system["paths"]["providers"]["bread0"]["bus_path"]
    system["topology"]["providers"]["ezo0"]["devices"][0]["address"] = bread_addr
    system["paths"]["providers"]["ezo0"]["bus_path"] = bread_bus
    errors = validator.validate_system(system)
    assert any("I2C address" in e for e in errors), errors


def test_same_address_different_bus_no_conflict():
    system = load_template("mixed-bus-mock")
    bread_addr = system["topology"]["providers"]["bread0"]["devices"][0]["address"]
    system["topology"]["providers"]["ezo0"]["devices"][0]["address"] = bread_addr
    # Keep different bus paths — this must NOT produce an error
    system["paths"]["providers"]["bread0"]["bus_path"] = "/dev/i2c-1"
    system["paths"]["providers"]["ezo0"]["bus_path"] = "/dev/i2c-2"
    errors = validator.validate_system(system)
    address_errors = [e for e in errors if "I2C address" in e]
    assert address_errors == [], address_errors


# ---------------------------------------------------------------------------
# Provider in runtime list but not in topology
# ---------------------------------------------------------------------------

def test_provider_in_runtime_but_missing_from_topology():
    system = load_template("sim-quickstart")
    system["topology"]["runtime"]["providers"].append(
        {"id": "ghost", "kind": "sim", "timeout_ms": 5000}
    )
    errors = validator.validate_system(system)
    assert any("ghost" in e for e in errors), errors


# ---------------------------------------------------------------------------
# Provider in topology but not in runtime list
# ---------------------------------------------------------------------------

def test_provider_in_topology_missing_from_runtime():
    system = load_template("sim-quickstart")
    system["topology"]["providers"]["orphan0"] = {"kind": "sim", "devices": []}
    errors = validator.validate_system(system)
    assert any("orphan0" in e for e in errors), errors


# ---------------------------------------------------------------------------
# Run standalone
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    tests = [
        test_clean_sim_is_valid,
        test_clean_mixed_is_valid,
        test_clean_bioreactor_is_valid,
        test_duplicate_provider_id,
        test_composer_port_conflict,
        test_shared_bus_address_conflict,
        test_same_address_different_bus_no_conflict,
        test_provider_in_runtime_but_missing_from_topology,
        test_provider_in_topology_missing_from_runtime,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except AssertionError as exc:
            print(f"  FAIL  {t.__name__}: {exc}")
        except Exception as exc:
            print(f"  ERROR {t.__name__}: {exc}")
    print(f"\n{passed}/{len(tests)} tests passed.")
    sys.exit(0 if passed == len(tests) else 1)
