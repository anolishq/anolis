#!/usr/bin/env python3
"""Derived-signals RFC, O0: what a behavior tree can already compute.

Runs four small trees in a real anolis-runtime, against the Adafruit provider
in mock mode (one simulated BME280), using only BehaviorTree.CPP's built-in
Script and ScriptCondition nodes beside the runtime's own nodes:

  a  arithmetic on a live reading, then a condition that holds
  b  the same arithmetic, a condition that does not hold
  c  values kept between ticks: fails on the fourth tick only if they were
  d  the probe-compensation pattern: quality gate, smoothing, a deadband, and
     a device call only when the held value changes

Writes only inside a temporary directory. Stdlib only.

  python3 probe.py --runtime PATH/anolis-runtime --provider PATH/anolis-provider-adafruit
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = 18080
API = f"http://127.0.0.1:{PORT}"

CONFIG = """\
runtime:
  name: tree-probe
  data_dir: ./data
  shutdown_timeout_ms: 2000
http:
  enabled: true
  bind: 127.0.0.1
  port: {port}
providers:
  - id: adafruit0
    command: {provider}
    args: ["--config", "provider.yaml"]
    timeout_ms: 5000
    hello_timeout_ms: 5000
    ready_timeout_ms: 10000
polling:
  interval_ms: 500
logging:
  level: info
automation:
  enabled: true
  behavior_tree: ./tree.xml
  tick_rate_hz: 5
  on_terminal: {on_terminal}
  parameters:
    - name: input
      type: double
      default: 1000.0
      min: 870.0
      max: 1085.0
  # The BME280 declares two settable functions, so entering AUTO needs a hook.
  mode_transition_hooks:
    before_transition:
      - to: FAULT
        fail_on_error: false
        calls:
          - device_handle: adafruit0/env0
            function_name: soft_reset
            args: {{}}
"""


def request(path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read() or b"{}")


class Run:
    """One runtime process with one tree, in AUTO."""

    def __init__(self, runtime, provider, tree, on_terminal):
        self.dir = Path(tempfile.mkdtemp(prefix="tree-probe-"))
        shutil.copy(HERE / tree, self.dir / "tree.xml")
        shutil.copy(HERE / "provider.yaml", self.dir)
        config = CONFIG.format(port=PORT, provider=provider, on_terminal=on_terminal)
        (self.dir / "runtime.yaml").write_text(config)
        self.log = self.dir / "runtime.log"
        self.proc = subprocess.Popen(
            [str(runtime), "--config", "runtime.yaml"],
            cwd=self.dir,
            stdout=self.log.open("w"),
            stderr=subprocess.STDOUT,
        )

    def __enter__(self):
        for _ in range(80):
            try:
                request("/v0/mode")
                break
            except OSError:
                time.sleep(0.25)
        request("/v0/mode", {"mode": "MANUAL"})
        self.auto = request("/v0/mode", {"mode": "AUTO"}).get("mode")
        return self

    def __exit__(self, *exc):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        shutil.rmtree(self.dir, ignore_errors=True)

    def status(self):
        s = request("/v0/automation/status")
        d = s.get("engine_diagnostics") or {}
        return s.get("execution_status"), d.get("total_ticks"), d.get("ticks_since_progress")

    def calls(self, function):
        return sum(1 for line in self.log.read_text().splitlines() if "[CallRouter] call" in line and function in line)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--runtime", type=Path, required=True, help="an anolis-runtime binary")
    p.add_argument("--provider", type=Path, required=True, help="an anolis-provider-adafruit executable")
    args = p.parse_args()
    version = subprocess.run([str(args.runtime), "--version"], capture_output=True, text=True).stdout.strip()
    print(f"{version}, anolis-provider-adafruit in mock mode, tree ticking at 5 Hz")
    print()

    for tree, expect in (("tree-a.xml", "completed"), ("tree-b.xml", "failed")):
        with Run(args.runtime, args.provider, tree, "stop") as run:
            time.sleep(1.0)
            status, ticks, _ = run.status()
        print(f"{tree}  entered {run.auto}; after 1 s: {status} on tick {ticks}  (expected {expect})")

    with Run(args.runtime, args.provider, "tree-c.xml", "repeat") as run:
        time.sleep(3.0)
        status, ticks, since = run.status()
    print(
        f"tree-c.xml  entered {run.auto}; after 3 s: {ticks} ticks, the last {since} without success  "
        f"(a counter kept between ticks makes every tick from the fourth fail)"
    )

    with Run(args.runtime, args.provider, "tree-d.xml", "repeat") as run:
        fn = "set_sea_level_pressure"
        time.sleep(2.0)
        first = run.calls(fn)
        request("/v0/parameters", {"name": "input", "value": 1000.3})
        time.sleep(2.0)
        small = run.calls(fn) - first
        request("/v0/parameters", {"name": "input", "value": 1003.0})
        time.sleep(4.0)
        large = run.calls(fn) - first - small
        status, ticks, _ = run.status()
    print(f"tree-d.xml  entered {run.auto}; {status} after {ticks} ticks")
    print(f"    device calls in the first 2 s, input 1000.0:        {first}")
    print(f"    device calls in the next 2 s, input moved by 0.3:   {small}")
    print(f"    device calls in the next 4 s, input moved by 3.0:   {large}")


if __name__ == "__main__":
    main()
