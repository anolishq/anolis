#!/usr/bin/env python3
"""Derived-signals spike: test vectors for another implementation of the blocks.

Writes, for each small block file below and for signals.yaml, the inputs per
tick and what engine.py gives for every published signal per tick, by state
and by value. Another implementation replays the inputs and has to match.

The inputs are synthetic and deterministic, so the vectors need no recorded
data. They are engine.py's behaviour, not an independent statement of what is
right: a wrong rule in engine.py is a wrong expectation here.

  python3 vectors.py OUT_DIR

Files, tab-separated with a header line:
  <name>.inputs.tsv    tick  t  signal  state  value
  <name>.expected.tsv  tick  signal  state  value
  <name>.graph.tsv     block  type  field  key  value
<name>.graph.tsv is the block file flattened for a reader with no YAML
parser: blocks in evaluation order, one row per input (field "in"), output
("out", the value being the published signal, if any), setting ("param",
durations in seconds) and fallback ("fallback"). An input's value is
sig:<signal or block.port> or num:<number>.
state is good, not_ready or bad; an input that is missing is not_ready.
value is empty unless the state is good. Booleans are 1 and 0.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import engine
import yaml
from reference import Sample

HERE = Path(__file__).resolve().parent
DT = 2.5
PARAMS = {"liquid_target_c": 25.0, "compensation_temp_c": 25.0}

MISSING = "missing"
BAD = "bad"


def block_doc(block_type, params, inputs=("x",), outputs=("y",)):
    """A block file with one block, every output published as out/<port>."""
    return {
        "period_ms": 2500,
        "blocks": [
            {
                "id": "b",
                "type": block_type,
                "in": {name: f"in/dev/{name}" for name in inputs},
                "params": params,
                "publish": {port: {"signal": f"out/{port}"} for port in outputs},
            }
        ],
    }


def ramp_with_faults(n=60):
    """A slow ramp with a glitch, one bad sample, a run of bad samples and a gap."""
    rows = []
    for i in range(n):
        v = 20.0 + 0.1 * i
        if i == 12:
            v = 1.8
        if i == 20 or 30 <= i < 36:
            v = BAD
        if 44 <= i < 47:
            v = MISSING
        rows.append({"x": v})
    return rows


def bool_pulses(n=80):
    """True for ticks 10 to 49, with one bad sample at 25 and a gap at 40 to 41."""
    rows = []
    for i in range(n):
        v = 1.0 if 10 <= i < 50 else 0.0
        if i == 25:
            v = BAD
        if i in (40, 41):
            v = MISSING
        rows.append({"x": v})
    return rows


BLOCK_VECTORS = {
    "median": (block_doc("median", {"samples": 3}), ramp_with_faults()),
    "window_skip": (
        block_doc("window_stats", {"window": "25s", "on_bad": "skip"}, outputs=("mean", "slope_per_min", "spread")),
        ramp_with_faults(),
    ),
    "window_reset": (
        block_doc("window_stats", {"window": "25s", "on_bad": "reset"}, outputs=("mean", "slope_per_min", "spread")),
        ramp_with_faults(),
    ),
    "held_reset": (block_doc("held_for", {"seconds": 50, "on_bad": "reset"}), bool_pulses()),
    "held_hold": (block_doc("held_for", {"seconds": 50, "on_bad": "hold"}), bool_pulses()),
    "since_true": (block_doc("since_true", {"longer_than_s": 30}), bool_pulses()),
    "latch": (block_doc("latch", {}, inputs=("set",)), [{"set": r["x"]} for r in bool_pulses()]),
    "all_good": (
        block_doc("all_good", {}, inputs=("a", "b")),
        [{"a": r["x"], "b": 1.0 if i != 50 else MISSING} for i, r in enumerate(ramp_with_faults())],
    ),
    "expr_divide": (
        block_doc("expr", {"formula": "a / b"}, inputs=("a", "b")),
        [{"a": r["x"], "b": float(i - 5)} for i, r in enumerate(ramp_with_faults())],
    ),
    "deadband": (block_doc("deadband", {"step": 0.5}), ramp_with_faults()),
    "integrate": (block_doc("integrate", {"per": "1h", "on_bad": "skip"}), ramp_with_faults()),
    "kalman1": (
        block_doc("kalman1", {"tau_s": 600, "q": 1.0e-6, "r": 0.0025}, inputs=("z", "steady"), outputs=("x", "sigma")),
        [{"z": r["x"], "steady": 25.0 if i >= 5 else MISSING} for i, r in enumerate(ramp_with_faults())],
    ),
}


def cases_scenario(n=1700):
    """Inputs for signals.yaml that reach every detector and both other states."""
    t1, t2, relay = "bread0/rlht0/t1_c", "bread0/rlht0/t2_c", "bread0/rlht0/relay1_on"
    sp, room, ph = "bread0/rlht0/setpoint1_c", "adafruit0/room0/temperature", "ezo0/ph0/ph_value"
    rows = []
    for i in range(n):
        t = i * DT
        row = {
            t1: 24.0 + 2.0 * (1 - math.exp(-t / 900.0)),
            t2: 30.0 + 0.05 * math.sin(i / 7.0),
            relay: 1.0 if i % 4 == 0 and i >= 4 else 0.0,
            sp: 30.0,
            room: 20.5 if i >= 20 else MISSING,
            ph: 7.0 + 0.3 * math.exp(-t / 300.0) + 0.002 * math.sin(i / 3.0),
        }
        if i == 60:
            row[t2] = 1.8  # one glitch, quality still good
        if t >= 500.0:
            row[t2] = row[t1] - 4.0  # pad below liquid: the detach timer starts
        if i == 400:
            row[t2] = BAD  # one bad pad reading restarts it
        if 700 <= i < 760:
            row[t1] = BAD  # 150 s of bad liquid: no good reading for 120 s
        if i == 900:
            row[ph] = BAD
        if t >= 4000.0:
            row[t2] = 71.0  # pad over temperature
        if t >= 4150.0:
            row[t1] = 46.0  # liquid over temperature
        rows.append(row)
    return rows


def graph_rows(doc, eng):
    by_id = {b["id"]: b for b in doc["blocks"]}
    published = {(bid, port): sig for sig, (bid, port, _) in eng.published.items()}
    rows = ["block\ttype\tfield\tkey\tvalue"]

    def source(src):
        if src[0] == "signal":
            return f"sig:{src[1]}"
        if src[0] == "block":
            return f"sig:{src[1]}.{src[2]}"
        return f"num:{fmt(eng.params[src[1]] if src[0] == 'param' else src[1])}"

    for bid in eng.order:
        b, block = by_id[bid], eng.blocks[bid]
        for name, src in eng.wiring[bid].items():
            rows.append(f"{bid}\t{b['type']}\tin\t{name}\t{source(src)}")
        for port in block.outputs:
            rows.append(f"{bid}\t{b['type']}\tout\t{port}\t{published.get((bid, port), '')}")
        for key, value in block.p.items():
            text = value if isinstance(value, str) else fmt(value)
            rows.append(f"{bid}\t{b['type']}\tparam\t{key}\t{text}")
        if bid in eng.on_bad:
            spec = eng.on_bad[bid]
            use = "hold" if spec == "hold" else source(eng._source(f"block {bid}", spec["use"], {}, {}))
            rows.append(f"{bid}\t{b['type']}\tfallback\t\t{use}")
    return rows


def to_sample(v):
    if v == MISSING:
        return None
    if v == BAD:
        return Sample(0.0, "FAULT", 0.0)
    return Sample(v, "OK", 0.0)


def fmt(v):
    if isinstance(v, bool):
        return "1" if v else "0"
    return repr(float(v))


def write(out_dir, name, doc, rows):
    eng = engine.Engine(doc, PARAMS)
    signals = sorted(eng.device_signals)
    inputs, expected = ["tick\tt\tsignal\tstate\tvalue"], ["tick\tsignal\tstate\tvalue"]
    for i, row in enumerate(rows):
        t = i * DT
        for sig in signals:
            v = row.get(sig.split("/")[-1], row.get(sig, MISSING))
            state = "not_ready" if v == MISSING else "bad" if v == BAD else "good"
            inputs.append(f"{i}\t{t!r}\t{sig}\t{state}\t{fmt(v) if state == 'good' else ''}")
        out = eng.tick(t, {sig: to_sample(row.get(sig.split("/")[-1], row.get(sig, MISSING))) for sig in signals})
        for sig in sorted(out):
            val = out[sig]
            expected.append(f"{i}\t{sig}\t{val.state}\t{fmt(val.v) if val.state == engine.GOOD else ''}")
    (out_dir / f"{name}.inputs.tsv").write_text("\n".join(inputs) + "\n")
    (out_dir / f"{name}.expected.tsv").write_text("\n".join(expected) + "\n")
    (out_dir / f"{name}.blocks.yaml").write_text(yaml.safe_dump(doc, sort_keys=False))
    (out_dir / f"{name}.graph.tsv").write_text("\n".join(graph_rows(doc, eng)) + "\n")
    states = {line.split("\t")[2] for line in expected[1:]}
    return len(rows), len(eng.published), sorted(states)


def main():
    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, (doc, rows) in BLOCK_VECTORS.items():
        n, sigs, states = write(out_dir, name, doc, rows)
        print(f"{name:14s} {n:5d} ticks, {sigs:2d} outputs, output states seen: {', '.join(states)}")
    doc = yaml.safe_load((HERE / "signals.yaml").read_text())
    n, sigs, states = write(out_dir, "cases", doc, cases_scenario())
    print(f"{'cases':14s} {n:5d} ticks, {sigs:2d} outputs, output states seen: {', '.join(states)}")


if __name__ == "__main__":
    main()
