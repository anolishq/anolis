"""Signal History: every typed value field shown, every panel honours the filters."""

from __future__ import annotations

import pytest

from tests.observability.harness import Telemetry, UInt, Window
from tests.observability.scenarios import MINUTE

UID = "anolis-signal-history"


def _series(grafana, dashboard, window, selected=None) -> dict[str, list]:
    """signal_id -> values, across every panel."""
    found: dict[str, list] = {}
    for panel in dashboard.panels():
        for frame in grafana.panel(dashboard, panel["id"], window, selected):
            for column in frame.value_columns():
                signal = column.labels.get("signal_id")
                # The String table names its signal column "Signal" (#712).
                if signal is None and "Signal" in [c.name for c in frame.columns]:
                    for row in frame.rows():
                        found.setdefault(row["Signal"], []).append(row.get(column.name))
                    continue
                found.setdefault(signal or column.name, []).extend(v for v in column.values if v is not None)
    return found


@pytest.fixture()
def typed(load, now_ms):
    t = Telemetry().provider_health(now_ms - MINUTE)
    at = now_ms - 5 * MINUTE
    t.signal(at, signal="double", value=1.5)
    t.signal(at, signal="int", value=-3)
    t.signal(at, signal="uint", value=UInt(7))
    t.signal(at, signal="bool", value=True)
    t.signal(at, signal="string", value="RUNNING")
    load(t)


def test_every_typed_value_field_is_shown(grafana, dashboards, typed, hour):
    shown = _series(grafana, dashboards[UID], hour)
    missing = sorted({"double", "int", "uint", "bool", "string"} - set(shown))
    assert not missing, f"signals with no panel: {missing}"


def test_a_signal_unchanged_since_before_the_range_is_still_drawn(grafana, dashboards, load, now_ms, hour):
    load(Telemetry().provider_health(now_ms - MINUTE).signal(now_ms - 3 * 60 * MINUTE, signal="steady", value=4.25))
    assert 4.25 in _series(grafana, dashboards[UID], hour).get("steady", [])


def test_history_plots_the_last_value_per_window_not_a_mean(grafana, dashboards, load, now_ms):
    """Changes are events, not samples: a window shows where the value ended up."""
    window = Window.last(3600, now_ms=now_ms, interval_ms=60_000)
    start = now_ms - 30 * MINUTE - (now_ms % MINUTE)  # one whole one-minute window
    load(Telemetry().signal(start + 1_000, signal="x", value=0.0).signal(start + 2_000, signal="x", value=10.0))
    assert set(_series(grafana, dashboards[UID], window).get("x", [])) == {10.0}


@pytest.mark.parametrize(
    "panel_id, signal, value, drawn", [(1, "double", 2.5, 2.5), (2, "bool", True, 1), (3, "int", 4, 4.0)]
)
def test_a_value_is_drawn_to_the_end_of_the_range(grafana, dashboards, load, hour, panel_id, signal, value, drawn):
    """A value holds until the next change, so the line reaches the range's last window."""
    load(Telemetry().signal(hour.start_ms + 50 * MINUTE, signal=signal, value=value))
    points = []
    for frame in grafana.panel(dashboards[UID], panel_id, hour):
        times = next(c for c in frame.columns if c.name in ("Time", "_time")).values
        for column in frame.value_columns():
            points += [(t, v) for t, v in zip(times, column.values, strict=True) if v is not None]
    assert points, f"panel {panel_id} drew nothing"
    last_time, last_value = max(points)
    assert last_time > hour.stop_ms - hour.interval_ms and last_value == drawn


@pytest.mark.parametrize("signal", ["double", "int", "uint", "bool", "string"])
def test_every_panel_honours_the_signal_filter(grafana, dashboards, typed, hour, signal):
    shown = _series(grafana, dashboards[UID], hour, {"signal_id": [signal]})
    assert set(shown) == {signal}


@pytest.mark.parametrize(
    "panel_id, signal, before, inside, drawn_before, drawn_inside",
    [
        (1, "double", 1.5, 2.5, 1.5, 2.5),
        (2, "bool", False, True, 0, 1),
        (3, "int", 3, 4, 3.0, 4.0),
        (3, "uint", UInt(3), UInt(4), 3.0, 4.0),
    ],
)
def test_a_series_with_points_on_both_sides_of_the_range_start_is_drawn_held(
    grafana, dashboards, load, now_ms, hour, panel_id, signal, before, inside, drawn_before, drawn_inside
):
    """#620: the seed and the changes are one series, not two tables with the same key, and the
    seeded value holds until the change inside the range."""
    t = Telemetry().provider_health(now_ms - MINUTE)
    t.signal(hour.start_ms - 10 * MINUTE, signal=signal, value=before)
    t.signal(hour.start_ms + 30 * MINUTE, signal=signal, value=inside)
    load(t)
    points = []
    for frame in grafana.panel(dashboards[UID], panel_id, hour):
        times = next(c for c in frame.columns if c.name in ("Time", "_time")).values
        for column in frame.value_columns():
            points += [(at, v) for at, v in zip(times, column.values, strict=True) if v is not None]
    points.sort()
    assert points, f"panel {panel_id} drew nothing"
    assert points[0][0] <= hour.start_ms + hour.interval_ms and points[0][1] == drawn_before
    assert points[-1][0] > hour.stop_ms - hour.interval_ms and points[-1][1] == drawn_inside
