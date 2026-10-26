"""Execute the bundled Grafana dashboards' queries against known telemetry.

Queries go through Grafana's ``/api/ds/query``, so the datasource, its default
bucket and the ``v.*`` macros (``v.timeRangeStart``, ``v.windowPeriod``,
``v.defaultBucket``) are Grafana's own. Dashboard template variables are not:
the browser substitutes them before a query is sent, so ``interpolate`` does
that here, the way Grafana does for the variable types these dashboards use.
"""

from __future__ import annotations

import os
import re
import time
from dataclasses import dataclass, field
from typing import Any, Iterable

import requests

ALL = "$__all"

# Characters Grafana escapes in a value substituted with the :regex format.
_REGEX_SPECIAL = re.compile(r"[\\^$*+?.()|{}\[\]/-]")
_VARIABLE = re.compile(r"\$\{(\w+)(?::(\w+))?\}|\$(\w+)")


def _env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} is not set; run the suite through tests/observability/compose.test.yml")
    return value


@dataclass
class Column:
    name: str
    labels: dict[str, str]
    values: list[Any]


@dataclass
class Frame:
    name: str
    columns: list[Column]

    def column(self, name: str) -> Column:
        for column in self.columns:
            if column.name == name:
                return column
        raise KeyError(f"frame {self.name!r} has no column {name!r}; columns: {[c.name for c in self.columns]}")

    def rows(self) -> list[dict[str, Any]]:
        if not self.columns:
            return []
        return [{c.name: c.values[i] for c in self.columns} for i in range(len(self.columns[0].values))]

    def value_columns(self) -> list[Column]:
        """Every non-time column: one series per column in a time-series frame."""
        return [c for c in self.columns if c.name not in ("Time", "_time", "_start", "_stop")]


def rows(frames: Iterable[Frame]) -> list[dict[str, Any]]:
    return [row for frame in frames for row in frame.rows()]


class QueryError(AssertionError):
    pass


@dataclass
class Window:
    """A dashboard time range, in epoch milliseconds."""

    start_ms: int
    stop_ms: int
    interval_ms: int = 10_000

    @classmethod
    def last(cls, seconds: int, *, now_ms: int, interval_ms: int = 10_000) -> "Window":
        return cls(now_ms - seconds * 1000, now_ms, interval_ms)


@dataclass
class Dashboard:
    uid: str
    model: dict[str, Any]

    @property
    def variables(self) -> dict[str, dict[str, Any]]:
        return {v["name"]: v for v in self.model.get("templating", {}).get("list", [])}

    def panel(self, panel_id: int) -> dict[str, Any]:
        for panel in self.panels():
            if panel["id"] == panel_id:
                return panel
        raise KeyError(f"{self.uid} has no panel id {panel_id}")

    def panels(self) -> list[dict[str, Any]]:
        return [p for p in self.model.get("panels", []) if p.get("type") != "row"]

    def annotation(self, name: str) -> dict[str, Any]:
        for annotation in self.annotations():
            if annotation["name"] == name:
                return annotation
        raise KeyError(f"{self.uid} has no annotation {name!r}")

    def annotations(self) -> list[dict[str, Any]]:
        return [a for a in self.model.get("annotations", {}).get("list", []) if not a.get("builtIn")]


def _query_text(target: dict[str, Any]) -> str:
    query = target.get("query")
    if isinstance(query, dict):
        query = query.get("query")
    if not isinstance(query, str):
        raise KeyError(f"target has no Flux query: {target}")
    return query


def _regex_escape(value: str) -> str:
    return _REGEX_SPECIAL.sub(lambda m: "\\" + m.group(0), value)


def interpolate(query: str, values: dict[str, list[str]]) -> str:
    """Substitute ``${name}``, ``${name:regex}`` and ``$name`` like Grafana.

    ``values`` maps a variable to its resolved values (``ALL`` already expanded
    to every option). Multi-value ``:regex`` becomes ``(a|b)``; a single value is
    escaped on its own. An unknown variable is left as written, as Grafana does.
    """

    def substitute(match: re.Match[str]) -> str:
        name = match.group(1) or match.group(3)
        fmt = match.group(2)
        if name not in values:
            return match.group(0)
        resolved = values[name]
        if fmt == "regex":
            escaped = [_regex_escape(v) for v in resolved]
            return escaped[0] if len(escaped) == 1 else "(" + "|".join(escaped) + ")"
        return ",".join(resolved)

    return _VARIABLE.sub(substitute, query)


class Grafana:
    def __init__(self) -> None:
        self.url = _env("GRAFANA_URL").rstrip("/")
        self.session = requests.Session()
        self.session.auth = (_env("GRAFANA_USER"), _env("GRAFANA_PASSWORD"))
        self._datasource_uid: str | None = None

    def wait_ready(self, timeout_s: float = 120.0) -> None:
        deadline = time.monotonic() + timeout_s
        last: object = None
        while time.monotonic() < deadline:
            try:
                health = self.session.get(f"{self.url}/api/health", timeout=5)
                if health.ok and health.json().get("database") == "ok" and self.datasource_uid():
                    return
                last = health.text
            except requests.RequestException as exc:
                last = exc
            time.sleep(1)
        raise RuntimeError(f"Grafana not ready after {timeout_s}s: {last}")

    def datasource_uid(self) -> str | None:
        if self._datasource_uid is None:
            response = self.session.get(f"{self.url}/api/datasources", timeout=10)
            response.raise_for_status()
            sources = [d for d in response.json() if d["type"] == "influxdb"]
            if len(sources) == 1:
                self._datasource_uid = sources[0]["uid"]
        return self._datasource_uid

    def dashboard_uids(self) -> list[str]:
        response = self.session.get(f"{self.url}/api/search", params={"type": "dash-db"}, timeout=10)
        response.raise_for_status()
        return sorted(d["uid"] for d in response.json())

    def dashboard(self, uid: str) -> Dashboard:
        response = self.session.get(f"{self.url}/api/dashboards/uid/{uid}", timeout=10)
        response.raise_for_status()
        return Dashboard(uid, response.json()["dashboard"])

    def query(self, flux: str, window: Window) -> list[Frame]:
        body = {
            "from": str(window.start_ms),
            "to": str(window.stop_ms),
            "queries": [
                {
                    "refId": "A",
                    "datasource": {"type": "influxdb", "uid": self.datasource_uid()},
                    "query": flux,
                    "intervalMs": window.interval_ms,
                    "maxDataPoints": 1000,
                }
            ],
        }
        response = self.session.post(f"{self.url}/api/ds/query", json=body, timeout=30)
        result = response.json().get("results", {}).get("A", {}) if response.content else {}
        if not response.ok or result.get("error"):
            raise QueryError(f"query failed ({response.status_code}): {result.get('error') or response.text}\n{flux}")
        return [_frame(f) for f in result.get("frames", [])]

    # -- dashboards -----------------------------------------------------------

    def resolve(
        self, dashboard: Dashboard, window: Window, selected: dict[str, list[str]] | None
    ) -> dict[str, list[str]]:
        """Resolve every query variable: a selection if given, else its default (All)."""
        selected = selected or {}
        resolved: dict[str, list[str]] = {}
        for name, variable in dashboard.variables.items():
            if variable.get("type") != "query":
                continue
            choice = selected.get(name, [ALL])
            if choice == [ALL]:
                options = rows(self.query(interpolate(_query_text(variable), resolved), window))
                choice = sorted({str(row["_value"]) for row in options if row.get("_value") is not None})
            resolved[name] = choice
        return resolved

    def panel(
        self,
        dashboard: Dashboard,
        panel_id: int,
        window: Window,
        selected: dict[str, list[str]] | None = None,
    ) -> list[Frame]:
        values = self.resolve(dashboard, window, selected)
        frames: list[Frame] = []
        for target in dashboard.panel(panel_id).get("targets", []):
            frames.extend(self.query(interpolate(_query_text(target), values), window))
        return frames

    def annotation(
        self,
        dashboard: Dashboard,
        name: str,
        window: Window,
        selected: dict[str, list[str]] | None = None,
    ) -> list[Frame]:
        values = self.resolve(dashboard, window, selected)
        annotation = dashboard.annotation(name)
        # Annotations keep their Flux at the top level; newer models nest it in target.
        source = annotation if "query" in annotation else annotation.get("target", {})
        return self.query(interpolate(_query_text(source), values), window)


def _frame(raw: dict[str, Any]) -> Frame:
    schema = raw.get("schema", {})
    values = raw.get("data", {}).get("values", [])
    columns = []
    for index, spec in enumerate(schema.get("fields", [])):
        columns.append(Column(spec["name"], spec.get("labels") or {}, values[index] if index < len(values) else []))
    return Frame(schema.get("name", ""), columns)


# -- telemetry fixtures ---------------------------------------------------------


class UInt(int):
    """An unsigned field value, written with line protocol's ``u`` suffix."""


def _tag(value: str) -> str:
    return re.sub(r"([,= \\])", r"\\\1", value)


def _field(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, UInt):
        return f"{int(value)}u"
    if isinstance(value, int):
        return f"{value}i"
    if isinstance(value, float):
        return repr(value)
    escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


@dataclass
class Telemetry:
    """Line-protocol rows in the shapes core/telemetry writes them."""

    lines: list[str] = field(default_factory=list)

    def _row(self, measurement: str, tags: dict[str, str], fields: dict[str, Any], t_ms: int) -> "Telemetry":
        tag_text = "".join(f",{k}={_tag(v)}" for k, v in tags.items())
        field_text = ",".join(f"{k}={_field(v)}" for k, v in fields.items())
        self.lines.append(f"{measurement}{tag_text} {field_text} {t_ms}")
        return self

    def signal(
        self,
        t_ms: int,
        *,
        runtime: str = "r1",
        provider: str = "p1",
        device: str = "d1",
        signal: str = "s1",
        quality: str = "OK",
        value: Any = None,
    ) -> "Telemetry":
        """One anolis_signal row; ``value`` None writes quality only (a non-finite double)."""
        fields: dict[str, Any] = {}
        if value is not None:
            kind = {bool: "bool", UInt: "uint", int: "int", float: "double", str: "string"}[type(value)]
            fields[f"value_{kind}"] = value
        fields["quality"] = quality
        tags = {"runtime_name": runtime, "provider_id": provider, "device_id": device, "signal_id": signal}
        return self._row("anolis_signal", tags, fields, t_ms)

    def device_health(
        self,
        t_ms: int,
        *,
        runtime: str = "r1",
        provider: str = "p1",
        device: str = "d1",
        health: str = "OK",
        **fields: Any,
    ) -> "Telemetry":
        tags = {"runtime_name": runtime, "provider_id": provider, "device_id": device}
        return self._row("anolis_device_health", tags, {"health": health, "registered": True, **fields}, t_ms)

    def provider_health(self, t_ms: int, *, runtime: str = "r1", provider: str = "p1", **fields: Any) -> "Telemetry":
        base: dict[str, Any] = {
            "state": "AVAILABLE",
            "lifecycle_state": "RUNNING",
            "degraded": False,
            "failed_device_count": 0,
            "device_count": 1,
            "uptime_seconds": 60,
            "supervision_enabled": True,
            "attempt_count": 0,
            "crash_detected": False,
            "circuit_open": False,
        }
        tags = {"runtime_name": runtime, "provider_id": provider}
        return self._row("anolis_provider_health", tags, {**base, **fields}, t_ms)

    def mode_change(self, t_ms: int, previous: str, new: str, *, runtime: str | None = "r1") -> "Telemetry":
        tags = {"runtime_name": runtime} if runtime else {}
        return self._row("mode_change", tags, {"previous_mode": previous, "new_mode": new}, t_ms)

    def parameter_change(self, t_ms: int, name: str, old: str, new: str, *, runtime: str | None = "r1") -> "Telemetry":
        tags = {"runtime_name": runtime} if runtime else {}
        tags["parameter_name"] = name
        return self._row("parameter_change", tags, {"old_value": old, "new_value": new}, t_ms)


class Influx:
    def __init__(self) -> None:
        self.url = _env("INFLUX_URL").rstrip("/")
        self.org = _env("INFLUX_ORG")
        self.bucket = _env("INFLUX_BUCKET")
        self.session = requests.Session()
        self.session.headers["Authorization"] = f"Token {_env('INFLUX_TOKEN')}"

    def write(self, telemetry: Telemetry) -> None:
        if not telemetry.lines:
            return
        response = self.session.post(
            f"{self.url}/api/v2/write",
            params={"org": self.org, "bucket": self.bucket, "precision": "ms"},
            data="\n".join(telemetry.lines).encode(),
            timeout=30,
        )
        if not response.ok:
            raise RuntimeError(f"write failed ({response.status_code}): {response.text}")

    def clear(self) -> None:
        response = self.session.post(
            f"{self.url}/api/v2/delete",
            params={"org": self.org, "bucket": self.bucket},
            json={"start": "1970-01-01T00:00:00Z", "stop": "2200-01-01T00:00:00Z"},
            timeout=30,
        )
        if not response.ok:
            raise RuntimeError(f"delete failed ({response.status_code}): {response.text}")
