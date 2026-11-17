#!/usr/bin/env python3
"""Two engines that know nothing of each other: one simulates the hardware, one reads it.

    combined.py --runtime build/ci-linux-release/core/anolis-runtime \\
                --provider <anolis-provider-sim built with -DENABLE_FLUXGRAPH_V2=ON> \\
                --sim-config <anolis-provider-sim>/config/spike-v2/provider.yaml \\
                --out <an empty directory> [--seconds 90]

The simulator runs a version 2 graph of a chamber's four thermal masses in its own process, at 100 ms. The
runtime polls it every 500 ms as it would hardware, and runs combined.yaml over the readings in its own.

At 2 s the chamber's controller, which is the simulator's and not a graph, is set to hold 50 C. Then:

- the top of the chamber is set against its equation, 22 + 60 (1 - exp(-0.03 t)) with the one heater on;
- each derived signal is set against the same thing worked out here from the readings HTTP gave.
"""

import argparse
import json
import math
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
FN_SET_MODE, FN_SET_SETPOINT = 1, 2  # of tempctl in anolis-provider-sim
DERIVED = ["spread", "smooth", "duty", "settled", "lift"]

CONFIG = """runtime:
  name: derived-combined
http:
  enabled: true
  bind: 127.0.0.1
  port: {port}
providers:
  - id: sim0
    command: {provider}
    args: ["--config", "{sim_config}"]
polling:
  interval_ms: 500
telemetry:
  enabled: false
logging:
  level: info
automation:
  enabled: false
derived:
  id: derived0
  graph: {graph}
  sample_time: arrival
  inputs:
    top: {{device_handle: sim0/tempctl0, signal_id: tc1_temp}}
    bottom: {{device_handle: sim0/tempctl0, signal_id: tc2_temp}}
    heater: {{device_handle: sim0/tempctl0, signal_id: relay1_state}}
    hotend: {{device_handle: sim0/tempctl1, signal_id: tc1_temp}}
"""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(base, path, body=None, timeout=2.0):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read())


def values(state):
    """{(provider, device, signal): (value, quality)} of a /v0/state answer."""
    out = {}
    for device in state.get("devices", []):
        for v in device.get("values", []):
            held = v.get("value", {})
            out[(device.get("provider_id"), device.get("device_id"), v["signal_id"])] = (
                held.get(held.get("type", "")),
                v.get("quality"),
            )
    return out


def call(base, device, function, args):
    body = {"provider_id": "sim0", "device_id": device, "function_id": function, "args": args}
    return http(base, "/v0/call", body)["status"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--provider", required=True, type=Path)
    parser.add_argument("--sim-config", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=90.0)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    port = free_port()
    base = f"http://127.0.0.1:{port}"
    (out / "runtime.yaml").write_text(
        CONFIG.format(
            port=port,
            provider=args.provider.resolve(),
            sim_config=args.sim_config.resolve(),
            graph=HERE / "combined.yaml",
        )
    )
    log = (out / "runtime.log").open("w")
    runtime = subprocess.Popen(
        [str(args.runtime.resolve()), "--config", str(out / "runtime.yaml")], stdout=log, stderr=subprocess.STDOUT
    )
    rows = []  # (t, {name: (value, quality)}) at each look, 50 ms apart
    closed_at = None
    try:
        for _ in range(80):
            try:
                http(base, "/v0/state")
                break
            except Exception:  # noqa: BLE001 - not up yet
                if runtime.poll() is not None:
                    sys.exit(f"the runtime stopped: see {out / 'runtime.log'}")
                time.sleep(0.25)
        start = time.monotonic()
        while (t := time.monotonic() - start) < args.seconds:
            if t >= 2 and closed_at is None:
                print(
                    "  the chamber is set to hold 50 C:",
                    call(base, "tempctl0", FN_SET_SETPOINT, {"value": {"type": "double", "double": 50.0}}),
                    call(base, "tempctl0", FN_SET_MODE, {"mode": {"type": "string", "string": "closed"}}),
                )
                closed_at = time.monotonic() - start
            now = values(http(base, "/v0/state"))
            row = {
                "top": now.get(("sim0", "tempctl0", "tc1_temp")),
                "bottom": now.get(("sim0", "tempctl0", "tc2_temp")),
                "heater": now.get(("sim0", "tempctl0", "relay1_state")),
                "hotend": now.get(("sim0", "tempctl1", "tc1_temp")),
                "material": now.get(("sim0", "tempctl1", "tc2_temp")),
            }
            row.update({name: now.get(("derived0", "graph", name)) for name in DERIVED})
            rows.append((t, row))
            time.sleep(0.05)
    finally:
        runtime.terminate()
        runtime.wait(timeout=10)
        log.close()
    json.dump(rows, (out / "rows.json").open("w"))

    def series(name):
        return [(t, row[name][0]) for t, row in rows if row[name] is not None and row[name][0] is not None]

    top = series("top")
    print(
        f"over {rows[-1][0]:.0f} s: the simulator's graph at 100 ms, polled every 500 ms, the derived graph at 500 ms"
    )

    # The simulated chamber against its equation, while the one heater is on for the first time.
    reached = next(t for t, v in top if v >= 48.0)
    exact = closed_at + math.log(60 / (60 - 26)) / 0.03
    print("  the simulated chamber:")
    print(
        f"    the top reaches 48 C at {reached:.1f} s; "
        f"by its equation from the call at {closed_at:.1f} s, {exact:.1f} s"
    )
    after = [v for t, v in top if t > reached + 5]
    print(
        f"    and from 5 s after that it is between {min(after):.2f} and {max(after):.2f} C, "
        "the controller's band being 48 to 52"
    )
    for name in ("hotend", "material"):
        got = series(name)
        print(f"    {name:8s} from {got[0][1]:.2f} to {got[-1][1]:.2f} C, with no heater of its own on")

    # The derived signals against the readings they are made of.
    print("  the derived signals:")
    for name in DERIVED:
        seen = []
        for _, row in rows:
            quality = row[name][1] if row[name] else None
            if not seen or seen[-1] != quality:
                seen.append(quality)
        print(f"    {name:8s} quality, in the order seen: {seen}")

    def pairs(a, b):
        """Every a - b of readings seen at one look or at looks next to each other."""
        got = set()
        last = None
        for _, row in rows:
            here = (row[a][0], row[b][0]) if row[a] and row[b] else None
            for x in (here, last):
                for y in (here, last):
                    if x and y and x[0] is not None and y[1] is not None:
                        got.add(round(x[0] - y[1], 9))
            last = here
        return got

    for name, a, b in (("spread", "top", "bottom"), ("lift", "hotend", "top")):
        made = pairs(a, b)
        given = {round(v, 9) for _, v in series(name)}
        print(f"    {name:8s} {len(given)} values, {len(given - made)} of them not {a} - {b} of readings seen")

    worst = 0.0
    for t, v in series("duty"):
        if t < 25:
            continue
        mine = [h for u, h in series("heater") if t - 20 <= u <= t]
        worst = max(worst, abs(v - sum(1.0 if h else 0.0 for h in mine) / len(mine)))
    duty = series("duty")
    print(
        f"    duty     from 25 s, at most {worst:.3f} from the share of the looks of the last 20 s with the heater on; "
        f"at the end {duty[-1][1]:.3f}"
    )

    smooth = [v for t, v in series("smooth") if t > reached + 5]
    print(f"    smooth   from 5 s after the top reached 48 C, between {min(smooth):.2f} and {max(smooth):.2f} C")

    first_true = next((t for t, v in series("settled") if v), None)
    if first_true is None:
        print("    settled  never true")
    else:
        outside = max(t for t, v in top if t < first_true and abs(v - 50) >= 3)
        print(
            f"    settled  first true at {first_true:.1f} s, "
            f"{first_true - outside:.1f} s after the top was last seen outside 3 of 50"
        )


if __name__ == "__main__":
    main()
