#!/usr/bin/env python3
"""Derived-signals paper exercise: what a loader can check in each style.

Loads the three strawman files, checks they publish the same signals, builds
each one's dependency graph, and then breaks each file in memory four ways to
see whether the breakage is caught. Reads files; writes nothing.

Run:  python3 lint.py
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
FILES = {"A": "style-a-expressions.yaml", "B": "style-b-chains.yaml", "C": "style-c-blocks.yaml"}

# Style A cannot tell a signal name from a function name without this list.
FUNCS = {
    "median",
    "mean",
    "slope",
    "spread",
    "abs",
    "since",
    "ok",
    "held",
    "latch",
    "deadband",
    "integral",
    "if",
    "kalman1",
    "and",
    "or",
    "not",
    "reset",
    "hold",
}
IDENT = re.compile(r"(?<![\w.])[A-Za-z_]\w*(?!\w)(?!\s*=(?!=))")
DEVICE_SIGNAL = re.compile(r"^[a-z0-9_]+/[a-z0-9_]+/[a-z0-9_]+$")


class Problem(Exception):
    pass


def formula_names(text):
    return {m.group(0) for m in IDENT.finditer(str(text))} - FUNCS


def graph_a(doc):
    """node -> set of nodes it reads; plus the published ids."""
    local = {}
    for sig in doc["signals"]:
        short = sig.split("/")[-1]
        if short in local:
            raise Problem(f"two derived signals share the short name {short}")
        local[short] = sig
    known = set(doc["inputs"]) | set(doc["params"])
    deps = {}
    for sig, body in doc["signals"].items():
        names = formula_names(body["expr"])
        bad = names - known - set(local)
        if bad:
            raise Problem(f"{sig}: unknown name(s) {sorted(bad)}")
        deps[sig] = {local[n] for n in names if n in local}
    return deps, set(doc["signals"])


def graph_b(doc):
    ids = {s["id"] for s in doc["signals"]}
    deps, published = {}, set()

    def resolve(owner, ref):
        if ref.startswith("derived/"):
            target = ref[len("derived/") :]
            if target not in ids:
                raise Problem(f"{owner}: no derived signal {ref}")
            return {target}
        if not DEVICE_SIGNAL.match(ref):
            raise Problem(f"{owner}: {ref} is not provider/device/signal")
        return set()

    for s in doc["signals"]:
        d = set()
        if "source" in s:
            d |= resolve(s["id"], s["source"])
        if "combine" in s:
            c = s["combine"]
            for ref in c["inputs"].values():
                d |= resolve(s["id"], ref)
            bad = formula_names(c["formula"]) - set(c["inputs"]) - set(c.get("params", []))
            if bad:
                raise Problem(f"{s['id']}: formula uses undeclared name(s) {sorted(bad)}")
        if "block" in s:
            b = s["block"]
            for ref in b.get("inputs", {}).values():
                d |= resolve(s["id"], ref)
            if "same_as" in b:
                if b["same_as"] not in ids:
                    raise Problem(f"{s['id']}: same_as names no signal")
                d.add(b["same_as"])
        deps[s["id"]] = d
        if s.get("publish", True):
            published.add(s["id"])
    return deps, published


def graph_c(doc):
    ids = {b["id"] for b in doc["blocks"]}
    deps, published = {}, set()
    for b in doc["blocks"]:
        d = set()
        for ref in b.get("in", {}).values():
            if isinstance(ref, str) and not DEVICE_SIGNAL.match(ref):
                block = ref.split(".")[0]
                if "." not in ref or block not in ids:
                    raise Problem(f"{b['id']}: input {ref} names no block output or device signal")
                d.add(block)
        if b["type"] == "expr":
            bad = formula_names(b["params"]["formula"]) - set(b["in"])
            if bad:
                raise Problem(f"{b['id']}: formula uses undeclared name(s) {sorted(bad)}")
        deps[b["id"]] = d
        for port in b.get("publish", {}).values():
            published.add(port["signal"])
    return deps, published


def order(deps):
    """Evaluation order, or raise on a cycle."""
    done, out = set(), []
    while len(out) < len(deps):
        ready = sorted(n for n in deps if n not in done and deps[n] <= done)
        if not ready:
            raise Problem(f"cycle among {sorted(set(deps) - done)}")
        out += ready
        done |= set(ready)
    return out


GRAPH = {"A": graph_a, "B": graph_b, "C": graph_c}


def check(style, text):
    doc = yaml.safe_load(text)
    deps, published = GRAPH[style](doc)
    return doc, deps, published, order(deps)


def logical_lines(text):
    return sum(1 for line in text.splitlines() if line.strip() and not line.strip().startswith("#"))


def count_formulas(style, doc):
    if style == "A":
        return len(doc["signals"])
    if style == "B":
        return sum(1 for s in doc["signals"] if "combine" in s)
    return sum(1 for b in doc["blocks"] if b["type"] == "expr")


# (label, {style: (old, new)}); each old string must occur exactly once.
MUTATIONS = [
    (
        "misspelt reference to another derived signal",
        {
            "A": ("expr: pad_c - liquid_c", "expr: pad_c - liquid_cc"),
            "B": ("liquid: derived/heater/liquid_c }", "liquid: derived/heater/liquid_cc }"),
            "C": ("in: { a: pad.y, b: liquid.y }", "in: { a: pad.y, b: liquidd.y }"),
        },
    ),
    (
        "dependency loop",
        {
            "A": ("expr: t1 - liquid_bias_c", "expr: t1 - liquid_bias_c + pad_liquid_gap_c"),
            "B": (
                "source: bread0/rlht0/t1_c\n    filters:\n      - offset:",
                "source: derived/heater/pad_liquid_gap_c\n    filters:\n      - offset:",
            ),
            "C": (
                "in: { a: bread0/rlht0/t1_c, b: { param: liquid_bias_c } }",
                "in: { a: gap.y, b: { param: liquid_bias_c } }",
            ),
        },
    ),
    (
        "misspelt name inside a formula",
        {
            "A": ("expr: median(t2, 3)", "expr: medain(t2, 3)"),
            "B": ("formula: pad - liquid", "formula: pad - liquidd"),
            "C": ("formula: t2 < t1 - 3", "formula: t2 < t11 - 3"),
        },
    ),
    (
        "misspelt setting of a filter or block",
        {
            "A": ("expr: median(t2, 3)", "expr: median(t2, samples=3, widnow=3)"),
            "B": ("- median: { samples: 3 }", "- median: { sampels: 3 }"),
            "C": ("params: { samples: 3 }", "params: { sampels: 3 }"),
        },
    ),
]


def main():
    texts = {k: (HERE / f).read_text() for k, f in FILES.items()}
    results = {k: check(k, t) for k, t in texts.items()}
    pub = {k: r[2] for k, r in results.items()}
    same = pub["A"] == pub["B"] == pub["C"]
    print(f"published signals: A {len(pub['A'])}, B {len(pub['B'])}, C {len(pub['C'])}; identical sets: {same}")
    print()
    print(f"{'':28s}{'A expressions':>16s}{'B chains':>16s}{'C blocks':>16s}")
    rows = [
        ("non-comment lines", lambda k: logical_lines(texts[k])),
        ("declared nodes", lambda k: len(results[k][1])),
        (
            "nodes not published",
            lambda k: (
                len(results[k][1]) - len(pub[k])
                if k != "C"
                else sum(1 for b in results[k][0]["blocks"] if "publish" not in b)
            ),
        ),
        ("formula strings", lambda k: count_formulas(k, results[k][0])),
    ]
    for label, fn in rows:
        print(f"{label:28s}" + "".join(f"{fn(k):>16}" for k in "ABC"))
    print()

    print("breakage test: is each mistake caught, and by what")
    for label, per_style in MUTATIONS:
        cells = []
        for k in "ABC":
            old, new = per_style[k]
            before = texts[k]
            if before.count(old) != 1:
                cells.append(f"ANCHOR x{before.count(old)}")
                continue
            after = before.replace(old, new)
            if hashlib.sha256(before.encode()).digest() == hashlib.sha256(after.encode()).digest():
                cells.append("BROKEN")
                continue
            try:
                check(k, after)
                cells.append("not caught")
            except Problem as exc:
                cells.append("caught")
                per_style.setdefault("why", {})[k] = str(exc)
        print(f"  {label:44s}" + "".join(f"{c:>14s}" for c in cells))
        for k, why in per_style.get("why", {}).items():
            print(f"      {k}: {why}")


if __name__ == "__main__":
    main()
