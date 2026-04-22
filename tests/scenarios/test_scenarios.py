"""Pytest entrypoints for scenario cases."""

from __future__ import annotations

from pathlib import Path

import pytest

from tests.scenarios.cases.fault_to_manual_recovery import FaultToManualRecovery
from tests.scenarios.cases.happy_path_end_to_end import HappyPathEndToEnd
from tests.scenarios.cases.mode_blocking_policy import ModeBlockingPolicy
from tests.scenarios.cases.mode_safety import ModeSafety
from tests.scenarios.cases.multi_device_concurrency import MultiDeviceConcurrency
from tests.scenarios.cases.override_policy import OverridePolicy
from tests.scenarios.cases.parameter_validation import ParameterValidation
from tests.scenarios.cases.precondition_enforcement import PreconditionEnforcement
from tests.scenarios.cases.provider_restart_recovery import ProviderRestartRecovery
from tests.scenarios.cases.slow_sse_client_behavior import SlowSseClientBehavior
from tests.scenarios.cases.telemetry_on_change import TelemetryOnChange


def _scenario_config(provider_exe: Path, port: int, policy: str) -> dict:
    root = Path(__file__).resolve().parents[2]
    bt_path = root / "tests" / "integration" / "fixtures" / "behaviors" / "test_noop.xml"
    fixture_path = root / "tests" / "integration" / "fixtures" / "provider-sim-default.yaml"
    return {
        "runtime": {},
        "http": {"enabled": True, "port": port, "bind": "127.0.0.1"},
        "providers": [
            {
                "id": "sim0",
                "command": str(provider_exe).replace("\\", "/"),
                "args": ["--config", str(fixture_path).replace("\\", "/")],
            }
        ],
        "polling": {"interval_ms": 500},
        "logging": {"level": "info"},
        "automation": {
            "enabled": True,
            "behavior_tree": str(bt_path),
            "tick_rate_hz": 10,
            "manual_gating_policy": policy,
        },
    }


def _run_case(case_cls, runtime_factory, provider_exe: Path, unique_port: int):
    policy = "OVERRIDE" if case_cls is OverridePolicy else "BLOCK"
    fixture = runtime_factory(config_dict=_scenario_config(provider_exe, unique_port, policy), port=unique_port)
    scenario = case_cls(fixture.base_url)

    try:
        scenario.setup()
        scenario.run()
    finally:
        try:
            scenario.cleanup()
        except Exception:
            pass


@pytest.mark.integration
@pytest.mark.scenario
@pytest.mark.timeout(420)
@pytest.mark.parametrize(
    "case_cls",
    [
        HappyPathEndToEnd,
        ModeBlockingPolicy,
        OverridePolicy,
        PreconditionEnforcement,
        ParameterValidation,
        FaultToManualRecovery,
        TelemetryOnChange,
        ModeSafety,
    ],
)
def test_scenario_cases(case_cls, runtime_factory, provider_exe: Path, unique_port: int):
    _run_case(case_cls, runtime_factory, provider_exe, unique_port)


@pytest.mark.integration
@pytest.mark.scenario
@pytest.mark.slow
@pytest.mark.timeout(600)
def test_provider_restart_recovery(runtime_factory, provider_exe: Path, unique_port: int):
    _run_case(ProviderRestartRecovery, runtime_factory, provider_exe, unique_port)


@pytest.mark.integration
@pytest.mark.scenario
@pytest.mark.stress
@pytest.mark.slow
@pytest.mark.timeout(900)
@pytest.mark.parametrize("case_cls", [MultiDeviceConcurrency, SlowSseClientBehavior])
def test_stress_scenarios(case_cls, runtime_factory, provider_exe: Path, unique_port: int):
    _run_case(case_cls, runtime_factory, provider_exe, unique_port)
