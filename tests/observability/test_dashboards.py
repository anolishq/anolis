"""Properties every bundled dashboard must hold, checked by executing it."""

from __future__ import annotations

import json
import pathlib
import re
from typing import Any

import pytest

from tests.observability.conftest import DASHBOARD_UIDS
from tests.observability.harness import Frame, Grafana, QueryError, Window
from tests.observability.scenarios import MINUTE, machine


def _queries(model: dict[str, Any]) -> list[str]:
    found: list[str] = []

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "query" and isinstance(value, str):
                    found.append(value)
                walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(model)
    return found


def _run_everything(grafana: Grafana, dashboard, window: Window, selected=None) -> dict[str, list[Frame]]:
    results: dict[str, list[Frame]] = {}
    errors: list[str] = []
    for panel in dashboard.panels():
        try:
            results[f"panel {panel['id']} {panel['title']}"] = grafana.panel(dashboard, panel["id"], window, selected)
        except QueryError as exc:
            errors.append(f"panel {panel['id']} {panel['title']!r}: {exc}")
    for annotation in dashboard.annotations():
        try:
            results[f"annotation {annotation['name']}"] = grafana.annotation(
                dashboard, annotation["name"], window, selected
            )
        except QueryError as exc:
            errors.append(f"annotation {annotation['name']!r}: {exc}")
    assert not errors, "\n\n".join(errors)
    return results


def _normalise(frames: list[Frame]) -> str:
    shaped = sorted(
        json.dumps([[c.name, sorted(c.labels.items()), c.values] for c in frame.columns], sort_keys=True, default=str)
        for frame in frames
    )
    return "\n".join(shaped)


def test_every_dashboard_is_provisioned(dashboards):
    assert sorted(dashboards) == sorted(DASHBOARD_UIDS)


@pytest.mark.parametrize("uid", DASHBOARD_UIDS)
def test_no_query_names_a_bucket(dashboards, uid):
    """Every query reads the datasource's configured bucket (INFLUX_BUCKET)."""
    literal = [q for q in _queries(dashboards[uid].model) if re.search(r"bucket:\s*\"", q)]
    assert not literal, f"{len(literal)} queries name a bucket instead of v.defaultBucket:\n" + "\n---\n".join(literal)


@pytest.mark.parametrize("uid", DASHBOARD_UIDS)
def test_every_query_executes_on_a_busy_machine(grafana, dashboards, load, now_ms, hour, uid):
    load(machine(now_ms))
    _run_everything(grafana, dashboards[uid], hour)


@pytest.mark.parametrize("uid", DASHBOARD_UIDS)
def test_every_query_executes_on_a_quiet_window(grafana, dashboards, load, now_ms, uid):
    """Nothing written inside the range: panels show no data, never an error."""
    load(machine(now_ms - 120 * MINUTE))
    _run_everything(grafana, dashboards[uid], Window.last(3600, now_ms=now_ms))


@pytest.mark.parametrize("uid", DASHBOARD_UIDS)
def test_every_dashboard_filters_by_runtime(dashboards, uid):
    variable = dashboards[uid].variables.get("runtime_name")
    assert variable is not None, f"{uid} has no runtime_name variable"
    assert variable["type"] == "query" and variable.get("multi") and variable.get("includeAll")


@pytest.mark.parametrize("uid", DASHBOARD_UIDS)
def test_a_second_runtime_does_not_change_the_view(grafana, dashboards, load, now_ms, hour, uid):
    """With runtime r1 selected, adding r2 under the same ids changes nothing on screen."""
    assert "runtime_name" in dashboards[uid].variables, f"{uid} has no runtime_name variable"
    selected = {"runtime_name": ["r1"]}

    load(machine(now_ms))
    alone = _run_everything(grafana, dashboards[uid], hour, selected)
    load(machine(now_ms, telemetry=machine(now_ms, runtime="r2", offset=1000.0)))
    shared = _run_everything(grafana, dashboards[uid], hour, selected)

    changed = [name for name in alone if _normalise(alone[name]) != _normalise(shared.get(name, []))]
    assert not changed, f"r2 leaks into: {changed}"


def _seconds(refresh: str) -> int:
    match = re.fullmatch(r"(\d+)([smh])", refresh)
    assert match, refresh
    return int(match.group(1)) * {"s": 1, "m": 60, "h": 3600}[match.group(2)]


def test_signal_history_refreshes_no_faster_than_a_pi_draws_it() -> None:
    # On a Raspberry Pi the Double panel's query took 3.1 s alone, and the whole
    # dashboard about 18 s to draw: at a 5 s refresh each reload cancelled the
    # last, and the panel never drew (#647).
    path = pathlib.Path(__file__).parents[2] / "observability" / "grafana" / "dashboards" / "signal-history.json"
    model = json.loads(path.read_text(encoding="utf-8"))
    assert _seconds(model["refresh"]) >= 30
