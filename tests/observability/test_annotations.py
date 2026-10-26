"""Mode and parameter annotations read every field they display, scoped by runtime."""

from __future__ import annotations

import pytest

from tests.observability.harness import Telemetry, rows
from tests.observability.scenarios import MINUTE

MODE = [
    ("anolis-device-health", "Mode Changes"),
    ("anolis-io-health", "Mode Changes"),
    ("anolis-signal-history", "Mode Changes"),
    ("anolis-system-overview", "Mode Changes"),
]
PARAMETER = [
    ("anolis-device-health", "Parameter Changes"),
    ("anolis-signal-history", "Parameter Changes"),
    ("anolis-system-overview", "Parameter Changes"),
]


def _events(grafana, dashboard, name, window, selected=None) -> list[tuple[int, str]]:
    return sorted((row["time"], row["text"]) for row in rows(grafana.annotation(dashboard, name, window, selected)))


@pytest.mark.parametrize("uid,name", MODE)
def test_mode_annotation_reads_both_modes(grafana, dashboards, load, now_ms, hour, uid, name):
    at = now_ms - 20 * MINUTE
    load(Telemetry().mode_change(at, "MANUAL", "AUTO"))
    assert _events(grafana, dashboards[uid], name, hour) == [(at, "Mode: MANUAL -> AUTO")]


@pytest.mark.parametrize("uid,name", PARAMETER)
def test_parameter_annotation_reads_name_and_values(grafana, dashboards, load, now_ms, hour, uid, name):
    at = now_ms - 10 * MINUTE
    load(Telemetry().parameter_change(at, "setpoint", "37.0", "37.5"))
    assert _events(grafana, dashboards[uid], name, hour) == [(at, "setpoint: 37.0 -> 37.5")]


@pytest.mark.parametrize("uid,name", MODE + PARAMETER)
def test_annotations_follow_the_runtime_filter(grafana, dashboards, load, now_ms, hour, uid, name):
    """r1's events and untagged history show under r1; r2's do not."""
    mine, untagged, other = now_ms - 30 * MINUTE, now_ms - 20 * MINUTE, now_ms - 10 * MINUTE
    t = Telemetry()
    if name == "Mode Changes":
        t.mode_change(mine, "IDLE", "MANUAL", runtime="r1")
        t.mode_change(untagged, "MANUAL", "AUTO", runtime=None)
        t.mode_change(other, "AUTO", "FAULT", runtime="r2")
    else:
        t.parameter_change(mine, "setpoint", "1", "2", runtime="r1")
        t.parameter_change(untagged, "setpoint", "2", "3", runtime=None)
        t.parameter_change(other, "setpoint", "3", "4", runtime="r2")
    load(t)
    shown = [time for time, _ in _events(grafana, dashboards[uid], name, hour, {"runtime_name": ["r1"]})]
    assert shown == [mine, untagged]
