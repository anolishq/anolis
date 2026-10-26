"""Signal History: every typed value field shown, every panel honours the filters."""

from __future__ import annotations

import pytest

from tests.observability.harness import Telemetry, UInt
from tests.observability.scenarios import MINUTE

UID = "anolis-signal-history"


def _series(grafana, dashboard, window, selected=None) -> dict[str, list]:
    """signal_id -> values, across every panel."""
    found: dict[str, list] = {}
    for panel in dashboard.panels():
        for frame in grafana.panel(dashboard, panel["id"], window, selected):
            for column in frame.value_columns():
                signal = column.labels.get("signal_id")
                if signal is None and "signal_id" in [c.name for c in frame.columns]:
                    for row in frame.rows():
                        found.setdefault(row["signal_id"], []).append(row.get(column.name))
                    continue
                found.setdefault(signal or column.name, []).extend(v for v in column.values if v is not None)
    return found


@pytest.fixture()
def typed(load, now_ms):
    t = Telemetry()
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


@pytest.mark.parametrize("signal", ["double", "int", "uint", "bool", "string"])
def test_every_panel_honours_the_signal_filter(grafana, dashboards, typed, hour, signal):
    shown = _series(grafana, dashboards[UID], hour, {"signal_id": [signal]})
    assert set(shown) == {signal}
