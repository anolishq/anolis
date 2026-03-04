"""Pytest entrypoints for integration suites."""

from __future__ import annotations

import signal
from pathlib import Path

import pytest

from tests.integration import (
    process_cleanup_suite,
    provider_config_suite,
    signal_fault_injection_suite,
    signal_handling_suite,
    simulation_devices_suite,
)
from tests.integration.automation_suite import (
    AUTOMATION_CHECKS,
    AutomationTester,
    assert_automation_disabled_returns_unavailable,
)
from tests.integration.concurrency_stress_suite import StressTestRunner
from tests.integration.core_suite import CORE_CHECKS, CoreFeatureTester
from tests.integration.http_suite import HTTP_CHECKS, HttpGatewayTester
from tests.integration.provider_supervision_suite import SUPERVISION_CHECKS, SupervisionTester


@pytest.mark.integration
@pytest.mark.timeout(300)
@pytest.mark.parametrize(
    ("_check_name", "check_fn"),
    CORE_CHECKS,
    ids=[name for name, _ in CORE_CHECKS],
)
def test_core_suite(
    runtime_exe: Path,
    provider_exe: Path,
    integration_timeout: float,
    unique_port: int,
    _check_name: str,
    check_fn,
) -> None:
    tester = CoreFeatureTester(runtime_exe, provider_exe, timeout=integration_timeout, port=unique_port)
    try:
        tester.start_runtime()
        check_fn(tester)
    finally:
        tester.cleanup()


@pytest.mark.integration
@pytest.mark.timeout(300)
@pytest.mark.parametrize(
    ("_check_name", "check_fn"),
    HTTP_CHECKS,
    ids=[name for name, _ in HTTP_CHECKS],
)
def test_http_suite(
    runtime_exe: Path,
    provider_exe: Path,
    integration_timeout: float,
    unique_port: int,
    _check_name: str,
    check_fn,
) -> None:
    tester = HttpGatewayTester(runtime_exe, provider_exe, port=unique_port, timeout=integration_timeout)
    try:
        tester.start_runtime()
        check_fn(tester)
    finally:
        tester.cleanup()


@pytest.mark.integration
@pytest.mark.timeout(360)
@pytest.mark.parametrize(
    ("_check_name", "check_fn"),
    AUTOMATION_CHECKS,
    ids=[name for name, _ in AUTOMATION_CHECKS],
)
def test_automation_suite(
    runtime_exe: Path,
    provider_exe: Path,
    integration_timeout: float,
    unique_port: int,
    _check_name: str,
    check_fn,
) -> None:
    tester = AutomationTester(runtime_exe, provider_exe, port=unique_port, timeout=integration_timeout)
    try:
        tester.start_runtime()
        check_fn(tester)
    finally:
        tester.cleanup()


@pytest.mark.integration
@pytest.mark.timeout(360)
def test_automation_disabled_mode_api(
    runtime_exe: Path,
    provider_exe: Path,
    integration_timeout: float,
    unique_port: int,
) -> None:
    assert_automation_disabled_returns_unavailable(
        runtime_exe,
        provider_exe,
        port=unique_port,
        timeout=integration_timeout,
    )


@pytest.mark.integration
@pytest.mark.timeout(360)
@pytest.mark.parametrize(
    ("_check_name", "check_fn"),
    SUPERVISION_CHECKS,
    ids=[name for name, _ in SUPERVISION_CHECKS],
)
def test_provider_supervision_suite(
    runtime_exe: Path,
    provider_exe: Path,
    integration_timeout: float,
    unique_port: int,
    _check_name: str,
    check_fn,
) -> None:
    tester = SupervisionTester(runtime_exe, provider_exe, integration_timeout, port=unique_port)
    check_fn(tester)


@pytest.mark.integration
@pytest.mark.timeout(360)
@pytest.mark.parametrize(
    "check",
    [
        provider_config_suite.test_missing_config_required,
        provider_config_suite.test_default_config_loads,
        provider_config_suite.test_multi_device_config,
        provider_config_suite.test_minimal_config,
        provider_config_suite.test_invalid_yaml_handling,
        provider_config_suite.test_unknown_device_type,
        provider_config_suite.test_fixture_configs_use_supported_simulation_keys,
    ],
)
def test_provider_config_suite(runtime_exe: Path, provider_exe: Path, unique_port: int, check) -> None:
    check(runtime_exe, provider_exe, unique_port)


@pytest.mark.integration
@pytest.mark.timeout(300)
@pytest.mark.parametrize(
    "check",
    [
        process_cleanup_suite.test_scoped_cleanup,
        process_cleanup_suite.test_cleanup_on_exception,
        process_cleanup_suite.test_double_cleanup,
        process_cleanup_suite.test_graceful_vs_force_kill,
    ],
)
def test_process_cleanup_suite(runtime_exe: Path, provider_exe: Path, unique_port: int, check) -> None:
    check(runtime_exe, provider_exe, unique_port)


@pytest.mark.integration
@pytest.mark.timeout(240)
@pytest.mark.parametrize(
    ("sig", "name"),
    [
        (signal.SIGINT, "SIGINT"),
        (signal.SIGTERM, "SIGTERM"),
    ],
    ids=["sigint", "sigterm"],
)
def test_signal_handling_suite(runtime_exe: Path, provider_exe: Path, unique_port: int, sig, name) -> None:
    signal_handling_suite.test_signal_handling(str(runtime_exe), str(provider_exe), sig, name, unique_port)


@pytest.mark.integration
@pytest.mark.timeout(300)
@pytest.mark.parametrize(
    "check",
    [
        simulation_devices_suite.test_device_discovery,
        simulation_devices_suite.test_relayio0,
        simulation_devices_suite.test_analogsensor0,
        simulation_devices_suite.test_fault_injection_clear,
        simulation_devices_suite.test_fault_injection_device_unavailable,
        simulation_devices_suite.test_fault_injection_call_latency,
        simulation_devices_suite.test_fault_injection_call_failure,
    ],
)
def test_simulation_devices_suite(runtime_factory, unique_port: int, check) -> None:
    fixture = runtime_factory(port=unique_port)
    check(fixture.base_url)


@pytest.mark.integration
@pytest.mark.timeout(180)
def test_signal_fault_injection_suite(runtime_factory, unique_port: int) -> None:
    fixture = runtime_factory(port=unique_port)
    signal_fault_injection_suite.test_signal_fault_injection(fixture.base_url)


@pytest.mark.integration
@pytest.mark.stress
@pytest.mark.slow
@pytest.mark.timeout(900)
def test_concurrency_stress_suite(runtime_exe: Path, provider_exe: Path, unique_port: int) -> None:
    runner = StressTestRunner(runtime_exe, provider_exe, num_clients=10, http_port=unique_port)
    runner.setup()
    runner.run_stress_test(num_restarts=100)
