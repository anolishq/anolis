"""Named telemetry fixtures shared across the dashboard tests."""

from __future__ import annotations

from tests.observability.harness import Telemetry, UInt

MINUTE = 60_000


def machine(now_ms: int, *, runtime: str = "r1", offset: float = 0.0, telemetry: Telemetry | None = None) -> Telemetry:
    """A small mixed machine over the last half hour.

    Two providers, a device id shared between them, every typed value field, a
    quality-only row, every signal quality, health snapshots, and mode and
    parameter events. ``offset`` shifts every numeric value, so a second runtime
    written with the same ids is distinguishable in any panel that leaks it.
    """
    t = telemetry or Telemetry()
    start = now_ms - 30 * MINUTE

    for i in range(6):
        at = start + i * 5 * MINUTE
        t.signal(at, runtime=runtime, provider="bread", device="dcmt0", signal="speed", value=10.0 + i + offset)
        t.signal(at, runtime=runtime, provider="bread", device="dcmt0", signal="enabled", value=i % 2 == 0)
        t.signal(at, runtime=runtime, provider="bread", device="rlht0", signal="temp", value=37.0 + i / 10 + offset)
        t.signal(at, runtime=runtime, provider="ezo", device="dcmt0", signal="temp", value=21.0 + offset)
        t.signal(at, runtime=runtime, provider="ezo", device="do0", signal="count", value=100 + i + int(offset))
        t.signal(at, runtime=runtime, provider="ezo", device="do0", signal="ticks", value=UInt(500 + i + int(offset)))
        t.signal(at, runtime=runtime, provider="ezo", device="do0", signal="state", value="RUNNING")
        for device, provider, staleness in (("dcmt0", "bread", 200), ("rlht0", "bread", 300), ("dcmt0", "ezo", 900)):
            t.device_health(
                at,
                runtime=runtime,
                provider=provider,
                device=device,
                staleness_ms=staleness + int(offset),
                stale_signal_count=0,
                fault_signal_count=0,
                io_ok=1000 * i,
                io_failed=i,
                io_retried_attempts=2 * i,
                watchdog_trip_count=0,
                watchdog_armed=True,
                watchdog_tripped=False,
                missing=False,
                excluded=False,
            )
        t.device_health(at, runtime=runtime, provider="ezo", device="do0", staleness_ms=400, io_failed=0)
        t.provider_health(at, runtime=runtime, provider="bread", failed_device_count=0)
        t.provider_health(at, runtime=runtime, provider="ezo", failed_device_count=0)

    t.signal(now_ms - 3 * MINUTE, runtime=runtime, provider="ezo", device="do0", signal="saturation", quality="FAULT")
    t.signal(
        now_ms - 2 * MINUTE, runtime=runtime, provider="ezo", device="do0", signal="ph", quality="STALE", value=7.0
    )
    t.signal(now_ms - MINUTE, runtime=runtime, provider="bread", device="rlht0", signal="duty", quality="UNAVAILABLE")
    t.mode_change(now_ms - 20 * MINUTE, "MANUAL", "AUTO", runtime=runtime)
    t.parameter_change(now_ms - 10 * MINUTE, "setpoint", "37.0", "37.5", runtime=runtime)
    return t
