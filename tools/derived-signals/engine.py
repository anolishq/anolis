#!/usr/bin/env python3
"""Derived-signals spike: a block file made executable (RFC open decision O3).

Loads a file of typed blocks, checks it against a catalogue of block types,
and evaluates it one tick at a time. It exists to give the block file a
meaning that can be tested, and to be the oracle a later implementation is
compared with. It is not part of the runtime.

Every value on a wire has one of three states (RFC O1):
  good       a usable value
  not_ready  no value yet: a window still filling, a filter not started
  bad        an input was not good

Needs PyYAML to load a file; the engine itself is stdlib only.
"""

from __future__ import annotations

import ast
import collections
import re
import statistics

GOOD, NOT_READY, BAD = "good", "not_ready", "bad"
Val = collections.namedtuple("Val", "state v")
NOT_READY_VAL = Val(NOT_READY, None)
BAD_VAL = Val(BAD, None)
DEVICE_SIGNAL = re.compile(r"^[a-z0-9_]+/[a-z0-9_]+/[a-z0-9_]+$")
DURATION = re.compile(r"^(\d+(?:\.\d+)?)(s|m|h)$")


class Problem(Exception):
    """The block file is wrong. Raised at load, never while ticking."""


def good(v):
    return Val(GOOD, v)


def worst(vals):
    """The state a strict operation takes from its operands."""
    states = {v.state for v in vals}
    return BAD if BAD in states else NOT_READY if NOT_READY in states else GOOD


def from_sample(sample):
    """A cached device signal (value, quality, age) as a wire value."""
    if sample is None:
        return NOT_READY_VAL
    return good(sample.v) if sample.q == "OK" else BAD_VAL


def seconds(text):
    if isinstance(text, (int, float)) and not isinstance(text, bool):
        return float(text)
    m = DURATION.match(str(text))
    if not m:
        raise Problem(f"{text!r} is not a duration (use 30s, 10m, 1h or a number of seconds)")
    return float(m.group(1)) * {"s": 1, "m": 60, "h": 3600}[m.group(2)]


# ---------------------------------------------------------------- formulas

_BIN = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.Div: lambda a, b: a / b,
}
_CMP = {
    ast.Lt: lambda a, b: a < b,
    ast.LtE: lambda a, b: a <= b,
    ast.Gt: lambda a, b: a > b,
    ast.GtE: lambda a, b: a >= b,
    ast.Eq: lambda a, b: a == b,
    ast.NotEq: lambda a, b: a != b,
}
_FUNCS = {"abs": (abs, 1), "min": (min, 2), "max": (max, 2)}


class Formula:
    """Arithmetic, comparison and three-valued logic over declared names."""

    def __init__(self, text, names):
        try:
            self.tree = ast.parse(str(text), mode="eval").body
        except SyntaxError as exc:
            raise Problem(f"formula {text!r} does not parse: {exc.msg}") from exc
        self._check(self.tree, set(names))

    def _check(self, node, names):
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float, bool)):
            return
        if isinstance(node, ast.Name):
            if node.id not in names:
                raise Problem(f"formula uses undeclared name {node.id!r}")
            return
        if isinstance(node, ast.BinOp) and type(node.op) in _BIN:
            children = [node.left, node.right]
        elif isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.Not)):
            children = [node.operand]
        elif isinstance(node, ast.BoolOp):
            children = node.values
        elif isinstance(node, ast.Compare) and len(node.ops) == 1 and type(node.ops[0]) in _CMP:
            children = [node.left, node.comparators[0]]
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and not node.keywords:
            name, n = node.func.id, len(node.args)
            if name == "if":
                raise Problem("'if' is a Python keyword; write when(condition, a, b)")
            if name != "when" and name not in _FUNCS:
                raise Problem(f"formula calls unknown function {name!r}")
            if n != (3 if name == "when" else _FUNCS[name][1]):
                raise Problem(f"formula calls {name} with {n} argument(s)")
            children = node.args
        else:
            raise Problem(f"formula uses unsupported syntax ({type(node).__name__})")
        for child in children:
            self._check(child, names)

    def eval(self, env):
        return self._eval(self.tree, env)

    def _eval(self, node, env):
        if isinstance(node, ast.Constant):
            return good(node.value)
        if isinstance(node, ast.Name):
            return env[node.id]
        if isinstance(node, ast.BoolOp):
            vals = [self._eval(v, env) for v in node.values]
            decides = not isinstance(node.op, ast.And)  # one true decides an "or", one false an "and"
            if any(v.state == GOOD and bool(v.v) == decides for v in vals):
                return good(decides)
            state = worst(vals)
            return good(not decides) if state == GOOD else Val(state, None)
        if isinstance(node, ast.Call) and node.func.id == "when":
            cond = self._eval(node.args[0], env)
            if cond.state != GOOD:
                return Val(cond.state, None)
            return self._eval(node.args[1] if cond.v else node.args[2], env)
        if isinstance(node, ast.UnaryOp):
            vals = [self._eval(node.operand, env)]
            fn = (lambda a: not a) if isinstance(node.op, ast.Not) else (lambda a: -a)
        elif isinstance(node, ast.BinOp):
            vals = [self._eval(node.left, env), self._eval(node.right, env)]
            fn = _BIN[type(node.op)]
        elif isinstance(node, ast.Compare):
            vals = [self._eval(node.left, env), self._eval(node.comparators[0], env)]
            fn = _CMP[type(node.ops[0])]
        else:
            vals = [self._eval(a, env) for a in node.args]
            fn = _FUNCS[node.func.id][0]
        state = worst(vals)
        if state != GOOD:
            return Val(state, None)
        try:
            return good(fn(*(v.v for v in vals)))
        except ZeroDivisionError:
            return BAD_VAL


# ---------------------------------------------------------------- blocks


class Block:
    inputs: dict = {}  # name -> required; None means "any names"
    params: dict = {}  # name -> (kind, required, choices or None)
    outputs: tuple = ("y",)

    def __init__(self, params, dt, used):
        self.p, self.dt, self.used = params, dt, used

    def step(self, t, ins):
        raise NotImplementedError


class Median(Block):
    """Median of the last `samples` good values."""

    inputs = {"x": True}
    params = {"samples": ("int", True, None)}

    def __init__(self, *a):
        super().__init__(*a)
        self.buf = collections.deque(maxlen=self.p["samples"])

    def step(self, t, ins):
        if ins["x"].state == GOOD:
            self.buf.append(ins["x"].v)
        full = len(self.buf) == self.buf.maxlen
        return {"y": good(statistics.median(self.buf)) if full else NOT_READY_VAL}


class Expr(Block):
    """A formula over the block's own inputs."""

    inputs = None
    params = {"formula": ("str", True, None)}

    def bind(self, names):
        self.formula = Formula(self.p["formula"], names)

    def step(self, t, ins):
        return {"y": self.formula.eval(ins)}


class AllGood(Block):
    """True when every input is good. Its own output is always good."""

    inputs = None

    def step(self, t, ins):
        return {"y": good(all(v.state == GOOD for v in ins.values()))}


class SinceTrue(Block):
    """True when the input has not been true for longer than `longer_than_s`."""

    inputs = {"x": True}
    params = {"longer_than_s": ("number", True, None)}

    def __init__(self, *a):
        super().__init__(*a)
        self.last = None

    def step(self, t, ins):
        x = ins["x"]
        if self.last is None or (x.state == GOOD and x.v):
            self.last = t
        return {"y": good(t - self.last > self.p["longer_than_s"])}


class Latch(Block):
    """False until the input is first true, then true."""

    inputs = {"set": True}

    def __init__(self, *a):
        super().__init__(*a)
        self.on = False

    def step(self, t, ins):
        self.on = self.on or (ins["set"].state == GOOD and bool(ins["set"].v))
        return {"y": good(self.on)}


class HeldFor(Block):
    """True once the input has been true for longer than `seconds`.

    on_bad says what an input that is not good does to the timer:
    reset stops it; hold leaves it running.
    """

    inputs = {"x": True}
    params = {"seconds": ("number", True, None), "on_bad": ("str", True, ("reset", "hold"))}

    def __init__(self, *a):
        super().__init__(*a)
        self.since = None

    def step(self, t, ins):
        x = ins["x"]
        if x.state == GOOD:
            self.since = (self.since if self.since is not None else t) if x.v else None
            return {"y": good(self.since is not None and t - self.since > self.p["seconds"])}
        if self.p["on_bad"] == "reset" or self.since is None:
            self.since = None
            return {"y": Val(x.state, None)}
        return {"y": good(t - self.since > self.p["seconds"])}


class WindowStats(Block):
    """Mean, slope and spread of the good values in a trailing window.

    on_bad says what an input that is not good does to the window:
    skip leaves it as it is; reset empties it.
    """

    inputs = {"x": True}
    params = {"window": ("duration", True, None), "on_bad": ("str", True, ("skip", "reset"))}
    outputs = ("mean", "slope_per_min", "spread")

    def __init__(self, *a):
        super().__init__(*a)
        self.n = round(self.p["window"] / self.dt)
        self.buf = collections.deque(maxlen=self.n)

    def step(self, t, ins):
        x = ins["x"]
        if x.state == GOOD:
            self.buf.append((t, float(x.v)))
        elif self.p["on_bad"] == "reset":
            self.buf.clear()
        if len(self.buf) < self.n:
            return dict.fromkeys(self.outputs, NOT_READY_VAL)
        out = {}
        ys = [v for _, v in self.buf]
        if "mean" in self.used:
            out["mean"] = good(sum(ys) / len(ys))
        if "spread" in self.used:
            out["spread"] = good(max(ys) - min(ys))
        if "slope_per_min" in self.used:
            t0 = self.buf[0][0]
            xs = [(ti - t0) / 60.0 for ti, _ in self.buf]
            mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
            den = sum((xi - mx) ** 2 for xi in xs)
            out["slope_per_min"] = good(sum((xi - mx) * (yi - my) for xi, yi in zip(xs, ys, strict=True)) / den)
        return out


class Deadband(Block):
    """Holds its output until the input has moved by `step` or more."""

    inputs = {"x": True}
    params = {"step": ("number", True, None)}

    def __init__(self, *a):
        super().__init__(*a)
        self.last = None

    def step(self, t, ins):
        x = ins["x"]
        if x.state != GOOD:
            return {"y": Val(x.state, None)}
        if self.last is None or abs(x.v - self.last) >= self.p["step"]:
            self.last = x.v
        return {"y": good(self.last)}


class Kalman1(Block):
    """One-state filter: relaxes toward `steady` with time constant tau_s, corrected by `z`."""

    inputs = {"z": True, "steady": True}
    params = {
        "tau_s": ("number", True, None),
        "q": ("number", True, None),
        "r": ("number", True, None),
        "p0": ("number", False, None),
    }
    outputs = ("x", "sigma")

    def __init__(self, *a):
        super().__init__(*a)
        self.x = None
        self.var = self.p.get("p0", 4.0)

    def step(self, t, ins):
        z = ins["z"].v if ins["z"].state == GOOD else None
        steady = ins["steady"].v if ins["steady"].state == GOOD else None
        if self.x is None:
            if z is None:
                return dict.fromkeys(self.outputs, NOT_READY_VAL)
            self.x, self.var = z, self.p["r"]
            return {"x": good(self.x), "sigma": good(self.var**0.5)}
        if steady is not None:
            a = 1.0 - self.dt / self.p["tau_s"]
            self.x = a * self.x + (1 - a) * steady
            self.var = a * a * self.var + self.p["q"] * self.dt
        else:
            self.var += self.p["q"] * self.dt
        if z is not None:
            k = self.var / (self.var + self.p["r"])
            self.x += k * (z - self.x)
            self.var *= 1 - k
        return {"x": good(self.x), "sigma": good(self.var**0.5)}


class Integrate(Block):
    """Running total of the input over time, in input units times `per`.

    persist and reset_on are accepted and recorded. This spike keeps the
    total in memory and never resets it (RFC O5).
    """

    inputs = {"x": True}
    params = {
        "per": ("duration", True, None),
        "on_bad": ("str", True, ("skip",)),
        "persist": ("bool", False, None),
        "reset_on": ("str", False, ("run_start",)),
    }

    def __init__(self, *a):
        super().__init__(*a)
        self.total = 0.0

    def step(self, t, ins):
        if ins["x"].state == GOOD:
            self.total += float(ins["x"].v) * self.dt / self.p["per"]
        return {"y": good(self.total)}


CATALOGUE = {
    "median": Median,
    "expr": Expr,
    "all_good": AllGood,
    "since_true": SinceTrue,
    "latch": Latch,
    "held_for": HeldFor,
    "window_stats": WindowStats,
    "deadband": Deadband,
    "kalman1": Kalman1,
    "integrate": Integrate,
}

_KINDS = {
    "int": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "str": lambda v: isinstance(v, str),
    "bool": lambda v: isinstance(v, bool),
    "duration": lambda v: True,  # checked by seconds()
}
_BLOCK_KEYS = {"id", "type", "in", "params", "publish", "on_bad"}


# ---------------------------------------------------------------- the engine


class Engine:
    def __init__(self, doc, params=None):
        self.dt = doc["period_ms"] / 1000.0
        self.constants = doc.get("constants", {})
        self.params = params or {}
        specs = doc["blocks"]
        ids = [b.get("id") for b in specs]
        if len(set(ids)) != len(ids):
            raise Problem("two blocks share an id")
        by_id = {b["id"]: b for b in specs}

        self.device_signals, self.published = set(), {}
        used = collections.defaultdict(set)
        wiring, deps = {}, {}
        for b in specs:
            where = f"block {b['id']}"
            if set(b) - _BLOCK_KEYS:
                raise Problem(f"{where}: unknown key(s) {sorted(set(b) - _BLOCK_KEYS)}")
            if b.get("type") not in CATALOGUE:
                raise Problem(f"{where}: unknown type {b.get('type')!r}")
            cls = CATALOGUE[b["type"]]
            given = b.get("in", {})
            if cls.inputs is not None:
                if set(given) - set(cls.inputs):
                    raise Problem(f"{where}: unknown input(s) {sorted(set(given) - set(cls.inputs))}")
                missing = [n for n, req in cls.inputs.items() if req and n not in given]
                if missing:
                    raise Problem(f"{where}: missing input(s) {missing}")
            elif not given:
                raise Problem(f"{where}: needs at least one input")
            wiring[b["id"]] = {n: self._source(where, ref, by_id, used) for n, ref in given.items()}
            deps[b["id"]] = {src[1] for src in wiring[b["id"]].values() if src[0] == "block"}
            for port, pub in b.get("publish", {}).items():
                if port not in cls.outputs:
                    raise Problem(f"{where}: publishes unknown output {port!r}")
                if pub["signal"] in self.published:
                    raise Problem(f"{where}: {pub['signal']} is published twice")
                self.published[pub["signal"]] = (b["id"], port, pub.get("unit"))
                used[b["id"]].add(port)
            if "on_bad" in b:
                self._fallback(where, b["on_bad"])

        self.order = self._order(deps)
        self.blocks, self.wiring, self.on_bad, self.last_good = {}, wiring, {}, {}
        for bid in self.order:
            b = by_id[bid]
            cls = CATALOGUE[b["type"]]
            block = cls(self._params(f"block {bid}", cls, b.get("params", {})), self.dt, used[bid])
            if isinstance(block, Expr):
                try:
                    block.bind(b["in"])
                except Problem as exc:
                    raise Problem(f"block {bid}: {exc}") from exc
            self.blocks[bid] = block
            if "on_bad" in b:
                self.on_bad[bid] = b["on_bad"]

    def _source(self, where, ref, by_id, used):
        if isinstance(ref, dict):
            if set(ref) == {"param"}:
                if ref["param"] not in self.params:
                    raise Problem(f"{where}: no parameter {ref['param']!r}")
                return ("param", ref["param"])
            if set(ref) == {"const"}:
                if ref["const"] not in self.constants:
                    raise Problem(f"{where}: no constant {ref['const']!r}")
                return ("value", self.constants[ref["const"]])
            raise Problem(f"{where}: input {ref!r} is neither {{param: name}} nor {{const: name}}")
        if isinstance(ref, (int, float, bool)):
            return ("value", ref)
        if DEVICE_SIGNAL.match(ref):
            self.device_signals.add(ref)
            return ("signal", ref)
        bid, _, port = ref.partition(".")
        if bid not in by_id or by_id[bid].get("type") not in CATALOGUE:
            raise Problem(f"{where}: input {ref!r} names no block or device signal")
        if port not in CATALOGUE[by_id[bid]["type"]].outputs:
            raise Problem(f"{where}: block {bid} has no output {port!r}")
        used[bid].add(port)
        return ("block", bid, port)

    def _fallback(self, where, spec):
        if spec == "hold":
            return
        if not (isinstance(spec, dict) and set(spec) == {"use"}):
            raise Problem(f"{where}: on_bad must be 'hold' or {{use: ...}}")
        self._source(where, spec["use"], {}, {})

    def _params(self, where, cls, given):
        if set(given) - set(cls.params):
            raise Problem(f"{where}: unknown setting(s) {sorted(set(given) - set(cls.params))}")
        out = {}
        for name, (kind, required, choices) in cls.params.items():
            if name not in given:
                if required:
                    raise Problem(f"{where}: missing setting {name!r}")
                continue
            value = given[name]
            if not _KINDS[kind](value):
                raise Problem(f"{where}: setting {name!r} must be a {kind}")
            if choices and value not in choices:
                raise Problem(f"{where}: setting {name!r} must be one of {list(choices)}")
            try:
                out[name] = seconds(value) if kind == "duration" else value
            except Problem as exc:
                raise Problem(f"{where}: {exc}") from exc
        return out

    @staticmethod
    def _order(deps):
        done, out = set(), []
        while len(out) < len(deps):
            ready = sorted(n for n in deps if n not in done and deps[n] <= done)
            if not ready:
                raise Problem(f"dependency loop among {sorted(set(deps) - done)}")
            out += ready
            done |= set(ready)
        return out

    def _read(self, src, wires, signals):
        if src[0] == "signal":
            return from_sample(signals.get(src[1]))
        if src[0] == "block":
            return wires[(src[1], src[2])]
        return good(self.params[src[1]] if src[0] == "param" else src[1])

    def tick(self, t, signals):
        """Evaluate every block once. `signals` maps a device signal to its cached sample."""
        wires = {}
        for bid in self.order:
            block = self.blocks[bid]
            ins = {n: self._read(src, wires, signals) for n, src in self.wiring[bid].items()}
            for port, val in block.step(t, ins).items():
                if val.state == GOOD:
                    self.last_good[(bid, port)] = val
                elif bid in self.on_bad:
                    spec = self.on_bad[bid]
                    if spec == "hold":
                        val = self.last_good.get((bid, port), val)
                    else:
                        val = self._read(self._source(f"block {bid}", spec["use"], {}, {}), wires, signals)
                wires[(bid, port)] = val
        return {sig: wires.get((bid, port), NOT_READY_VAL) for sig, (bid, port, _) in self.published.items()}


def load(path, params=None):
    import yaml

    with open(path) as fh:
        return Engine(yaml.safe_load(fh), params)
