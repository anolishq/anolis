"""Device Health: quality against the locked contract, and real freshness."""

from __future__ import annotations

import re

from tests.observability.harness import Telemetry, rows
from tests.observability.scenarios import MINUTE

UID = "anolis-device-health"
SIGNAL_TABLE = 1
QUALITY_OVER_TIME = 5
STALENESS = 6
CONTRACT_QUALITIES = {"OK", "STALE", "UNAVAILABLE", "FAULT"}


def _stat(grafana, dashboard, title, window) -> float:
    panel = next((p for p in dashboard.panels() if p["title"] == title), None)
    assert panel is not None, f"no panel titled {title!r}; panels: {[p['title'] for p in dashboard.panels()]}"
    values = [
        v for frame in grafana.panel(dashboard, panel["id"], window) for c in frame.value_columns() for v in c.values
    ]
    assert len(values) <= 1, f"{title}: expected one value, got {values}"
    return values[0] if values else 0


def _signals_by_quality(now_ms: int) -> Telemetry:
    t = Telemetry().provider_health(now_ms - MINUTE)
    # Each signal's earlier quality differs from its latest one, so only the latest may count.
    latest = {"OK": 2, "STALE": 1, "UNAVAILABLE": 1, "FAULT": 3}
    n = 0
    for quality, count in latest.items():
        for _ in range(count):
            n += 1
            t.signal(now_ms - 40 * MINUTE, device=f"d{n}", quality="OK" if quality != "OK" else "FAULT", value=1.0)
            t.signal(now_ms - 5 * MINUTE, device=f"d{n}", quality=quality, value=1.0)
    return t


def test_quality_stats_count_each_contract_value(grafana, dashboards, load, now_ms, hour):
    load(_signals_by_quality(now_ms))
    dashboard = dashboards[UID]
    counts = {q: _stat(grafana, dashboard, f"Signals {q}", hour) for q in sorted(CONTRACT_QUALITIES)}
    assert counts == {"OK": 2, "STALE": 1, "UNAVAILABLE": 1, "FAULT": 3}


def test_no_panel_filters_on_a_quality_outside_the_contract(dashboards):
    used = set()
    for panel in dashboards[UID].panels():
        for target in panel.get("targets", []):
            used |= set(re.findall(r'_value\s*==\s*"([A-Z_]+)"', target.get("query", "")))
    assert used <= CONTRACT_QUALITIES, f"not contract values: {sorted(used - CONTRACT_QUALITIES)}"


def test_quality_over_time_counts_latest_quality_per_signal(grafana, dashboards, load, now_ms, hour):
    load(_signals_by_quality(now_ms))
    frames = grafana.panel(dashboards[UID], QUALITY_OVER_TIME, hour)
    last = {}
    for frame in frames:
        for column in frame.value_columns():
            points = [v for v in column.values if v is not None]
            if points:
                last[column.labels.get("quality", column.name)] = points[-1]
    assert last == {"OK": 2, "STALE": 1, "UNAVAILABLE": 1, "FAULT": 3}


def test_staleness_gauge_shows_device_health_staleness(grafana, dashboards, load, now_ms, hour):
    """The gauge reads the runtime's staleness_ms, not the age of a signal's last change."""
    t = Telemetry()
    t.device_health(now_ms - 10 * MINUTE, device="d1", staleness_ms=9000)
    t.device_health(now_ms - MINUTE, device="d1", staleness_ms=1500)
    t.device_health(now_ms - MINUTE, device="d2", staleness_ms=4200)
    t.signal(now_ms - 50 * MINUTE, device="d1", value=1.0)  # unchanged for 50 minutes, yet fresh
    load(t)
    frames = grafana.panel(dashboards[UID], STALENESS, hour)
    values = sorted(
        v for frame in frames for c in frame.value_columns() for v in c.values if isinstance(v, (int, float))
    )
    assert values == [1500, 4200]


def test_signal_table_shows_a_signal_unchanged_since_before_the_range(grafana, dashboards, load, now_ms, hour):
    """Rows are written on change: a steady signal's state is its last row, however old."""
    steady, recent = now_ms - 3 * 60 * MINUTE, now_ms - 3 * MINUTE
    load(
        Telemetry()
        .provider_health(now_ms - MINUTE)
        .signal(steady, signal="steady", value=1.0)
        .signal(recent, signal="recent", value=2.0)
    )
    table = {row["Signal"]: row["Last change"] for row in rows(grafana.panel(dashboards[UID], SIGNAL_TABLE, hour))}
    assert table == {"steady": steady, "recent": recent}


def test_signal_table_reports_last_change_in_milliseconds(grafana, dashboards, load, now_ms, hour):
    first, second = now_ms - 7 * MINUTE, now_ms - 3 * MINUTE
    load(
        Telemetry()
        .provider_health(now_ms - MINUTE)
        .signal(first, signal="a", value=1.0)
        .signal(second, signal="b", value=2.0)
    )
    table = {row["Signal"]: row["Last change"] for row in rows(grafana.panel(dashboards[UID], SIGNAL_TABLE, hour))}
    assert table == {"a": first, "b": second}


def test_quality_over_time_with_a_signal_seen_on_both_sides_of_the_range_start(grafana, dashboards, load, now_ms, hour):
    """#620: a signal with a quality before the range and a change inside it is one series, held
    from the range start until the change."""
    t = Telemetry().provider_health(now_ms - MINUTE)
    t.signal(hour.start_ms - 10 * MINUTE, quality="OK", value=1.0)
    t.signal(hour.start_ms + 30 * MINUTE, quality="FAULT", value=1.0)
    load(t)
    points = []
    for frame in grafana.panel(dashboards[UID], QUALITY_OVER_TIME, hour):
        times = next(c for c in frame.columns if c.name in ("Time", "_time")).values
        for column in frame.value_columns():
            label = column.labels.get("quality", column.name)
            points += [(at, label, v) for at, v in zip(times, column.values, strict=True) if v is not None]
    points.sort()
    assert points, "nothing drawn"
    assert points[0][0] <= hour.start_ms + hour.interval_ms and points[0][1:] == ("OK", 1)
    assert points[-1][0] > hour.stop_ms - hour.interval_ms and points[-1][1:] == ("FAULT", 1)
