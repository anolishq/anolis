#!/usr/bin/env python3
"""Run the spike as the RFC's first version would be used: derived signals that are read and not acted on.

    monitor.py --runtime build/ci-linux-release/core/anolis-runtime \\
               --provider ../anolis-provider-sim/build/ci-linux-release/anolis-provider-sim \\
               --out <an empty directory> [--seconds 60] [--one]

Two simulators, one graph over a signal of each (monitor.yaml), telemetry on, and no tree. It asks three things
that run.py does not:

- a graph of two providers: does a signal made of both come out, and what is it while one of them fails;
- telemetry: how many rows a derived signal writes beside the signal it is made of;
- a doubted output: what HTTP and the health of its device say of a derived signal while its input is away.

Telemetry is written to a stand-in for InfluxDB in this process, which takes each write, answers 204 and counts
the rows by the signal they are of. It is not InfluxDB: nothing is stored or queried, and what is counted is what
the runtime sends.

At 20 s the first simulator's `a` reads FAULT for 1.5 s, which the low-pass rides through; at 35 s the second's
`b` does for 5 s. --one runs monitor-one.yaml, which has the signals made of `a` alone, so that nothing of its
device is FAULT while one of its signals is doubted.
"""

import argparse
import collections
import json
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
FN_SIGNAL_FAULT = 2  # of chaos_control in anolis-provider-sim
DERIVED = ["apart", "smooth", "quiet", "mean", "far"]

CONFIG = """runtime:
  name: derived-monitor
http:
  enabled: true
  bind: 127.0.0.1
  port: {port}
providers:
  - id: sim0
    command: {provider}
    args: ["--config", "{sim_config}"]
  - id: sim1
    command: {provider}
    args: ["--config", "{sim_config}"]
polling:
  interval_ms: 500
telemetry:
  enabled: true
  influxdb:
    url: http://127.0.0.1:{influx_port}
    org: stand-in
    bucket: stand-in
    token: stand-in-not-a-secret
    flush_interval_ms: 500
logging:
  level: info
automation:
  enabled: false
derived:
  id: derived0
  graph: {graph}
  sample_time: arrival
  inputs:
    a: {{device_handle: sim0/analogsensor0, signal_id: voltage_ch1}}
{b}"""

rows = collections.Counter()  # (provider, device, signal) -> rows written
qualities = collections.Counter()  # (signal of derived0, quality) -> rows written
lock = threading.Lock()


class StandIn(BaseHTTPRequestHandler):
    """Takes what the telemetry sink sends InfluxDB, and counts the rows of anolis_signal."""

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"status": "pass"}')

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode(errors="replace")
        with lock:
            for line in body.splitlines():
                head = line.split(" ", 1)[0].split(",")
                if head[0] != "anolis_signal":
                    continue
                tags = dict(tag.split("=", 1) for tag in head[1:])
                key = (tags.get("provider_id"), tags.get("device_id"), tags.get("signal_id"))
                rows[key] += 1
                if key[0] == "derived0" and 'quality="' in line:
                    qualities[(key[2], line.split('quality="', 1)[1].split('"', 1)[0])] += 1
        self.send_response(204)
        self.end_headers()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(base, path, body=None, timeout=2.0):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read())


def derived_now(state):
    """{signal: (value, quality)} of derived0/graph in a /v0/state answer."""
    for device in state.get("devices", []):
        if device.get("provider_id") == "derived0":
            return {
                v["signal_id"]: (v.get("value", {}).get(v.get("value", {}).get("type", "")), v.get("quality"))
                for v in device.get("values", [])
            }
    return {}


def health_of_graph(health):
    for provider in health.get("providers", []):
        for device in provider.get("devices", []):
            if provider.get("provider_id") == "derived0" or device.get("device_id") == "graph":
                return {k: device.get(k) for k in ("health", "stale_signal_count", "fault_signal_count")}
    return None


def fault(base, provider, seconds):
    text = lambda v: {"type": "string", "string": v}  # noqa: E731
    call = {
        "provider_id": provider,
        "device_id": "chaos_control",
        "function_id": FN_SIGNAL_FAULT,
        "args": {
            "device_id": text("analogsensor0"),
            "signal_id": text("voltage_ch1"),
            "duration_ms": {"type": "int64", "int64": int(seconds * 1000)},
        },
    }
    return http(base, "/v0/call", call)["status"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--provider", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--one", action="store_true", help="monitor-one.yaml: the signals of `a` alone")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    influx = ThreadingHTTPServer(("127.0.0.1", 0), StandIn)
    threading.Thread(target=influx.serve_forever, daemon=True).start()
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    sim_config = HERE.parent.parent.parent / "tests" / "integration" / "fixtures" / "provider-sim-default.yaml"
    (out / "runtime.yaml").write_text(
        CONFIG.format(
            port=port,
            provider=args.provider.resolve(),
            sim_config=sim_config,
            influx_port=influx.server_address[1],
            graph=HERE / ("monitor-one.yaml" if args.one else "monitor.yaml"),
            b="" if args.one else "    b: {device_handle: sim1/analogsensor0, signal_id: voltage_ch1}\n",
        )
    )
    log = (out / "runtime.log").open("w")
    runtime = subprocess.Popen(
        [str(args.runtime.resolve()), "--config", str(out / "runtime.yaml")], stdout=log, stderr=subprocess.STDOUT
    )
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
        seen = collections.defaultdict(list)  # signal -> [(t, quality)] where the quality changed
        health_seen = []  # [(t, health)] where it changed
        done = set()
        while (t := time.monotonic() - start) < args.seconds:
            if t >= 20 and "a" not in done:
                print(f"  at {t:.1f} s, sim0's a reads FAULT for 1.5 s:", fault(base, "sim0", 1.5))
                done.add("a")
            if t >= 35 and "b" not in done:
                print(f"  at {t:.1f} s, sim1's b reads FAULT for 5 s:", fault(base, "sim1", 5))
                done.add("b")
            for name, (_, quality) in derived_now(http(base, "/v0/state")).items():
                if name in DERIVED and (not seen[name] or seen[name][-1][1] != quality):
                    seen[name].append((round(t, 1), quality))
            health = health_of_graph(http(base, "/v0/providers/health"))
            if not health_seen or health_seen[-1][1] != health:
                health_seen.append((round(t, 1), health))
            time.sleep(0.1)
        elapsed = time.monotonic() - start
    finally:
        runtime.terminate()
        runtime.wait(timeout=10)
        log.close()
    time.sleep(0.2)

    print(f"over {elapsed:.0f} s, polled every 500 ms, the graph's period 500 ms")
    print("  what HTTP gave as the quality of each derived signal, where it changed:")
    for name in DERIVED:
        print(f"    {name:7s} " + ", ".join(f"{q} from {t} s" for t, q in seen[name]))
    print("  the health of derived0/graph, where it changed:")
    for t, health in health_seen:
        print(f"    from {t} s: {health}")
    print("  rows of anolis_signal the telemetry sink sent:")
    with lock:
        for key in [("sim0", "analogsensor0", "voltage_ch1"), ("sim1", "analogsensor0", "voltage_ch1")] + [
            ("derived0", "graph", name) for name in DERIVED
        ]:
            by_quality = {q: n for (s, q), n in sorted(qualities.items()) if key[0] == "derived0" and s == key[2]}
            print(f"    {'/'.join(key):36s} {rows[key]:5d}" + (f"  {by_quality}" if by_quality else ""))
        graph_rows = sum(n for (p, _, _), n in rows.items() if p == "derived0")
        print(f"    all of derived0, its counts among them: {graph_rows}; of everything: {sum(rows.values())}")
    json.dump({"/".join(k): v for k, v in rows.items()}, (out / "rows.json").open("w"), indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
