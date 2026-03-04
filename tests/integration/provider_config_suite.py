"""
Provider Configuration Integration Tests

Validates anolis-provider-sim configuration system requirements:
1. --config argument is mandatory (no backward compatibility)
2. Multi-device configurations with custom IDs work correctly
3. Error handling for missing/invalid config files
4. Device type validation and graceful degradation

These tests ensure the provider configuration system works correctly
before Operator UI integration.

"""

import os
import tempfile
import time
from pathlib import Path

import requests

from tests.support.runtime_fixture import RuntimeFixture


def get_fixture_path(filename: str) -> Path:
    """Get path to test fixture config file."""
    return Path(__file__).parent / "fixtures" / filename


REMOVED_SIMULATION_KEYS = ("noise_enabled", "noise_amplitude", "update_rate_hz")
FIXTURE_CONFIGS = ("provider-sim-default.yaml", "provider-minimal.yaml", "provider-multi-tempctl.yaml")


def test_fixture_configs_use_supported_simulation_keys(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Fail fast if fixtures reintroduce removed provider-sim simulation keys."""

    del runtime_path
    del provider_path
    del port

    violations: list[str] = []
    for fixture_name in FIXTURE_CONFIGS:
        fixture_path = get_fixture_path(fixture_name)
        assert fixture_path.exists(), f"Fixture config not found: {fixture_path}"

        content = fixture_path.read_text(encoding="utf-8")
        for key in REMOVED_SIMULATION_KEYS:
            token = f"{key}:"
            if token in content:
                violations.append(f"{fixture_name}: contains removed key simulation.{key}")

    assert not violations, "\n".join(violations)


def create_invalid_yaml_config() -> Path:
    """Create a temporary file with invalid YAML syntax."""
    fd, path = tempfile.mkstemp(suffix=".yaml", prefix="invalid-config-")
    with os.fdopen(fd, "w") as f:
        f.write("devices: [this is: {not: valid: yaml")
    return Path(path)


def create_unknown_type_config() -> Path:
    """Create config with unknown device type."""
    fd, path = tempfile.mkstemp(suffix=".yaml", prefix="unknown-type-")
    with os.fdopen(fd, "w") as f:
        f.write("devices:\n")
        f.write("  - id: widget0\n")
        f.write("    type: nonexistent_device\n")
        f.write("  - id: tempctl0\n")
        f.write("    type: tempctl\n")
    return Path(path)


def test_missing_config_required(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Test that provider requires --config argument."""

    # Create runtime config WITHOUT --config argument for provider
    config = {
        "runtime": {},
        "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_path).replace("\\", "/"),
                "args": [],  # No --config argument
                "timeout_ms": 5000,
            }
        ],
        "polling": {"interval_ms": 500},
    }

    fixture = RuntimeFixture(runtime_path, provider_path, config_dict=config)

    try:
        assert fixture.start(), "Runtime failed to start"

        capture = fixture.get_output_capture()
        assert capture is not None, "No output capture available"

        # Wait a moment for provider to exit (provider launched by runtime)
        time.sleep(3)

        # Check if runtime is still running (should be, but provider should have died)
        # Provider errors appear in runtime's captured output
        output = capture.get_all_output()

        # Look for provider exit or error message about config
        has_config_error = "FATAL" in output and "config" in output.lower()
        has_provider_died = "Provider exited" in output or "Provider died" in output

        assert has_config_error or has_provider_died, "Expected provider to fail without config"

    finally:
        fixture.cleanup()


def test_default_config_loads(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Test that default fixture config loads correctly."""
    base_url = f"http://127.0.0.1:{port}"

    config_path = get_fixture_path("provider-sim-default.yaml")

    assert config_path.exists(), f"Fixture config not found: {config_path}"

    config = {
        "runtime": {},
        "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_path).replace("\\", "/"),
                "args": ["--config", str(config_path).replace("\\", "/")],
                "timeout_ms": 5000,
            }
        ],
        "polling": {"interval_ms": 500},
    }

    fixture = RuntimeFixture(runtime_path, provider_path, config_dict=config)

    try:
        assert fixture.start(), "Runtime failed to start"

        capture = fixture.get_output_capture()
        assert capture is not None, "No output capture available"

        # Wait for runtime to become ready
        assert capture.wait_for_marker("Runtime Ready", timeout=15), "Runtime did not become ready"

        # Query devices via HTTP API
        resp = requests.get(f"{base_url}/v0/devices", timeout=5)
        assert resp.status_code == 200, f"HTTP request failed: {resp.status_code}"

        data = resp.json()
        devices = data.get("devices", [])
        sim0_devices = [d for d in devices if d.get("provider_id") == "sim0"]
        device_ids = [d["device_id"] for d in sim0_devices]

        # Expected: tempctl0, motorctl0, relayio0, analogsensor0, chaos_control
        expected = {"tempctl0", "motorctl0", "relayio0", "analogsensor0", "chaos_control"}

        actual = set(device_ids)
        assert actual == expected, f"Device set mismatch. expected={expected} actual={actual}"
    finally:
        fixture.cleanup()


def test_multi_device_config(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Test multi-device configuration with 2 tempctl instances."""
    base_url = f"http://127.0.0.1:{port}"

    config_path = get_fixture_path("provider-multi-tempctl.yaml")

    assert config_path.exists(), f"Fixture config not found: {config_path}"

    config = {
        "runtime": {},
        "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_path).replace("\\", "/"),
                "args": ["--config", str(config_path).replace("\\", "/")],
                "timeout_ms": 5000,
            }
        ],
        "polling": {"interval_ms": 500},
    }

    fixture = RuntimeFixture(runtime_path, provider_path, config_dict=config)

    try:
        assert fixture.start(), "Runtime failed to start"

        capture = fixture.get_output_capture()
        assert capture is not None, "No output capture available"

        # Wait for runtime to become ready
        assert capture.wait_for_marker("Runtime Ready", timeout=15), "Runtime did not become ready"

        # Query devices
        resp = requests.get(f"{base_url}/v0/devices", timeout=5)
        assert resp.status_code == 200, f"HTTP request failed: {resp.status_code}"

        data = resp.json()
        devices = data.get("devices", [])
        sim0_devices = [d for d in devices if d.get("provider_id") == "sim0"]
        device_ids = [d["device_id"] for d in sim0_devices]

        # Expected: tempctl0, tempctl1, motorctl0, chaos_control
        expected = {"tempctl0", "tempctl1", "motorctl0", "chaos_control"}

        assert set(device_ids) == expected, f"Expected {expected}, got {set(device_ids)}"
        # Verify both tempctl devices are functional
        for dev_id in ["tempctl0", "tempctl1"]:
            resp = requests.get(f"{base_url}/v0/state/sim0/{dev_id}", timeout=5)
            assert resp.status_code == 200, f"{dev_id} returned status {resp.status_code}"

    finally:
        fixture.cleanup()


def test_minimal_config(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Test minimal configuration with single device."""
    base_url = f"http://127.0.0.1:{port}"

    config_path = get_fixture_path("provider-minimal.yaml")

    assert config_path.exists(), f"Fixture config not found: {config_path}"

    config = {
        "runtime": {},
        "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_path).replace("\\", "/"),
                "args": ["--config", str(config_path).replace("\\", "/")],
                "timeout_ms": 5000,
            }
        ],
        "polling": {"interval_ms": 500},
    }

    fixture = RuntimeFixture(runtime_path, provider_path, config_dict=config)

    try:
        assert fixture.start(), "Runtime failed to start"

        capture = fixture.get_output_capture()
        assert capture is not None, "No output capture available"

        # Wait for runtime to become ready
        assert capture.wait_for_marker("Runtime Ready", timeout=15), "Runtime did not become ready"

        # Query devices
        resp = requests.get(f"{base_url}/v0/devices", timeout=5)
        assert resp.status_code == 200, f"HTTP request failed: {resp.status_code}"

        data = resp.json()
        devices = data.get("devices", [])
        sim0_devices = [d for d in devices if d.get("provider_id") == "sim0"]
        device_ids = [d["device_id"] for d in sim0_devices]

        # Expected: only tempctl0 + chaos_control
        expected = {"tempctl0", "chaos_control"}

        assert set(device_ids) == expected, f"Expected {expected}, got {set(device_ids)}"
    finally:
        fixture.cleanup()


def test_invalid_yaml_handling(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Test provider handling of invalid YAML syntax."""

    config_path = create_invalid_yaml_config()

    try:
        config = {
            "runtime": {},
            "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
            "providers": [
                {
                    "id": "sim0",
                    "command": str(provider_path).replace("\\", "/"),
                    "args": ["--config", str(config_path).replace("\\", "/")],
                    "timeout_ms": 5000,
                }
            ],
            "polling": {"interval_ms": 500},
        }

        fixture = RuntimeFixture(runtime_path, provider_path, config_dict=config)

        try:
            assert fixture.start(), "Runtime failed to start"

            # Wait for provider to exit with error
            time.sleep(3)

            # Check output for provider failure
            capture = fixture.get_output_capture()
            assert capture is not None, "No output capture available"

            output = capture.get_all_output()

            # Provider should have died due to bad YAML
            assert "Provider exited" in output or "Provider died" in output or "Failed to load" in output, (
                f"Expected provider to fail with invalid YAML. Output sample: {output[:300]}"
            )

        finally:
            fixture.cleanup()
    finally:
        config_path.unlink(missing_ok=True)


def test_unknown_device_type(runtime_path: Path, provider_path: Path, port: int) -> None:
    """Test that unknown device types cause provider to fail fast at startup."""

    config_path = create_unknown_type_config()

    try:
        config = {
            "runtime": {},
            "http": {"enabled": True, "bind": "127.0.0.1", "port": port},
            "providers": [
                {
                    "id": "sim0",
                    "command": str(provider_path).replace("\\", "/"),
                    "args": ["--config", str(config_path).replace("\\", "/")],
                    "timeout_ms": 5000,
                }
            ],
            "polling": {"interval_ms": 500},
        }

        fixture = RuntimeFixture(runtime_path, provider_path, config_dict=config)

        try:
            assert fixture.start(), "Runtime failed to start"

            capture = fixture.get_output_capture()
            assert capture is not None, "No output capture available"

            # Provider should fail to start with unknown device type
            # Check for error message in output
            if not capture.wait_for_marker("unknown device type", timeout=5):
                assert not capture.wait_for_marker("Runtime Ready", timeout=5), (
                    "Provider started successfully (should have failed fast)"
                )

        finally:
            fixture.cleanup()
    finally:
        config_path.unlink(missing_ok=True)
