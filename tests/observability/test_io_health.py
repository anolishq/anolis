"""IO Health: device handles stay distinct across runtimes."""

from __future__ import annotations

from tests.observability.harness import Telemetry
from tests.observability.scenarios import MINUTE

UID = "anolis-io-health"
MISSING_OR_EXCLUDED = 6


def test_missing_count_tells_runtimes_apart(grafana, dashboards, load, now_ms, hour):
    """Under Runtime=All, two runtimes with the same provider and device ids are two devices."""
    t = Telemetry()
    at = now_ms - 5 * MINUTE
    for runtime in ("r1", "r2"):
        t.device_health(at, runtime=runtime, provider="bread", device="dcmt0", missing=True, excluded=False)
    t.device_health(at, runtime="r1", provider="bread", device="rlht0", missing=False, excluded=False)
    load(t)
    frames = grafana.panel(dashboards[UID], MISSING_OR_EXCLUDED, hour)
    assert [v for f in frames for c in f.value_columns() for v in c.values] == [2]
