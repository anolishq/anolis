#!/usr/bin/env python3
"""Derived-signals spike: check the block engine against the reference.

Three parts:
  A. signals.yaml loads, and eleven deliberate mistakes in it are each refused.
  B. On the five synthetic guard traces the engine's `heater/trip` goes true
     at the same tick, for the same reason, as the reference detectors (and as
     heater_guard.py itself, with --guard).
  C. Replayed over the recordings, every published signal equals the
     reference at every tick (--archive, --room-csv).

Reads files; writes nothing. Exits 1 on any difference. Needs PyYAML.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import sys
from pathlib import Path

import engine
import reference
import yaml

HERE = Path(__file__).resolve().parent
PARAMS = {"liquid_target_c": 25.0, "compensation_temp_c": 25.0}
ROOM_SIGNAL = "adafruit0/room0/temperature"
TOLERANCE = 1e-9

MISTAKES = [
    ("misspelt setting", "params: { samples: 3 }", "params: { sampels: 3 }"),
    ("bad-input policy left out", "params: { seconds: 1800, on_bad: reset }", "params: { seconds: 1800 }"),
    ("unknown bad-input policy", "params: { window: 10m, on_bad: reset }", "params: { window: 10m, on_bad: clear }"),
    ("unknown block type", "type: median", "type: medain"),
    ("unknown output of a block", "in: { pad: pad.y, liquid: liquid.y }", "in: { pad: pad.out, liquid: liquid.y }"),
    (
        "dependency loop",
        "in: { t1: bread0/rlht0/t1_c, bias: { const: liquid_bias_c } }",
        "in: { t1: gap.y, bias: { const: liquid_bias_c } }",
    ),
    ("undeclared name in a formula", "formula: pad - liquid", "formula: pad - liquidd"),
    ("unknown constant", "bias: { const: liquid_bias_c }", "bias: { const: liquid_bais_c }"),
    ("unsupported syntax in a formula", "formula: t1 - bias", "formula: t1 ** bias"),
    ("setting of the wrong type", "longer_than_s: 120", "longer_than_s: two minutes"),
    ("one signal published twice", "signal: heater/pad_liquid_gap_c", "signal: heater/pad_c"),
]

DETECTORS = ["no_good_reading", "over_temp_pad", "over_temp_liquid", "pad_detached"]  # the guard's own order


def is_true(val):
    return val.state == engine.GOOD and bool(val.v)


def part_a(text):
    ok = True
    eng = engine.Engine(yaml.safe_load(text), PARAMS)
    print("PART A  loading signals.yaml")
    print(
        f"  loads: {len(eng.blocks)} blocks of {len({type(b) for b in eng.blocks.values()})} types, "
        f"{len(eng.published)} published signals, {len(eng.device_signals)} device signals read"
    )
    for label, old, new in MISTAKES:
        if text.count(old) != 1:
            print(f"  {label:34s} ANCHOR matched {text.count(old)} times")
            ok = False
            continue
        broken = text.replace(old, new)
        if hashlib.sha256(broken.encode()).digest() == hashlib.sha256(text.encode()).digest():
            print(f"  {label:34s} BROKEN: the file did not change")
            ok = False
            continue
        try:
            engine.Engine(yaml.safe_load(broken), PARAMS)
            print(f"  {label:34s} NOT REFUSED")
            ok = False
        except engine.Problem as exc:
            print(f"  {label:34s} refused: {exc}")
    print()
    return ok


def engine_first_trip(doc, rows):
    eng = engine.Engine(doc, PARAMS)

    def sample(v):
        return reference.Sample(v, "OK", 0.0) if v is not None else reference.Sample(0.0, "FAULT", 0.0)

    for t, t1, t2, relay in rows:
        out = eng.tick(t, {reference.T1: sample(t1), reference.T2: sample(t2), reference.RELAY: sample(relay)})
        if is_true(out["heater/trip"]):
            why = next(d for d in DETECTORS if is_true(out[f"heater/{d}"]))
            return t, why
    return None, None


def part_b(doc, guard_path):
    ok = True
    hold = copy.deepcopy(doc)
    next(b for b in hold["blocks"] if b["id"] == "pad_detached")["params"]["on_bad"] = "hold"
    print("PART B  heater/trip on synthetic traces, evaluated every 2.5 s")
    print(f"  {'trace':38s} {'policy':6s} {'reference':>26s} {'engine':>26s}")
    for name, rows in reference.guard_traces().items():
        for policy, d in (("reset", doc), ("hold", hold)):
            det = reference.Detectors(on_bad=policy)

            def ref_step(t, t1, t2, relay, det=det):
                def s(v):
                    return reference.Sample(v, "OK", 0.0) if v is not None else reference.Sample(0.0, "FAULT", 0.0)

                return det.step(t, s(t1), s(t2), s(relay))

            want = reference.first_trip(ref_step, rows)
            got = engine_first_trip(d, rows)
            same = want == got
            ok = ok and same

            def show(r):
                return f"{r[1]} @ {r[0]:.1f}" if r[1] else "no trip"

            print(f"  {name:38s} {policy:6s} {show(want):>26s} {show(got):>26s}{'' if same else '   DIFFERENT'}")
    if guard_path:
        reference.GUARD_PY = guard_path
        Guard = reference.load_real_guard()
        args = argparse.Namespace(pad_max=70.0, liquid_max=45.0, stale=120.0, detach_delta=3.0, detach_seconds=1800.0)
        same_all = True
        for rows in reference.guard_traces().values():
            guard = Guard(args)
            guard.last_good = 0.0

            def real(t, t1, t2, relay, g=guard):
                return g.reason({"t1_c": t1, "t2_c": t2, "relay1_on": relay}, True, t)

            t_real, _ = reference.first_trip(real, rows)
            t_eng, _ = engine_first_trip(doc, rows)
            same_all = same_all and t_real == t_eng
        print(f"  heater_guard.py trips at the same tick as the engine (reset policy) on all five: {same_all}")
        ok = ok and same_all
    ok = window_policies() and ok
    print()
    return ok


def window_policies():
    """A window with one bad sample in it: skip keeps the window, reset empties it.

    The recordings have no bad sample inside a window, so only this trace
    tells the two policies apart.
    """
    ok = True
    samples = [float(i) if i != 15 else None for i in range(40)]
    for policy in ("skip", "reset"):
        doc = {
            "period_ms": 2500,
            "blocks": [
                {
                    "id": "w",
                    "type": "window_stats",
                    "in": {"x": "p/d/s"},
                    "params": {"window": "25s", "on_bad": policy},
                    "publish": {"mean": {"signal": "g/mean"}},
                }
            ],
        }
        eng, win, same, ready = engine.Engine(doc), reference.Trailing(25), True, 0
        for i, v in enumerate(samples):
            t = i * reference.DT
            if v is not None:
                win.push(t, v)
            elif policy == "reset":
                win.clear()
            sample = reference.Sample(v, "OK", 0.0) if v is not None else reference.Sample(0.0, "FAULT", 0.0)
            got = eng.tick(t, {"p/d/s": sample})["g/mean"]
            have = got.v if got.state == engine.GOOD else None
            same = same and have == win.mean()
            ready += have is not None
        ok = ok and same
        print(
            f"  window of 10 with one bad sample, on_bad {policy:5s}: ready on {ready} of {len(samples)} ticks, "
            f"{'the same as the reference' if same else 'DIFFERENT from the reference'}"
        )
    return ok


def part_c(doc, archive, room_csv):
    ok = True
    reference.RAW, reference.ROOM_CSV = archive, room_csv
    trace = []
    reference.night(trace=trace)
    start, end = reference.ts("2026-11-09T03:00:00"), reference.ts("2026-11-09T15:00:00")
    series = reference.load(start, end, [reference.T1, reference.T2, reference.RELAY, reference.SP])
    series[ROOM_SIGNAL] = reference.load_room()

    eng = engine.Engine(doc, PARAMS)
    stats = {sig: dict(both=0, readiness=0, worst=0.0) for sig in eng.published}
    substituted = 0
    for row, (t, snap) in zip(trace, reference.grid(series, start, end), strict=True):
        assert row["t"] == t
        out = eng.tick(t, snap)
        for sig, val in out.items():
            key = sig.split("/")[-1]
            if key not in row:
                continue
            want = row[key]
            if key == "compensation_c" and want is None:
                substituted += val.state == engine.GOOD and val.v == PARAMS["compensation_temp_c"]
                continue
            have = val.v if val.state == engine.GOOD else None
            if (want is None) != (have is None):
                stats[sig]["readiness"] += 1
            elif want is not None:
                stats[sig]["both"] += 1
                stats[sig]["worst"] = max(stats[sig]["worst"], abs(float(want) - float(have)))

    print(f"PART C  replay of the 2026-11-08 overnight run: {len(trace)} ticks")
    print(f"  {'signal':34s} {'ticks compared':>14s} {'ready/not-ready differs':>24s} {'largest difference':>20s}")
    for sig, s in stats.items():
        if s["both"] == 0 and s["readiness"] == 0:
            continue
        bad = s["readiness"] > 0 or s["worst"] > TOLERANCE
        ok = ok and not bad
        print(f"  {sig:34s} {s['both']:>14d} {s['readiness']:>24d} {s['worst']:>20.3g}{'   DIFFERENT' if bad else ''}")
    print(
        f"  probes/compensation_c used its fallback parameter for the first {substituted} ticks, "
        f"while the 5-minute window filled"
    )
    not_compared = sorted(sig for sig, s in stats.items() if s["both"] == 0 and s["readiness"] == 0)
    print(f"  not compared here (no reference series): {', '.join(not_compared)}")

    want = [(t, steady) for t, steady, *_ in reference.ph_changes()]
    start, end = reference.ts(reference.PH_WINDOW[0]), reference.ts(reference.PH_WINDOW[1])
    series = reference.load(start, end, [reference.PH])
    eng, state, got = engine.Engine(doc, PARAMS), "start", []
    for t, snap in reference.grid(series, start, end):
        val = eng.tick(t, snap)["ph/steady"]
        now = bool(val.v) if val.state == engine.GOOD else None
        if now != state:
            got.append((t, now))
            state = now
    if got and got[0][1] is None and (not want or want[0][1] is not None):
        got = got[1:]  # the reference starts from "unknown" and does not list it
    same = got == want
    ok = ok and same
    print(
        f"  ph/steady over the 2026-11-02 calibration: {len(got)} changes of state, "
        f"{'the same ticks as the reference' if same else 'DIFFERENT from the reference'}"
    )
    return ok


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--guard", type=Path, help="heater_guard.py from anolis-projects bioreactor-v1")
    p.add_argument("--archive", type=Path, help="directory of 30-minute raw_event CSV exports, gzipped")
    p.add_argument("--room-csv", type=Path, help="room temperature log for the overnight run")
    args = p.parse_args()
    text = (HERE / "signals.yaml").read_text()
    doc = yaml.safe_load(text)
    ok = part_a(text)
    ok = part_b(doc, args.guard) and ok
    if args.archive and args.room_csv:
        ok = part_c(doc, args.archive, args.room_csv) and ok
    else:
        print("PART C skipped: needs --archive and --room-csv")
    print()
    print("ALL THE SAME" if ok else "DIFFERENCES FOUND")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
