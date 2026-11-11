#!/usr/bin/env python3
"""Derived-signals paper exercise: the reference behaviour, as plain code.

This is the control for the exercise ("style D"): each test case written the
way it would be written with no declarative layer at all. Its output is the
behaviour the three strawman declaration styles have to reproduce.

Two parts:
  1. Guard detectors on synthetic traces, compared tick for tick with the real
     Guard class in anolis-projects' heater_guard.py (--guard).
  2. Every case replayed over recorded rig telemetry: the 2026-11-08 overnight
     heater run and the 2026-11-02 pH calibration (--archive, --room-csv).
     The recordings are not published; README.md describes their format.

Stdlib only. Reads files; writes nothing. See README.md for how to run it.
"""

from __future__ import annotations

import argparse
import collections
import csv
import datetime as dt
import gzip
import importlib.util
import statistics
from pathlib import Path

# Set from the command line in main().
RAW = Path()
GUARD_PY = Path()
ROOM_CSV = Path()

DT = 2.5  # evaluation period, s: the rig's polling.interval_ms
UTC = dt.timezone.utc
EST = dt.timezone(dt.timedelta(hours=-5))

Sample = collections.namedtuple("Sample", "v q age")  # value, quality, s since the row was written


def ok(s):
    return s is not None and s.q == "OK"


# ---------------------------------------------------------------- building blocks


class Median:
    """Median of the last n good samples; no output until n have been seen."""

    def __init__(self, n):
        self.buf = collections.deque(maxlen=n)

    def step(self, s):
        if ok(s):
            self.buf.append(s.v)
        return statistics.median(self.buf) if len(self.buf) == self.buf.maxlen else None


class Trailing:
    """Good samples from the last `seconds`; no output until the window is full."""

    def __init__(self, seconds):
        self.n = round(seconds / DT)
        self.buf = collections.deque(maxlen=self.n)

    def push(self, t, v):
        self.buf.append((t, v))

    def clear(self):
        self.buf.clear()

    def full(self):
        return len(self.buf) == self.n

    def mean(self):
        return sum(v for _, v in self.buf) / len(self.buf) if self.full() else None

    def spread(self):
        vs = [v for _, v in self.buf]
        return max(vs) - min(vs) if self.full() else None

    def slope_per_min(self):
        if not self.full():
            return None
        t0 = self.buf[0][0]
        xs = [(t - t0) / 60.0 for t, _ in self.buf]
        ys = [v for _, v in self.buf]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        den = sum((x - mx) ** 2 for x in xs)
        return sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True)) / den


class Detectors:
    """Case 4. The heater guard's stop decisions as per-tick detectors.

    on_bad is the policy for the detach timer when t1 or t2 is not good:
    'reset' (what heater_guard.py does) or 'hold'.
    """

    def __init__(self, pad_max=70.0, liquid_max=45.0, stale=120.0, delta=3.0, detach=1800.0, on_bad="reset"):
        self.p = (pad_max, liquid_max, stale, delta, detach)
        self.on_bad = on_bad
        self.last_good = 0.0
        self.relay_seen = False
        self.since = None
        self.timer_starts = 0

    def step(self, t, t1, t2, relay):
        pad_max, liquid_max, stale, delta, detach = self.p
        good = ok(t1) and ok(t2)
        if good:
            self.last_good = t
        elif t - self.last_good > stale:
            return "no_good_reading"
        if ok(t2) and t2.v > pad_max:
            return "over_temp_pad"
        if ok(t1) and t1.v > liquid_max:
            return "over_temp_liquid"
        self.relay_seen = self.relay_seen or (ok(relay) and bool(relay.v))
        if self.relay_seen and good and t2.v < t1.v - delta:
            if self.since is None:
                self.since = t
                self.timer_starts += 1
            if t - self.since > detach:
                return "pad_detached"
        elif good or self.on_bad == "reset":
            self.since = None
        return None


class Kalman1:
    """Case 8. One-state filter for the liquid temperature.

    Model: the liquid relaxes toward the steady value from case 3 with time
    constant tau. Measurement: the corrected liquid thermocouple, when good.
    tau, q and r are illustrative, not fitted.
    """

    def __init__(self, tau_s=3.5 * 3600, q=1e-6, r=0.05**2, p0=4.0):
        self.tau, self.q, self.r, self.p0 = tau_s, q, r, p0
        self.x = None
        self.p = p0

    def step(self, steady, z):
        if self.x is None:
            if z is None:
                return None, None
            self.x, self.p = z, self.r
            return self.x, self.p**0.5
        if steady is not None:
            a = 1.0 - DT / self.tau
            self.x = a * self.x + (1 - a) * steady
            self.p = a * a * self.p + self.q * DT
        else:
            self.p += self.q * DT
        if z is not None:
            k = self.p / (self.p + self.r)
            self.x += k * (z - self.x)
            self.p *= 1 - k
        return self.x, self.p**0.5


def predicted_liquid(room, pad, k):
    """Case 3. Steady liquid temperature for a held pad temperature."""
    return room + (pad - room) / (1 + k)


def suggested_pad(target, room, k):
    """Case 3. Pad setpoint for a liquid target (runbook, 'Choosing the pad setpoint')."""
    return target + k * (target - room)


# ---------------------------------------------------------------- part 1: the guard


def load_real_guard():
    spec = importlib.util.spec_from_file_location("heater_guard", GUARD_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Guard


def guard_traces():
    """name -> list of (t, t1, t2, relay); a value of None is a reading that is not OK."""

    def base(n, t1=25.0, t2=30.0):
        return [[i * DT, t1, t2, i * DT >= 10.0] for i in range(n)]

    over = base(80)
    for r in over:
        if r[0] >= 100.0:
            r[2] = 71.0
    stale = base(120)
    for r in stale:
        if r[0] >= 50.0:
            r[1] = None
    detach = base(800)
    for r in detach:
        if r[0] >= 100.0:
            r[2] = 21.0
    broken = [list(r) for r in detach]
    for r in broken:
        if r[0] == 1000.0:
            r[2] = None
    glitch = base(800)
    for r in glitch:
        if r[0] == 500.0:
            r[2] = 1.8
    return {
        "pad over 70 from t=100": over,
        "liquid reading bad from t=50": stale,
        "pad 4 C below liquid from t=100": detach,
        "same, one bad pad reading at t=1000": broken,
        "one 1.8 C pad sample at t=500": glitch,
    }


def first_trip(step, rows):
    for t, t1, t2, relay in rows:
        why = step(t, t1, t2, relay)
        if why:
            return t, why
    return None, None


def part1():
    Guard = load_real_guard()
    args = argparse.Namespace(pad_max=70.0, liquid_max=45.0, stale=120.0, detach_delta=3.0, detach_seconds=1800.0)
    print("PART 1  guard detectors on synthetic traces, evaluated every 2.5 s")
    print(f"{'trace':40s} {'heater_guard.py':>24s} {'detectors, reset':>24s} {'detectors, hold':>24s}")
    for name, rows in guard_traces().items():
        guard = Guard(args)
        guard.last_good = 0.0

        def real(t, t1, t2, relay, g=guard):
            why = g.reason({"t1_c": t1, "t2_c": t2, "relay1_on": relay}, True, t)
            if not why:
                return None
            if why.startswith("no good"):
                return "no_good_reading"
            if why.startswith("pad t2_c") and "above" in why:
                return "over_temp_pad"
            if why.startswith("liquid"):
                return "over_temp_liquid"
            return "pad_detached"

        def mine(policy):
            d = Detectors(on_bad=policy)

            def run(now, liquid, pad, relay):
                def s(v):
                    return Sample(v, "OK", 0.0) if v is not None else Sample(0.0, "FAULT", 0.0)

                return d.step(now, s(liquid), s(pad), s(relay))

            return run

        cells = []
        for step in (real, mine("reset"), mine("hold")):
            t, why = first_trip(step, rows)
            cells.append(f"{why} @ {t:.1f}" if why else "no trip")
        print(f"{name:40s} {cells[0]:>24s} {cells[1]:>24s} {cells[2]:>24s}")
    print()


# ---------------------------------------------------------------- part 2: recorded data


def parse_ts(s):
    return dt.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()


def load(start, end, wanted, carry_h=72):
    """Rows for `wanted` signals in [start - carry, end], oldest first, per signal."""
    out = {w: [] for w in wanted}
    t = dt.datetime.fromtimestamp(start - carry_h * 3600, UTC).replace(minute=0, second=0, microsecond=0)
    last = dt.datetime.fromtimestamp(end, UTC)
    while t <= last:
        f = RAW / (t.strftime("%Y%m%dT%H%M%SZ") + ".csv.gz")
        if f.exists():
            with gzip.open(f, "rt", newline="") as fh:
                for r in csv.DictReader(fh):
                    key = f"{r['provider_id']}/{r['device_id']}/{r['signal_id']}"
                    if key in out:
                        v = r["value"]
                        v = (v.lower() == "true") if r["value_type"] == "bool" else float(v)
                        out[key].append((parse_ts(r["timestamp"]), v, r["quality"]))
        t += dt.timedelta(minutes=30)
    for rows in out.values():
        rows.sort()
    return out


def load_room():
    rows = []
    with open(ROOM_CSV, newline="") as fh:
        for r in csv.DictReader(fh):
            if r["temp_c"]:
                local = dt.datetime.fromisoformat(r["time"]).replace(tzinfo=EST)
                rows.append((local.timestamp(), float(r["temp_c"]), "OK"))
    return rows


def grid(series, start, end):
    """Sample-and-hold every series onto the evaluation grid."""
    idx = {k: 0 for k in series}
    t = start
    while t <= end:
        snap = {}
        for k, rows in series.items():
            i = idx[k]
            while i < len(rows) and rows[i][0] <= t:
                i += 1
            idx[k] = i
            snap[k] = Sample(rows[i - 1][1], rows[i - 1][2], t - rows[i - 1][0]) if i else None
        yield t, snap
        t += DT


def ts(s):
    return dt.datetime.fromisoformat(s).replace(tzinfo=UTC).timestamp()


def hhmm(t, tz=UTC):
    return dt.datetime.fromtimestamp(t, tz).strftime("%H:%M:%S")


T1, T2 = "bread0/rlht0/t1_c", "bread0/rlht0/t2_c"
RELAY, SP = "bread0/rlht0/relay1_on", "bread0/rlht0/setpoint1_c"
PH = "ezo0/ph0/ph_value"
ROOM = "room"
# Hourly duty from the rig's own record of that night (EST hours), worked out
# at the time from the same telemetry by a separate script.
BENCH_DUTY = {22: 27.5, 0: 23.2, 2: 18.9, 4: 18.1, 6: 17.6, 8: 16.9, 9: 16.9}


def night(mask=None):
    """One pass over the 2026-11-08 overnight run. mask=(a, b) hides t1 between a and b."""
    start, end = ts("2026-11-09T03:00:00"), ts("2026-11-09T15:00:00")
    series = load(start, end, [T1, T2, RELAY, SP])
    series[ROOM] = load_room()
    K, BIAS, VOLTS, AMPS = 1.1, 1.2, 12.0, 2.95

    med = Median(3)
    duty10, comp5 = Trailing(600), Trailing(300)
    kf = Kalman1()
    det_raw, det_clean = Detectors(), Detectors()
    det_raw.last_good = det_clean.last_good = start
    hours = collections.defaultdict(lambda: {"on": 0, "n": 0, "d10": []})
    res = {
        "glitch": [],
        "outliers": 0,
        "trips_raw": [],
        "trips_clean": [],
        "comp": [],
        "comp_db": [],
        "comp_raw_changes": 0,
    }
    energy_wh, last_comp, last_mean, last_db = 0.0, None, None, None
    age_max = collections.defaultdict(float)
    sp_carry = None

    for t, s in grid(series, start, end):
        t1, t2, relay, sp, room = s[T1], s[T2], s[RELAY], s[SP], s[ROOM]
        for k in (T1, T2, RELAY, SP):
            if s[k] is not None:
                age_max[k] = max(age_max[k], s[k].age)
        if sp_carry is None and sp is not None:
            sp_carry = sp.age

        # case 1: glitch rejection
        t2c = med.step(t2)
        if ok(t2) and t2c is not None and abs(t2.v - t2c) > 5.0:
            res["outliers"] += 1
            res["glitch"].append((t, t2.v, t2c))

        # case 2: corrected liquid temperature and the pad-liquid gap
        t1_seen = t1 if not (mask and mask[0] <= t < mask[1]) else None
        t1c = t1_seen.v - BIAS if ok(t1_seen) else None
        gap = t2c - t1c if (t2c is not None and t1c is not None) else None

        # case 3: steady liquid temperature for the pad setpoint in force
        pad = sp.v if ok(sp) else None
        pred = predicted_liquid(room.v, pad, K) if (ok(room) and pad is not None) else None

        # case 4: detectors, on the raw and on the cleaned pad reading
        t2_clean_s = Sample(t2c, "OK", 0.0) if t2c is not None else None
        for det, pad_s, key in ((det_raw, t2, "trips_raw"), (det_clean, t2_clean_s, "trips_clean")):
            why = det.step(t, t1, pad_s, relay)
            if why:
                res[key].append((t, why))

        # case 6: relay duty over 10 min, and heater power
        if ok(relay):
            duty10.push(t, 1.0 if relay.v else 0.0)
            energy_wh += (VOLTS * AMPS * DT / 3600.0) if relay.v else 0.0
        d10 = duty10.mean()
        h = dt.datetime.fromtimestamp(t, EST).hour
        if ok(relay):
            hours[h]["on"] += 1 if relay.v else 0
            hours[h]["n"] += 1
        if d10 is not None:
            hours[h]["d10"].append(d10)

        # case 7: compensation temperature
        if t1c is not None:
            comp5.push(t, t1c)
        m = comp5.mean()
        if m is not None:
            if last_mean is not None and round(m, 2) != round(last_mean, 2):
                res["comp_raw_changes"] += 1
            last_mean = m
            q = round(m * 2) / 2
            if q != last_comp:
                res["comp"].append((t, q))
                last_comp = q
            if last_db is None or abs(m - last_db) >= 0.5:
                last_db = m
                res["comp_db"].append((t, m))

        # case 8: one-state filter
        x, sig = kf.step(pred, t1c)
        res["last"] = dict(t=t, t1c=t1c, pred=pred, gap=gap, x=x, sig=sig, d10=d10)
        if mask and abs(t - mask[1]) < DT / 2:
            res["mask_end"] = dict(x=x, sig=sig, actual=(t1.v - BIAS) if ok(t1) else None)
        if mask and abs(t - (mask[0] - DT)) < DT / 2:
            res["mask_start"] = dict(x=x, sig=sig)

    res.update(
        hours=hours,
        energy_wh=energy_wh,
        span_h=(end - start) / 3600.0,
        age_max=age_max,
        sp_carry=sp_carry,
        starts_raw=det_raw.timer_starts,
        starts_clean=det_clean.timer_starts,
        rows={k: len(v) for k, v in series.items()},
    )
    return res


def part2():
    r = night()
    print("PART 2  replay over recorded telemetry, 2026-11-09T03:00Z to 15:00Z (22:00 to 10:00 EST)")
    print("rows loaded (with up to 72 h carry-in):", ", ".join(f"{k.split('/')[-1]} {n}" for k, n in r["rows"].items()))
    print(f"setpoint1_c: last change row is {r['sp_carry'] / 3600:.1f} h before the window opens")
    print(
        "longest time without a row inside the window: "
        + ", ".join(f"{k.split('/')[-1]} {r['age_max'][k]:.0f} s" for k in (T1, T2, RELAY))
    )
    print()

    print("case 1  glitch rejection: ticks where raw pad and median-of-3 differ by more than 5 C:", r["outliers"])
    for t, raw, clean in r["glitch"][:4]:
        print(f"    {hhmm(t)}Z  raw {raw:.2f}  median3 {clean:.2f}")
    print()

    last = r["last"]
    print(
        f"cases 2, 3  at {hhmm(last['t'])}Z: corrected liquid {last['t1c']:.2f} C, pad-liquid gap {last['gap']:.2f} C, "
        f"predicted steady liquid {last['pred']:.2f} C (k 1.1)"
    )
    print(
        "    suggested pad for a 25 / 30 / 37 C liquid in a 20.5 C room: "
        + " / ".join(f"{suggested_pad(tg, 20.5, 1.1):.2f}" for tg in (25.0, 30.0, 37.0))
    )
    print()

    print(
        f"case 4  detectors over the night: trips on raw pad {len(r['trips_raw'])}, "
        f"on cleaned pad {len(r['trips_clean'])}; "
        f"detach timer started {r['starts_raw']} time(s) on raw, {r['starts_clean']} on cleaned"
    )
    print()

    print("case 6  relay duty from 2.5 s sample-and-hold of change rows")
    print("    hour(EST)  duty   rig record    10-min window min..max")
    for h in (22, 0, 2, 4, 6, 8, 9):
        e = r["hours"][h]
        d10 = e["d10"]
        print(
            f"    {h:02d}        {100 * e['on'] / e['n']:5.1f}%   {BENCH_DUTY[h]:5.1f}%        "
            f"{100 * min(d10):4.1f}..{100 * max(d10):4.1f}%"
        )
    print(
        f"    energy over the {r['span_h']:.0f} h window: {r['energy_wh']:.1f} Wh, "
        f"mean {r['energy_wh'] / r['span_h']:.2f} W "
        f"(12 V x 2.95 A while on; not metered)"
    )
    print()

    print(
        f"case 7  compensation temperature: 5-min mean of corrected liquid, rounded to 0.5 C: "
        f"{len(r['comp'])} value(s) sent over the night; "
        f"the unrounded mean changed (at 0.01 C) {r['comp_raw_changes']} times"
    )
    for t, q in r["comp"][:7]:
        print(f"    {hhmm(t)}Z  {q:.1f} C")
    print(
        f"    with a 0.5 C deadband on the mean instead of rounding: {len(r['comp_db'])} value(s): "
        + ", ".join(f"{m:.2f}" for _, m in r["comp_db"])
    )
    print()

    print(f"case 8  one-state filter at {hhmm(last['t'])}Z: estimate {last['x']:.2f} C, sigma {last['sig']:.3f} C")
    m = night(mask=(ts("2026-11-09T13:00:00"), ts("2026-11-09T14:00:00")))
    print(
        f"    with the liquid reading hidden 13:00Z to 14:00Z: "
        f"sigma {m['mask_start']['sig']:.3f} -> {m['mask_end']['sig']:.3f} C; "
        f"estimate at 14:00Z {m['mask_end']['x']:.2f} C against a reading of {m['mask_end']['actual']:.2f} C"
    )
    print()

    # case 5: steady detection on the 2026-11-02 pH calibration
    start, end = ts("2026-11-02T14:45:00"), ts("2026-11-02T16:45:00")
    series = load(start, end, [PH])
    win, state, changes = Trailing(600), None, []
    for t, s in grid(series, start, end):
        if ok(s[PH]):
            win.push(t, s[PH].v)
        else:
            win.clear()
        slope, spread = win.slope_per_min(), win.spread()
        steady = None if slope is None else (abs(slope) < 0.002 and spread < 0.02)
        if steady != state:
            changes.append((t, steady, slope, spread, s[PH].v if s[PH] else None))
            state = steady
    print("case 5  pH steady = 10-min window full, |slope| < 0.002 pH/min, spread < 0.02 pH  (2026-11-02, EST)")
    print(f"    {len(changes)} state changes in 09:45 to 11:45 EST; the first ten:")
    for t, st, slope, spread, v in changes[:10]:
        label = {None: "unknown", True: "steady", False: "moving"}[st]
        extra = "" if slope is None else f"  slope {slope:+.4f}/min  spread {spread:.3f}  pH {v:.3f}"
        print(f"    {hhmm(t, EST)}  {label}{extra}")


def main():
    global RAW, GUARD_PY, ROOM_CSV
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--guard", type=Path, required=True, help="heater_guard.py from anolis-projects bioreactor-v1")
    p.add_argument("--archive", type=Path, help="directory of 30-minute raw_event CSV exports, gzipped")
    p.add_argument("--room-csv", type=Path, help="room temperature log for the overnight run")
    args = p.parse_args()
    GUARD_PY = args.guard
    part1()
    if args.archive and args.room_csv:
        RAW, ROOM_CSV = args.archive, args.room_csv
        part2()
    else:
        print("PART 2 skipped: needs --archive and --room-csv")


if __name__ == "__main__":
    main()
