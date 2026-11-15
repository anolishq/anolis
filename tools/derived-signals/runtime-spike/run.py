#!/usr/bin/env python3
"""Run the runtime-placement spike (RFC O10): a speed loop on the simulator's motor, closed by a graph in process.

    run.py --runtime build/ci-linux-release/core/anolis-runtime \\
           --provider ../anolis-provider-sim/build/ci-linux-release/anolis-provider-sim \\
           --out <an empty directory> [--poll-ms 100] [--call-ms 0] [--stall-s 0] [--seconds 20]

It writes a runtime config into --out, starts the runtime, goes to AUTO, samples /v0/state every 50 ms into
samples.csv, and prints what the loop did. --stall-s stops the runtime process for that long, --at seconds in;
--unreachable-s has the simulator fail the motor's reads for that long, --at seconds in. --graph names another
graph file with the same inputs and outputs; one that has `follow: idle` is given `idle`.
"""

import argparse
import csv
import json
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SETPOINT = 1500.0
# Functions of chaos_control in anolis-provider-sim
FN_SIGNAL_FAULT, FN_CALL_LATENCY, FN_UNREACHABLE = 2, 3, 6
COUNTS = [
    "host_calls",
    "host_ticks",
    "host_most_ticks",
    "host_gaps",
    "host_refused_calls",
    "host_refused_writes",
    "host_thrown",
]

CONFIG = """
runtime:
  data_dir: "{out}/data"
http:
  enabled: true
  bind: 127.0.0.1
  port: {port}
providers:
  - id: sim0
    command: {provider}
    args: ["--config", "{sim_config}"]
polling:
  interval_ms: {poll_ms}
telemetry:
  enabled: false
logging:
  level: info
automation:
  enabled: true
  behavior_tree: {tree}
  tick_rate_hz: 10
  manual_gating_policy: OVERRIDE
  mode_transition_hooks:
    before_transition:
      - to: FAULT
        fail_on_error: false
        calls:
          - device_handle: sim0/motorctl0
            function_name: set_motor_duty
            args: {{motor_index: 1, duty: 0.0}}
safety:
  safe_state:
    hooks:
      - device_handle: sim0/motorctl0
        function_name: set_motor_duty
        args: {{motor_index: 1, duty: 0.0}}
derived:
  id: derived0
  graph: {graph}
  call_interval_ms: {call_ms}
  sample_skew_ms: {skew_ms}
  inputs:
    setpoint: {{constant: {setpoint}}}
    speed: {{device_handle: sim0/motorctl0, signal_id: motor1_speed}}
    applied: {{device_handle: sim0/motorctl0, signal_id: motor1_duty}}
{idle}"""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(base, path, body=None, timeout=2.0):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read())


def signals(state):
    """{(provider, device, signal): (value, quality)} of a /v0/state answer."""
    out = {}
    for device in state.get("devices", []):
        for value in device.get("values", []):
            out[(device.get("provider_id"), device.get("device_id"), value.get("signal_id"))] = (
                value.get("value", {}).get(value.get("value", {}).get("type", ""), value.get("value")),
                value.get("quality"),
            )
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--provider", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--poll-ms", type=int, default=100)
    parser.add_argument("--call-ms", type=int, default=0)
    parser.add_argument("--stall-s", type=float, default=0.0)
    parser.add_argument("--unreachable-s", type=float, default=0.0)
    parser.add_argument("--signal-fault-s", type=float, default=0.0, help="the speed reads FAULT for that long")
    parser.add_argument("--call-latency-ms", type=int, default=0, help="every call to the motor takes that long")
    parser.add_argument("--at", type=float, default=10.0)
    parser.add_argument(
        "--rearm-at", type=float, default=0.0, help="go to MANUAL and back to AUTO then, as an operator would"
    )
    parser.add_argument("--graph", type=Path, default=HERE / "motor-pid.yaml")
    parser.add_argument("--max-age", default="1s", help="of the speed and of the applied duty")
    parser.add_argument("--graph-line", default="", help="one more top-level line for the graph, as max_catch_up: 3")
    parser.add_argument("--skew-ms", type=int, default=0, help="added to every sample time the graph is given")
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--dump", action="store_true", help="print one /v0/state answer and stop")
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    sim_config = HERE.parent.parent.parent / "tests" / "integration" / "fixtures" / "provider-sim-default.yaml"
    (out / "runtime.yaml").write_text(
        CONFIG.format(
            out=out,
            port=port,
            provider=args.provider.resolve(),
            sim_config=sim_config,
            poll_ms=args.poll_ms,
            call_ms=args.call_ms,
            skew_ms=args.skew_ms,
            idle="    idle: {not_acting_on: sim0/motorctl0}\n" if "follow: idle" in args.graph.read_text() else "",
            tree=HERE / "tree.xml",
            graph=out / "graph.yaml",
            setpoint=SETPOINT,
        )
    )
    (out / "graph.yaml").write_text(
        args.graph.read_text().replace("max_age: 1s", f"max_age: {args.max_age}") + args.graph_line + "\n"
    )
    log = open(out / "runtime.log", "w")
    runtime = subprocess.Popen(
        [str(args.runtime.resolve()), f"--config={out / 'runtime.yaml'}"], stdout=log, stderr=subprocess.STDOUT
    )
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                http(base, "/v0/mode")
                break
            except OSError:
                if runtime.poll() is not None or time.monotonic() > deadline:
                    sys.exit(f"the runtime did not come up; see {out / 'runtime.log'}")
                time.sleep(0.1)
        if args.dump:
            time.sleep(1.0)
            print(json.dumps(http(base, "/v0/state"), indent=1)[:6000])
            return
        http(base, "/v0/mode", {"mode": "MANUAL"})
        http(base, "/v0/mode", {"mode": "AUTO"})
        start = time.monotonic()

        rows, stalled, rearmed = [], False, False
        while (t := time.monotonic() - start) < args.seconds:
            if args.stall_s and not stalled and t >= args.at:
                runtime.send_signal(signal.SIGSTOP)
                time.sleep(args.stall_s)
                runtime.send_signal(signal.SIGCONT)
                stalled = True
                continue
            if (args.unreachable_s or args.signal_fault_s or args.call_latency_ms) and not stalled and t >= args.at:
                text = lambda v: {"type": "string", "string": v}  # noqa: E731
                whole = lambda v: {"type": "int64", "int64": int(v)}  # noqa: E731
                if args.unreachable_s:
                    fn, more = FN_UNREACHABLE, {"duration_ms": whole(args.unreachable_s * 1000)}
                elif args.signal_fault_s:
                    fn = FN_SIGNAL_FAULT
                    more = {"signal_id": text("motor1_speed"), "duration_ms": whole(args.signal_fault_s * 1000)}
                else:
                    fn, more = FN_CALL_LATENCY, {"latency_ms": whole(args.call_latency_ms)}
                call = {
                    "provider_id": "sim0",
                    "device_id": "chaos_control",
                    "function_id": fn,
                    "args": {"device_id": text("motorctl0")} | more,
                }
                try:
                    print("  injected:", http(base, "/v0/call", call)["status"])
                except urllib.error.HTTPError as e:
                    sys.exit(f"the fault could not be injected: {e.code} {e.read().decode()}")
                stalled = True
            if args.rearm_at and not rearmed and t >= args.rearm_at:
                http(base, "/v0/mode", {"mode": "MANUAL"})
                http(base, "/v0/mode", {"mode": "AUTO"})
                rearmed = True
            try:
                got = signals(http(base, "/v0/state"))
            except OSError:
                time.sleep(0.05)
                continue
            motor = {
                name: got.get(("sim0", "motorctl0", name), (None, None)) for name in ("motor1_speed", "motor1_duty")
            }
            graph = {
                name: got.get(("derived0", "graph", name), (None, None))
                for name in ["duty", "integral", "limited"] + COUNTS
            }
            rows.append(
                [round(t, 3), motor["motor1_speed"][0], motor["motor1_duty"][0], graph["duty"][0], graph["duty"][1]]
                + [graph["integral"][0], graph["limited"][0]]
                + [graph[name][0] for name in COUNTS]
            )
            time.sleep(0.05)
    finally:
        runtime.send_signal(signal.SIGINT)
        try:
            runtime.wait(timeout=10)
        except subprocess.TimeoutExpired:
            runtime.kill()
        log.close()

    header = ["t", "speed", "applied", "duty", "duty_quality", "integral", "limited"] + COUNTS
    with open(out / "samples.csv", "w", newline="") as f:
        csv.writer(f).writerows([header] + rows)

    speeds = [(r[0], r[1]) for r in rows if isinstance(r[1], (int, float))]
    within = next((t for t, v in speeds if abs(v - SETPOINT) <= 0.05 * SETPOINT), None)
    settled = None
    for t, v in reversed(speeds):
        if abs(v - SETPOINT) > 0.05 * SETPOINT:
            break
        settled = t
    disturbed = args.stall_s or args.unreachable_s or args.signal_fault_s or args.call_latency_ms
    before = [v for t, v in speeds if t < args.at] if disturbed else [v for _, v in speeds]
    tail = [v for t, v in speeds if t >= args.seconds - 3.0]
    print(
        f"{args.graph.name}: poll {args.poll_ms} ms, call {args.call_ms or 'period'} ms, stall {args.stall_s} s, "
        f"unreachable {args.unreachable_s} s, speed FAULT {args.signal_fault_s} s, "
        f"call latency {args.call_latency_ms} ms, at {args.at} s, max_age {args.max_age}, skew {args.skew_ms} ms, "
        f"{args.graph_line or 'no line more'}, "
        f"{len(rows)} samples"
    )
    print(f"  first within 5% of {SETPOINT:g} rpm at {within} s; within 5% from {settled} s to the end")
    if before:
        print(f"  highest speed before it: {max(before):.1f} rpm ({100 * (max(before) / SETPOINT - 1):+.1f}%)")
    if disturbed:
        after = [v for t, v in speeds if t >= args.at]
        print(
            f"  from {args.at} s on: lowest {min(after):.1f}, highest {max(after):.1f} rpm "
            f"({100 * (max(after) / SETPOINT - 1):+.1f}%)"
        )
    if args.rearm_at:
        after = [v for t, v in speeds if t >= args.rearm_at]
        print(
            f"  from the re-arm at {args.rearm_at} s: highest {max(after):.1f} rpm "
            f"({100 * (max(after) / SETPOINT - 1):+.1f}%)"
        )
    print(f"  mean over the last 3 s: {sum(tail) / len(tail):.1f} rpm")
    qualities = sorted({str(r[4]) for r in rows})
    print(f"  qualities of duty seen: {qualities}")
    print("  " + ", ".join(f"{name} {rows[-1][7 + i]}" for i, name in enumerate(COUNTS)))


if __name__ == "__main__":
    main()
