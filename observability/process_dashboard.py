#!/usr/bin/env python3
"""Generate a first process dashboard for one runtime from its saved inventory.

Prototype for anolishq/anolis#477. The input is the runtime's HTTP inventory,
saved verbatim: the body of ``GET /v0/devices`` and of each device's
``GET /v0/devices/{provider_id}/{device_id}/capabilities``::

    {"devices": <GET /v0/devices>, "capabilities": [<capabilities>, ...]}

The runtime name is an argument, because the HTTP API does not report it.

Grouping is deliberately conservative: a row per device and a panel per
signal. Nothing is combined on an axis, whatever the units, and no setpoint is
paired with a measurement, because nothing in the metadata says which belong
together. Declared units are shown verbatim; no range, threshold or colour
meaning is invented. Standard library only, so it runs wherever the
observability tarball is unpacked.

    python3 observability/process_dashboard.py inventory.json --runtime-name NAME -o dashboard.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# value_type -> the anolis_signal field core/telemetry writes it to.
NUMERIC_FIELDS = {"double": "value_double", "int64": "value_int", "uint64": "value_uint"}
STATE_FIELDS = {"bool": "value_bool", "string": "value_string"}
QUALITIES = ("OK", "STALE", "UNAVAILABLE", "FAULT")

DATASOURCE = {"type": "influxdb", "uid": "${DS_INFLUXDB}"}
WIDTH, HEIGHT = 12, 8


class InventoryError(ValueError):
    pass


@dataclass(frozen=True)
class Signal:
    signal_id: str
    value_type: str
    label: str
    unit: str | None
    description: str | None


@dataclass(frozen=True)
class Device:
    provider_id: str
    device_id: str
    display_name: str
    type: str
    signals: tuple[Signal, ...]


def load_inventory(document: dict[str, Any]) -> list[Device]:
    """Devices sorted by identity, each with its signals in declared order."""
    try:
        return _load(document)
    except (KeyError, TypeError) as exc:
        raise InventoryError(f"not an inventory document: missing or malformed {exc}") from exc


def _load(document: dict[str, Any]) -> list[Device]:
    listed = document["devices"]["devices"]
    responses = document["capabilities"]
    capabilities: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for response in responses:
        key = (response["provider_id"], response["device_id"])
        if key in capabilities:
            raise InventoryError(f"capabilities for {key[0]}/{key[1]} appear twice")
        capabilities[key] = response["capabilities"]["signals"]

    devices: dict[tuple[str, str], Device] = {}
    for entry in listed:
        key = (entry["provider_id"], entry["device_id"])
        if key in devices:
            raise InventoryError(f"device {key[0]}/{key[1]} is listed twice")
        if key not in capabilities:
            raise InventoryError(f"device {key[0]}/{key[1]} has no capabilities response")
        signals = tuple(
            Signal(s["signal_id"], s["value_type"], s["label"], s.get("unit"), s.get("description"))
            for s in capabilities[key]
        )
        ids = [s.signal_id for s in signals]
        duplicated = sorted({i for i in ids if ids.count(i) > 1})
        if duplicated:
            raise InventoryError(f"device {key[0]}/{key[1]} declares {duplicated} more than once")
        devices[key] = Device(key[0], key[1], entry["display_name"], entry["type"], signals)

    unlisted = sorted(set(capabilities) - set(devices))
    if unlisted:
        raise InventoryError(f"capabilities for devices not in the device list: {unlisted}")
    return [devices[key] for key in sorted(devices)]


def _flux_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("${", "\\${")
    return f'"{escaped}"'


def _stable_id(*parts: str) -> int:
    """A panel id derived from identity, so adding a device renumbers nothing."""
    digest = hashlib.sha256("\x1f".join(parts).encode()).hexdigest()
    return int(digest[:7], 16) + 1


def _series_query(runtime: str, device: Device, field: str, signal_id: str | None, convert: str) -> str:
    """Seed with the last value before the range, then the last value per window, held.

    Rows are written only on change. A value unchanged since before the range
    is seeded from up to 30 days back, but only while the runtime reported
    provider health in the range: nothing is written when a runtime stops, so
    its last values would otherwise stand.
    """
    identity = [
        f"r.runtime_name == {_flux_string(runtime)}",
        f"r.provider_id == {_flux_string(device.provider_id)}",
        f"r.device_id == {_flux_string(device.device_id)}",
    ]
    if signal_id is not None:
        identity.append(f"r.signal_id == {_flux_string(signal_id)}")
    selection = (
        f'  |> filter(fn: (r) => r._measurement == "anolis_signal" and r._field == {_flux_string(field)})\n'
        + "".join(f"  |> filter(fn: (r) => {term})\n" for term in identity)
    )
    return (
        'import "date"\n\n'
        "live = from(bucket: v.defaultBucket)\n"
        "  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n"
        '  |> filter(fn: (r) => r._measurement == "anolis_provider_health" and r._field == "state")\n'
        f"  |> filter(fn: (r) => {identity[0]})\n"
        '  |> keep(columns: ["runtime_name"])\n'
        "  |> group()\n"
        '  |> distinct(column: "runtime_name")\n'
        '  |> findColumn(fn: (key) => true, column: "_value")\n\n'
        "seed = from(bucket: v.defaultBucket)\n"
        "  |> range(start: date.sub(d: 30d, from: v.timeRangeStart), stop: v.timeRangeStart)\n"
        f"{selection}"
        "  |> filter(fn: (r) => contains(value: r.runtime_name, set: live))\n"
        "  |> last()\n"
        "  |> map(fn: (r) => ({r with _time: v.timeRangeStart}))\n\n"
        "changes = from(bucket: v.defaultBucket)\n"
        "  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n"
        f"{selection}\n"
        "union(tables: [seed, changes])\n"
        "  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n"
        '  |> group(columns: ["_start", "_stop", "_measurement", "_field", '
        '"runtime_name", "provider_id", "device_id", "signal_id"])\n'
        '  |> sort(columns: ["_time"])\n'
        f"{convert}"
        "  |> aggregateWindow(every: v.windowPeriod, fn: last, createEmpty: true)\n"
        "  |> fill(usePrevious: true)\n"
        "  |> filter(fn: (r) => exists r._value)"
    )


def _describe(device: Device, signal: Signal) -> str:
    unit = f"unit `{signal.unit}`" if signal.unit else "no unit declared"
    lines = [signal.description or "No description declared."]
    lines.append(f"`{device.provider_id}/{device.device_id}/{signal.signal_id}` · `{signal.value_type}` · {unit}")
    if signal.value_type == "uint64":
        lines.append("Values above 2^53 lose precision: Grafana plots numbers as 64-bit floats.")
    return "\n\n".join(lines)


def _target(query: str) -> list[dict[str, Any]]:
    return [{"datasource": DATASOURCE, "query": query, "refId": "A"}]


def _numeric_panel(runtime: str, device: Device, signal: Signal) -> dict[str, Any]:
    defaults: dict[str, Any] = {
        "color": {"mode": "palette-classic"},
        "custom": {
            "axisLabel": signal.unit or "no unit declared",
            "drawStyle": "line",
            "lineInterpolation": "stepAfter",
            "lineWidth": 2,
            "showPoints": "never",
            "spanNulls": False,
        },
        "displayName": signal.label,
        "mappings": [],
        "thresholds": {"mode": "absolute", "steps": [{"color": "text", "value": None}]},
    }
    if signal.unit:
        defaults["unit"] = f"suffix: {signal.unit}"
    if signal.value_type != "double":
        defaults["decimals"] = 0
    return {
        "type": "timeseries",
        "title": signal.label,
        "description": _describe(device, signal),
        "datasource": DATASOURCE,
        "fieldConfig": {"defaults": defaults, "overrides": []},
        "options": {
            "legend": {"calcs": ["lastNotNull", "min", "max"], "displayMode": "list", "placement": "bottom"},
            "tooltip": {"mode": "single"},
        },
        "targets": _target(_series_query(runtime, device, NUMERIC_FIELDS[signal.value_type], signal.signal_id, "")),
    }


def _state_panel(runtime: str, device: Device, signal: Signal) -> dict[str, Any]:
    mappings: list[dict[str, Any]] = []
    convert = ""
    if signal.value_type == "bool":
        # Neutral colours: whether true is good depends on the signal (an e-stop).
        convert = "  |> map(fn: (r) => ({r with _value: if r._value then 1 else 0}))\n"
        mappings = [
            {
                "type": "value",
                "options": {
                    "0": {"index": 0, "text": "false", "color": "text"},
                    "1": {"index": 1, "text": "true", "color": "blue"},
                },
            }
        ]
    return {
        "type": "state-timeline",
        "title": signal.label,
        "description": _describe(device, signal),
        "datasource": DATASOURCE,
        "fieldConfig": {
            "defaults": {
                "color": {"mode": "palette-classic"},
                "displayName": signal.label,
                "mappings": mappings,
                "thresholds": {"mode": "absolute", "steps": [{"color": "text", "value": None}]},
            },
            "overrides": [],
        },
        "options": {"legend": {"showLegend": False}, "showValue": "auto", "mergeValues": True},
        "targets": _target(_series_query(runtime, device, STATE_FIELDS[signal.value_type], signal.signal_id, convert)),
    }


def _unplotted_panel(device: Device, signal: Signal) -> dict[str, Any]:
    if signal.value_type == "bytes":
        reason = "Telemetry stores a `bytes` value as a string of raw bytes; there is no faithful plot of it."
    else:
        reason = f"Value type `{signal.value_type}` is not one this generator knows how to plot."
    return {
        "type": "text",
        "title": f"{signal.label} (not plotted)",
        "description": _describe(device, signal),
        "options": {
            "mode": "markdown",
            "content": f"**{signal.label}** (`{signal.signal_id}`) is not plotted.\n\n{reason}",
        },
    }


def _quality_panel(runtime: str, device: Device) -> dict[str, Any]:
    colours = {"OK": "green", "STALE": "yellow", "UNAVAILABLE": "text", "FAULT": "red"}
    return {
        "type": "state-timeline",
        "title": "Signal quality",
        "description": f"The reported quality of every signal on `{device.provider_id}/{device.device_id}`.",
        "datasource": DATASOURCE,
        "fieldConfig": {
            "defaults": {
                "color": {"mode": "thresholds"},
                "displayName": "${__field.labels.signal_id}",
                "mappings": [
                    {
                        "type": "value",
                        "options": {q: {"index": i, "text": q, "color": colours[q]} for i, q in enumerate(QUALITIES)},
                    }
                ],
                "thresholds": {"mode": "absolute", "steps": [{"color": "text", "value": None}]},
            },
            "overrides": [],
        },
        "options": {"legend": {"showLegend": False}, "showValue": "never", "mergeValues": True},
        "targets": _target(_series_query(runtime, device, "quality", None, "")),
    }


def _signal_panel(runtime: str, device: Device, signal: Signal) -> dict[str, Any]:
    if signal.value_type in NUMERIC_FIELDS:
        return _numeric_panel(runtime, device, signal)
    if signal.value_type in STATE_FIELDS:
        return _state_panel(runtime, device, signal)
    return _unplotted_panel(device, signal)


def _annotations(runtime: str) -> list[dict[str, Any]]:
    # Rows from runtimes before the runtime_name tag carry none; show them too.
    scope = f"  |> filter(fn: (r) => not exists r.runtime_name or r.runtime_name == {_flux_string(runtime)})\n"
    pivot = '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
    rng = "from(bucket: v.defaultBucket)\n  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n"
    return [
        {
            "datasource": DATASOURCE,
            "enable": True,
            "iconColor": "rgba(255, 96, 96, 1)",
            "name": "Mode Changes",
            "query": rng
            + '  |> filter(fn: (r) => r._measurement == "mode_change")\n'
            + scope
            + pivot
            + '  |> map(fn: (r) => ({time: r._time, text: "Mode: " + r.previous_mode + " -> " + r.new_mode}))',
            "titleFormat": "Mode Change",
        },
        {
            "datasource": DATASOURCE,
            "enable": True,
            "iconColor": "rgba(96, 165, 250, 1)",
            "name": "Parameter Changes",
            "query": rng
            + '  |> filter(fn: (r) => r._measurement == "parameter_change")\n'
            + scope
            + pivot
            + "  |> map(fn: (r) => ({time: r._time, "
            + 'text: r.parameter_name + ": " + r.old_value + " -> " + r.new_value}))',
            "titleFormat": "Parameter Change",
        },
    ]


def generate(document: dict[str, Any], runtime: str) -> dict[str, Any]:
    if not runtime:
        raise InventoryError("the runtime name must not be empty")
    devices = load_inventory(document)

    panels: list[dict[str, Any]] = []
    y = 0
    for device in devices:
        identity = (device.provider_id, device.device_id)
        panels.append(
            {
                "type": "row",
                "id": _stable_id("row", *identity),
                "title": f"{device.display_name} — {device.provider_id}/{device.device_id} ({device.type})",
                "collapsed": False,
                "gridPos": {"h": 1, "w": 24, "x": 0, "y": y},
                "panels": [],
            }
        )
        y += 1
        for index, signal in enumerate(device.signals):
            panel = _signal_panel(runtime, device, signal)
            panel["id"] = _stable_id("signal", *identity, signal.signal_id)
            panel["gridPos"] = {"h": HEIGHT, "w": WIDTH, "x": (index % 2) * WIDTH, "y": y + (index // 2) * HEIGHT}
            panels.append(panel)
        y += (len(device.signals) + 1) // 2 * HEIGHT
        quality = _quality_panel(runtime, device)
        quality["id"] = _stable_id("quality", *identity)
        quality["gridPos"] = {"h": HEIGHT, "w": 24, "x": 0, "y": y}
        panels.append(quality)
        y += HEIGHT

    ids = [p["id"] for p in panels]
    if len(set(ids)) != len(ids):
        raise InventoryError("two panels derived the same id; rename a device or signal")

    return {
        "uid": "anolis-process-" + hashlib.sha256(runtime.encode()).hexdigest()[:12],
        "title": f"Process — {runtime}",
        "description": "Generated by observability/process_dashboard.py from a saved inventory. "
        "Regenerate it rather than editing it.",
        "tags": ["anolis", "generated", "process"],
        "editable": True,
        "graphTooltip": 1,
        "id": None,
        "links": [],
        "refresh": "30s",
        "schemaVersion": 39,
        "time": {"from": "now-1h", "to": "now"},
        "timepicker": {},
        "timezone": "browser",
        "version": 1,
        "annotations": {"list": _annotations(runtime)},
        "templating": {
            "list": [
                {
                    "name": "DS_INFLUXDB",
                    "label": "Data Source",
                    "type": "datasource",
                    "query": "influxdb",
                    "current": {"selected": False, "text": "InfluxDB", "value": "InfluxDB"},
                    "hide": 0,
                    "options": [],
                    "refresh": 1,
                    "regex": "",
                }
            ]
        },
        "panels": panels,
    }


def render(dashboard: dict[str, Any]) -> str:
    return json.dumps(dashboard, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("inventory", type=Path, help="saved inventory document (JSON)")
    parser.add_argument("--runtime-name", required=True, help="the runtime's telemetry runtime_name")
    parser.add_argument("-o", "--output", type=Path, help="write here instead of stdout")
    args = parser.parse_args(argv)
    try:
        dashboard = generate(json.loads(args.inventory.read_text(encoding="utf-8")), args.runtime_name)
    except (OSError, json.JSONDecodeError, InventoryError) as exc:
        print(f"process_dashboard: {exc}", file=sys.stderr)
        return 2
    text = render(dashboard)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
