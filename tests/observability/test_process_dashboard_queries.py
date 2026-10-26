"""The generated process dashboard's queries, run through Grafana against fixture telemetry."""

from __future__ import annotations

from typing import Any

import process_dashboard as gen
import pytest

from tests.observability.harness import Dashboard, Telemetry, UInt, Window
from tests.observability.scenarios import MINUTE

RUNTIME = "r1"
SIGNALS = [
    {"signal_id": "x", "value_type": "double", "label": "X", "unit": "V"},
    {"signal_id": "n", "value_type": "int64", "label": "N"},
    {"signal_id": "u", "value_type": "uint64", "label": "U"},
    {"signal_id": "b", "value_type": "bool", "label": "B"},
    {"signal_id": "s", "value_type": "string", "label": "S"},
]


def _inventory() -> dict[str, Any]:
    ok = {"code": "OK", "message": "ok"}
    devices = [("p1", "d1", SIGNALS), ("p1", "d2", SIGNALS[:1])]  # x on two devices
    return {
        "devices": {
            "status": ok,
            "devices": [
                {"provider_id": p, "device_id": d, "display_name": d, "type": "test.device"} for p, d, _ in devices
            ],
        },
        "capabilities": [
            {"status": ok, "provider_id": p, "device_id": d, "capabilities": {"signals": s, "functions": []}}
            for p, d, s in devices
        ],
    }


@pytest.fixture(scope="module")
def dashboard() -> Dashboard:
    model = gen.generate(_inventory(), RUNTIME)
    return Dashboard(model["uid"], model)


def _points(grafana, dashboard, panel_id, window) -> list[tuple[int, Any]]:
    """(time, value) of every non-null point, across the panel's series, by time."""
    points = []
    for frame in grafana.panel(dashboard, panel_id, window):
        time_column = next((c for c in frame.columns if c.name in ("Time", "_time")), None)
        if time_column is None:  # an empty result
            assert not any(c.values for c in frame.value_columns()), frame
            continue
        times = time_column.values
        for column in frame.value_columns():
            points += [(t, v) for t, v in zip(times, column.values, strict=True) if v is not None]
    return sorted(points)


def _series(grafana, dashboard, panel_id, window) -> dict[str, list[Any]]:
    """signal_id -> values, for the quality panel's one series per signal."""
    found: dict[str, list[Any]] = {}
    for frame in grafana.panel(dashboard, panel_id, window):
        for column in frame.value_columns():
            found.setdefault(column.labels.get("signal_id", column.name), []).extend(
                v for v in column.values if v is not None
            )
    return found


def signal(device: str, signal_id: str) -> int:
    return gen._stable_id("signal", "p1", device, signal_id)


def quality(device: str) -> int:
    return gen._stable_id("quality", "p1", device)


def live(now_ms: int) -> Telemetry:
    return Telemetry().provider_health(now_ms - MINUTE, runtime=RUNTIME, provider="p1")


@pytest.mark.parametrize(
    "signal_id, value, drawn",
    [("x", 2.5, 2.5), ("n", -4, -4.0), ("u", UInt(7), 7.0), ("b", True, 1), ("s", "RUNNING", "RUNNING")],
)
def test_each_value_type_is_drawn(grafana, dashboard, load, now_ms, hour, signal_id, value, drawn):
    load(live(now_ms).signal(now_ms - 5 * MINUTE, provider="p1", device="d1", signal=signal_id, value=value))
    points = _points(grafana, dashboard, signal("d1", signal_id), hour)
    assert points and points[-1][1] == drawn
    # The panel's value mappings are keyed 0 and 1; True == 1 in Python, so check the type too.
    assert isinstance(points[-1][1], bool) == isinstance(drawn, bool)


@pytest.mark.parametrize("signal_id, value", [("x", 4.25), ("b", True), ("s", "IDLE")])
def test_a_value_unchanged_since_before_the_range_is_held_across_it(
    grafana, dashboard, load, now_ms, hour, signal_id, value
):
    """A window with no new events still shows the value, from the start of the range to its end."""
    load(live(now_ms).signal(now_ms - 3 * 60 * MINUTE, provider="p1", device="d1", signal=signal_id, value=value))
    points = _points(grafana, dashboard, signal("d1", signal_id), hour)
    assert points, "nothing drawn"
    assert points[0][0] <= hour.start_ms + hour.interval_ms
    assert points[-1][0] > hour.stop_ms - hour.interval_ms
    assert len({v for _, v in points}) == 1


def test_a_window_shows_the_last_value_in_it_not_a_mean(grafana, dashboard, load, now_ms):
    window = Window.last(3600, now_ms=now_ms, interval_ms=60_000)
    start = now_ms - 30 * MINUTE - (now_ms % MINUTE)
    t = live(now_ms)
    t.signal(start + 1_000, provider="p1", device="d1", signal="x", value=0.0)
    t.signal(start + 2_000, provider="p1", device="d1", signal="x", value=10.0)
    load(t)
    assert {v for _, v in _points(grafana, dashboard, signal("d1", "x"), window)} == {10.0}


def test_a_stopped_runtime_is_not_seeded(grafana, dashboard, load, now_ms, hour):
    """No provider health in the range: the runtime is gone, so its old values are not carried in."""
    t = Telemetry().provider_health(now_ms - 3 * 60 * MINUTE, runtime=RUNTIME, provider="p1")
    t.signal(now_ms - 3 * 60 * MINUTE, provider="p1", device="d1", signal="x", value=1.0)
    load(t)
    assert _points(grafana, dashboard, signal("d1", "x"), hour) == []


def test_a_gap_inside_the_range_is_drawn_as_the_held_value(grafana, dashboard, load, now_ms, hour):
    """A limitation, pinned so a change to it is deliberate: rows are written only on change, and
    nothing marks a runtime that stopped and restarted inside the range, so the line runs across."""
    t = Telemetry()
    t.provider_health(now_ms - 55 * MINUTE, runtime=RUNTIME, provider="p1")
    t.signal(now_ms - 55 * MINUTE, provider="p1", device="d1", signal="x", value=3.0)
    t.provider_health(now_ms - 5 * MINUTE, runtime=RUNTIME, provider="p1")  # nothing in between
    load(t)
    points = _points(grafana, dashboard, signal("d1", "x"), hour)
    gap = [v for at, v in points if now_ms - 50 * MINUTE < at < now_ms - 10 * MINUTE]
    assert gap and set(gap) == {3.0}


def test_quality_changes_show_on_the_device_quality_panel(grafana, dashboard, load, now_ms, hour):
    t = live(now_ms)
    t.signal(now_ms - 40 * MINUTE, provider="p1", device="d1", signal="x", value=1.0)
    t.signal(now_ms - 20 * MINUTE, provider="p1", device="d1", signal="x", quality="FAULT")  # quality only
    t.signal(now_ms - 3 * 60 * MINUTE, provider="p1", device="d1", signal="n", quality="STALE", value=1)
    load(t)
    series = _series(grafana, dashboard, quality("d1"), hour)
    assert series["x"][0] == "OK" and series["x"][-1] == "FAULT"
    assert set(series["n"]) == {"STALE"}  # seeded from before the range
    assert "s" not in series  # no rows, no series


def test_a_panel_shows_only_its_own_runtime_and_device(grafana, dashboard, load, now_ms, hour):
    t = live(now_ms).provider_health(now_ms - MINUTE, runtime="r2", provider="p1")
    t.signal(now_ms - 5 * MINUTE, provider="p1", device="d1", signal="x", value=1.0)
    t.signal(now_ms - 5 * MINUTE, provider="p1", device="d2", signal="x", value=2.0)
    t.signal(now_ms - 5 * MINUTE, runtime="r2", provider="p1", device="d1", signal="x", value=3.0)
    t.signal(now_ms - 5 * MINUTE, provider="p2", device="d1", signal="x", value=4.0)
    load(t)
    assert {v for _, v in _points(grafana, dashboard, signal("d1", "x"), hour)} == {1.0}
    assert {v for _, v in _points(grafana, dashboard, signal("d2", "x"), hour)} == {2.0}


def test_annotations_show_this_runtime_and_untagged_rows(grafana, dashboard, load, now_ms, hour):
    t = Telemetry()
    t.mode_change(now_ms - 10 * MINUTE, "IDLE", "MANUAL", runtime=RUNTIME)
    t.mode_change(now_ms - 9 * MINUTE, "MANUAL", "AUTO", runtime="r2")
    t.mode_change(now_ms - 8 * MINUTE, "AUTO", "IDLE", runtime=None)
    t.parameter_change(now_ms - 7 * MINUTE, "rate", "1", "2", runtime=RUNTIME)
    t.parameter_change(now_ms - 6 * MINUTE, "rate", "2", "3", runtime="r2")
    load(t)
    modes = sorted(v for f in grafana.annotation(dashboard, "Mode Changes", hour) for v in f.column("text").values)
    assert modes == ["Mode: AUTO -> IDLE", "Mode: IDLE -> MANUAL"]
    params = [v for f in grafana.annotation(dashboard, "Parameter Changes", hour) for v in f.column("text").values]
    assert params == ["rate: 1 -> 2"]
