"""Latest-state panels show a runtime's signals only while it reports health."""

from __future__ import annotations

import pytest

from tests.observability.harness import Telemetry, rows
from tests.observability.scenarios import MINUTE

QUALITIES = ("OK", "STALE", "UNAVAILABLE", "FAULT")


@pytest.fixture()
def stopped(load, now_ms):
    """r1 reports health in range; r2 stopped hours ago, its signals steady since."""
    t = Telemetry()
    old = now_ms - 3 * 60 * MINUTE
    t.provider_health(now_ms - 5 * MINUTE, runtime="r1")
    t.provider_health(old, runtime="r2")
    for runtime in ("r1", "r2"):
        t.signal(old, runtime=runtime, signal="double", value=1.5)
        t.signal(old, runtime=runtime, signal="bool", value=True)
        t.signal(old, runtime=runtime, signal="int", value=3)
        t.signal(old, runtime=runtime, signal="string", value="RUNNING")
    for quality in QUALITIES[1:]:
        t.signal(old, runtime="r2", signal=f"q_{quality}", quality=quality, value=1.0)
    load(t)


def _values(grafana, dashboard, panel_id, window) -> list:
    return [v for f in grafana.panel(dashboard, panel_id, window) for c in f.value_columns() for v in c.values]


def _runtimes(frames) -> set[str]:
    """runtime_name of every row or series: a table column, or a series label."""
    found: set[str] = set()
    for frame in frames:
        names = [c.name for c in frame.columns]
        for key in ("Runtime", "runtime_name"):
            if key in names:
                found |= {str(v) for v in frame.column(key).values}
        found |= {c.labels["runtime_name"] for c in frame.value_columns() if "runtime_name" in c.labels}
    return found


@pytest.mark.parametrize(
    "uid, panel_id",
    [
        ("anolis-device-health", 1),
        ("anolis-system-overview", 6),
        ("anolis-signal-history", 1),
        ("anolis-signal-history", 2),
        ("anolis-signal-history", 3),
        ("anolis-signal-history", 4),
    ],
)
def test_a_stopped_runtime_drops_out_of_latest_state(grafana, dashboards, stopped, hour, uid, panel_id):
    assert _runtimes(grafana.panel(dashboards[uid], panel_id, hour)) == {"r1"}


def test_quality_stats_count_only_live_runtimes(grafana, dashboards, stopped, hour):
    dashboard = dashboards["anolis-device-health"]
    counts = {}
    for quality in QUALITIES:
        panel = next(p for p in dashboard.panels() if p["title"] == f"Signals {quality}")
        values = _values(grafana, dashboard, panel["id"], hour)
        counts[quality] = values[0] if values else 0
    assert counts == {"OK": 4, "STALE": 0, "UNAVAILABLE": 0, "FAULT": 0}


def test_quality_over_time_seeds_only_live_runtimes(grafana, dashboards, stopped, hour):
    last = {}
    for frame in grafana.panel(dashboards["anolis-device-health"], 5, hour):
        for column in frame.value_columns():
            points = [v for v in column.values if v is not None]
            if points:
                last[column.labels.get("quality", column.name)] = points[-1]
    assert last == {"OK": 4}


def test_quality_pie_counts_only_live_runtimes(grafana, dashboards, stopped, hour):
    counts = {}
    for row in rows(grafana.panel(dashboards["anolis-system-overview"], 7, hour)):
        counts.update({k: v for k, v in row.items() if k in QUALITIES and v})
    assert counts == {"OK": 4}
