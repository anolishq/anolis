"""The process dashboard generator's output, without Grafana: grouping, identity, fallbacks."""

from __future__ import annotations

import copy
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import jsonschema
import process_dashboard as gen
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).with_name("fixtures")
GENERATOR = REPO / "tools" / "process-dashboard" / "process_dashboard.py"


def inventory(name: str) -> dict[str, Any]:
    document: dict[str, Any] = json.loads((FIXTURES / f"inventory-{name}.json").read_text())
    return document


def signal_panels(dashboard: dict[str, Any]) -> list[dict[str, Any]]:
    return [p for p in dashboard["panels"] if p["type"] not in ("row",) and p["title"] != "Signal quality"]


def queries(panel: dict[str, Any]) -> list[str]:
    return [t["query"] for t in panel.get("targets", [])]


def identity(query: str) -> dict[str, str]:
    """The tag values a query pins with ==, from its selection of anolis_signal."""
    return dict(re.findall(r'r\.(runtime_name|provider_id|device_id|signal_id) == "([^"]*)"', query))


def add_device(document: dict[str, Any], provider: str, device: str, signals: list[dict[str, Any]]) -> None:
    document["devices"]["devices"].append(
        {"provider_id": provider, "device_id": device, "display_name": device, "type": "test.device"}
    )
    document["capabilities"].append(
        {
            "status": {"code": "OK", "message": "ok"},
            "provider_id": provider,
            "device_id": device,
            "capabilities": {"signals": signals, "functions": []},
        }
    )


@pytest.mark.parametrize("name", ["bioreactor", "sim"])
def test_fixtures_are_valid_http_responses(name):
    spec = yaml.safe_load((REPO / "schemas/http/runtime-http.openapi.v0.yaml").read_text())
    document = inventory(name)

    def check(body: Any, schema: str) -> None:
        jsonschema.validate(body, {"$ref": f"#/components/schemas/{schema}", "components": spec["components"]})

    check(document["devices"], "DevicesResponse")
    for response in document["capabilities"]:
        check(response, "DeviceCapabilitiesResponse")


@pytest.mark.parametrize("name", ["bioreactor", "sim"])
def test_every_signal_gets_exactly_one_panel_of_its_own(name):
    document = inventory(name)
    dashboard = gen.generate(document, "r1")
    declared = sorted(
        (c["provider_id"], c["device_id"], s["signal_id"])
        for c in document["capabilities"]
        for s in c["capabilities"]["signals"]
    )
    shown = []
    for panel in signal_panels(dashboard):
        if panel["type"] == "text":
            continue
        (query,) = queries(panel)
        pinned = identity(query)
        shown.append((pinned["provider_id"], pinned["device_id"], pinned["signal_id"]))
    assert sorted(shown) == declared


def test_dissolved_oxygen_concentration_and_saturation_never_share_a_panel():
    dashboard = gen.generate(inventory("bioreactor"), "r1")
    do_panels = [p for p in signal_panels(dashboard) if "do0" in json.dumps(p.get("targets"))]
    assert sorted(identity(queries(p)[0])["signal_id"] for p in do_panels) == ["do_mg_l", "do_saturation_pct"]
    units = {identity(queries(p)[0])["signal_id"]: p["fieldConfig"]["defaults"]["unit"] for p in do_panels}
    assert units == {"do_mg_l": "suffix: mg/L", "do_saturation_pct": "suffix: %"}


def test_same_unit_signals_on_one_device_are_not_combined():
    """t1_c, t2_c, setpoint1_c and setpoint2_c are all C; each is its own panel."""
    dashboard = gen.generate(inventory("bioreactor"), "r1")
    celsius = [
        p for p in signal_panels(dashboard) if p.get("fieldConfig", {}).get("defaults", {}).get("unit") == "suffix: C"
    ]
    assert sorted(identity(queries(p)[0])["signal_id"] for p in celsius) == [
        "setpoint1_c",
        "setpoint2_c",
        "t1_c",
        "t2_c",
    ]


def test_the_same_signal_id_on_two_devices_is_two_panels_each_pinned_to_its_device():
    dashboard = gen.generate(inventory("sim"), "r1")
    tc1 = [identity(queries(p)[0]) for p in signal_panels(dashboard) if p["title"] == "TC1 Temperature"]
    assert sorted(i["device_id"] for i in tc1) == ["tempctl0", "tempctl1"]
    rows = [p["title"] for p in dashboard["panels"] if p["type"] == "row"]
    assert sum("sim0/tempctl0" in r for r in rows) == 1 and sum("sim0/tempctl1" in r for r in rows) == 1


def test_queries_pin_the_runtime_and_two_runtimes_get_two_dashboards():
    a = gen.generate(inventory("sim"), "line-a")
    b = gen.generate(inventory("sim"), "line-b")
    assert a["uid"] != b["uid"]
    for dashboard, runtime in ((a, "line-a"), (b, "line-b")):
        for panel in dashboard["panels"]:
            for query in queries(panel):
                assert identity(query)["runtime_name"] == runtime
                assert "runtime_name ==" in query and "=~" not in query


@pytest.mark.parametrize(
    "value_type, panel_type",
    [
        ("double", "timeseries"),
        ("int64", "timeseries"),
        ("uint64", "timeseries"),
        ("bool", "state-timeline"),
        ("string", "state-timeline"),
        ("bytes", "text"),
        ("unknown", "text"),
    ],
)
def test_each_value_type_gets_a_presentation_and_none_is_silently_dropped(value_type, panel_type):
    document = inventory("sim")
    add_device(document, "p", "d", [{"signal_id": "s", "value_type": value_type, "label": "S"}])
    panel_id = gen._stable_id("signal", "p", "d", "s")
    (panel,) = [p for p in gen.generate(document, "r1")["panels"] if p["id"] == panel_id]
    assert panel["type"] == panel_type
    if panel_type == "text":
        assert value_type in panel["options"]["content"] or "bytes" in panel["options"]["content"]
        assert "not plotted" in panel["options"]["content"]
    else:
        assert identity(queries(panel)[0])["signal_id"] == "s"


def test_integers_are_drawn_without_decimals_and_uint64_says_where_precision_ends():
    dashboard = gen.generate(inventory("bioreactor"), "r1")
    by_signal = {identity(queries(p)[0])["signal_id"]: p for p in signal_panels(dashboard) if p["type"] == "timeseries"}
    assert by_signal["motor1_value"]["fieldConfig"]["defaults"]["decimals"] == 0
    assert "2^53" in by_signal["period1_ms"]["description"]
    assert "decimals" not in by_signal["t1_c"]["fieldConfig"]["defaults"]


def test_a_signal_without_a_unit_says_so_and_gets_no_invented_one():
    dashboard = gen.generate(inventory("sim"), "r1")
    (duty,) = [p for p in signal_panels(dashboard) if p["title"] == "Motor 1 Duty"]
    defaults = duty["fieldConfig"]["defaults"]
    assert "unit" not in defaults
    assert defaults["custom"]["axisLabel"] == "no unit declared"
    assert "no unit declared" in duty["description"]


def test_no_ranges_or_thresholds_are_invented():
    for name in ("bioreactor", "sim"):
        for panel in signal_panels(gen.generate(inventory(name), "r1")):
            defaults = panel.get("fieldConfig", {}).get("defaults", {})
            assert "min" not in defaults and "max" not in defaults, panel["title"]
            assert defaults.get("thresholds", {}).get("steps", [{}])[1:] == [], panel["title"]


def test_booleans_carry_no_good_or_bad_colour():
    dashboard = gen.generate(inventory("bioreactor"), "r1")
    colours = set()
    for panel in signal_panels(dashboard):
        for mapping in panel.get("fieldConfig", {}).get("defaults", {}).get("mappings", []):
            colours |= {o["color"] for o in mapping["options"].values()}
    assert colours and not colours & {"green", "red"}


def test_identical_input_gives_identical_output_whatever_the_listing_order():
    document = inventory("bioreactor")
    first = gen.render(gen.generate(document, "r1"))
    reordered = copy.deepcopy(document)
    reordered["devices"]["devices"].reverse()
    reordered["capabilities"].reverse()
    assert gen.render(gen.generate(document, "r1")) == first
    assert gen.render(gen.generate(reordered, "r1")) == first


def test_adding_a_device_keeps_every_existing_panel_and_its_id():
    document = inventory("sim")
    before = {p["id"]: (p["title"], queries(p)) for p in gen.generate(document, "r1")["panels"]}
    # "a0" sorts before every existing device, so positional ids would all move.
    add_device(document, "a0", "extra0", [{"signal_id": "x", "value_type": "double", "label": "X", "unit": "V"}])
    after = {p["id"]: (p["title"], queries(p)) for p in gen.generate(document, "r1")["panels"]}
    assert {k: after[k] for k in before} == before
    assert len(after) == len(before) + 3  # a row, the signal, the device's quality panel


def test_panel_ids_are_unique_and_grid_positions_do_not_overlap():
    dashboard = gen.generate(inventory("bioreactor"), "r1")
    ids = [p["id"] for p in dashboard["panels"]]
    assert len(ids) == len(set(ids))
    cells = set()
    for panel in dashboard["panels"]:
        g = panel["gridPos"]
        for x in range(g["x"], g["x"] + g["w"]):
            for y in range(g["y"], g["y"] + g["h"]):
                assert (x, y) not in cells, panel["title"]
                cells.add((x, y))


@pytest.mark.parametrize(
    "mutate, message",
    [
        (lambda d: d["capabilities"].pop(), "has no capabilities response"),
        (lambda d: d["devices"]["devices"].append(d["devices"]["devices"][0]), "listed twice"),
        (lambda d: d["devices"]["devices"].pop(), "not in the device list"),
        (
            lambda d: d["capabilities"][0]["capabilities"]["signals"].append(
                d["capabilities"][0]["capabilities"]["signals"][0]
            ),
            "more than once",
        ),
        (lambda d: d.pop("capabilities"), "not an inventory document"),
    ],
)
def test_an_inconsistent_inventory_is_refused(mutate, message):
    document = inventory("sim")
    mutate(document)
    with pytest.raises(gen.InventoryError, match=message):
        gen.generate(document, "r1")


def test_runtime_names_are_escaped_in_flux_strings():
    dashboard = gen.generate(inventory("sim"), 'odd "name" \\ ${x}')
    query = queries(signal_panels(dashboard)[0])[0]
    assert r'r.runtime_name == "odd \"name\" \\ \${x}"' in query


def test_the_command_line_writes_the_dashboard_and_refuses_a_bad_inventory(tmp_path):
    out = tmp_path / "dashboard.json"
    fixture = FIXTURES / "inventory-sim.json"
    run = subprocess.run(
        [sys.executable, str(GENERATOR), str(fixture), "--runtime-name", "r1", "-o", str(out)],
        capture_output=True,
        text=True,
    )
    assert run.returncode == 0, run.stderr
    assert json.loads(out.read_text()) == gen.generate(inventory("sim"), "r1")

    bad = tmp_path / "bad.json"
    bad.write_text("{}")
    run = subprocess.run(
        [sys.executable, str(GENERATOR), str(bad), "--runtime-name", "r1"], capture_output=True, text=True
    )
    assert run.returncode == 2 and "not an inventory document" in run.stderr
