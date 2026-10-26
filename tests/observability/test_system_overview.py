"""System Overview: counts seen in range, per-signal status, rates."""

from __future__ import annotations

import pytest

from tests.observability.harness import Telemetry, rows
from tests.observability.scenarios import MINUTE

UID = "anolis-system-overview"
PROVIDERS, DEVICES, SIGNALS, CHANGES_PER_SECOND, RATE_BY_PROVIDER, STATUS_TABLE, QUALITY_PIE = 1, 2, 3, 4, 5, 6, 7
DAY = 24 * 60 * MINUTE


def _single(grafana, dashboard, panel_id, window) -> float:
    values = [v for f in grafana.panel(dashboard, panel_id, window) for c in f.value_columns() for v in c.values]
    assert len(values) == 1, f"panel {panel_id}: expected one value, got {values}"
    return float(values[0])


def test_counts_are_what_was_seen_in_range(grafana, dashboards, load, now_ms, hour):
    t = Telemetry()
    at = now_ms - 10 * MINUTE
    for provider, device in (("bread", "dev0"), ("bread", "dev1"), ("ezo", "dev0")):  # dev0 under two providers
        t.provider_health(at, provider=provider)
        t.device_health(at, provider=provider, device=device)
        t.signal(at, provider=provider, device=device, signal="temp", value=1.0)  # one signal id on every device
    t.provider_health(now_ms - 2 * DAY, provider="retired")
    t.device_health(now_ms - 2 * DAY, provider="retired", device="old")
    t.signal(now_ms - 2 * DAY, provider="retired", device="old", signal="gone", value=1.0)
    load(t)
    dashboard = dashboards[UID]
    counts = [_single(grafana, dashboard, p, hour) for p in (PROVIDERS, DEVICES, SIGNALS)]
    assert counts == [2, 3, 3]
    for panel_id in (PROVIDERS, DEVICES, SIGNALS):
        assert "in range" in dashboard.panel(panel_id)["title"].lower(), dashboard.panel(panel_id)["title"]


def test_status_table_lists_each_signal_with_its_last_change(grafana, dashboards, load, now_ms, hour):
    first, second = now_ms - 7 * MINUTE, now_ms - 3 * MINUTE
    t = Telemetry().provider_health(now_ms - MINUTE)
    t.signal(first, signal="a", value=1.0).signal(second, signal="b", quality="FAULT", value=2.0)
    load(t)
    table = rows(grafana.panel(dashboards[UID], STATUS_TABLE, hour))
    seen = sorted((r["Device"], r["Signal"], r["Quality"], r["Last change"]) for r in table)
    assert seen == [("d1", "a", "OK", first), ("d1", "b", "FAULT", second)]


def test_status_table_shows_a_signal_unchanged_since_before_the_range(grafana, dashboards, load, now_ms, hour):
    steady = now_ms - 3 * 60 * MINUTE
    load(Telemetry().provider_health(now_ms - MINUTE).signal(steady, signal="steady", value=1.0))
    table = rows(grafana.panel(dashboards[UID], STATUS_TABLE, hour))
    assert [(r["Signal"], r["Last change"]) for r in table] == [("steady", steady)]


def test_quality_pie_counts_latest_quality_per_signal(grafana, dashboards, load, now_ms, hour):
    t = Telemetry().provider_health(now_ms - MINUTE)
    for device, earlier, latest in (("d1", "FAULT", "OK"), ("d2", "OK", "OK"), ("d3", "OK", "FAULT")):
        t.signal(now_ms - 30 * MINUTE, device=device, quality=earlier, value=1.0)
        t.signal(now_ms - 5 * MINUTE, device=device, quality=latest, value=1.0)
    load(t)
    counts = {}
    for row in rows(grafana.panel(dashboards[UID], QUALITY_PIE, hour)):
        counts.update({k: v for k, v in row.items() if k in {"OK", "STALE", "UNAVAILABLE", "FAULT"} and v})
    assert counts == {"OK": 2, "FAULT": 1}


def test_change_rate_counts_every_signal_row(grafana, dashboards, load, now_ms, hour):
    """Quality-only rows (a non-finite double) are changes too."""
    t = Telemetry()
    for i in range(5):
        t.signal(now_ms - 10_000 - i * 1000, signal=f"s{i}", value=float(i))
    t.signal(now_ms - 20_000, signal="nan", quality="FAULT")
    load(t)
    assert _single(grafana, dashboards[UID], CHANGES_PER_SECOND, hour) == pytest.approx(6 / 60)
    assert "change" in dashboards[UID].panel(CHANGES_PER_SECOND)["title"].lower()


def test_change_rate_by_provider_is_one_series_per_provider(grafana, dashboards, load, now_ms, hour):
    t = Telemetry()
    for i in range(3):
        t.signal(now_ms - 5 * MINUTE, provider="bread", signal=f"s{i}", value=1.0)
    t.signal(now_ms - 5 * MINUTE, provider="ezo", signal="s0", value=1.0)
    load(t)
    series = [c for f in grafana.panel(dashboards[UID], RATE_BY_PROVIDER, hour) for c in f.value_columns()]
    providers = sorted(c.labels.get("provider_id", c.name) for c in series)
    assert providers == ["bread", "ezo"]
    peak = {c.labels.get("provider_id", c.name): max(v for v in c.values if v is not None) for c in series}
    assert peak == {"bread": pytest.approx(3 / 10), "ezo": pytest.approx(1 / 10)}
