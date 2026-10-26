"""Fixtures for the dashboard query suite (runs in compose.test.yml's `tester`)."""

from __future__ import annotations

import time
from typing import Callable

import pytest

from tests.observability.harness import Dashboard, Grafana, Influx, Telemetry, Window

DASHBOARD_UIDS = ["anolis-device-health", "anolis-io-health", "anolis-signal-history", "anolis-system-overview"]


@pytest.fixture(scope="session")
def grafana() -> Grafana:
    client = Grafana()
    client.wait_ready()
    return client


@pytest.fixture(scope="session")
def influx() -> Influx:
    return Influx()


@pytest.fixture(scope="session")
def dashboards(grafana: Grafana) -> dict[str, Dashboard]:
    return {uid: grafana.dashboard(uid) for uid in grafana.dashboard_uids()}


@pytest.fixture()
def now_ms() -> int:
    # Whole seconds, so window arithmetic in assertions stays exact.
    return int(time.time()) * 1000


@pytest.fixture()
def hour(now_ms: int) -> Window:
    return Window.last(3600, now_ms=now_ms)


@pytest.fixture()
def load(influx: Influx) -> Callable[[Telemetry], None]:
    """Replace the bucket's contents with one fixture."""

    def _load(telemetry: Telemetry) -> None:
        influx.clear()
        influx.write(telemetry)

    influx.clear()
    return _load
