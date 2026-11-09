#!/usr/bin/env python3
"""Check the bounded model in this directory. See README.md for the rules."""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SPEC = "ModeDispatch.tla"
PINS = ROOT / ".ci" / "dependency-pins.yml"
JAR = ROOT / ".tla-tools" / "tla2tools.jar"

TLC_OK = 0
TLC_INVARIANT_VIOLATED = 12

# Exact state counts. A narrowed guard or a shrunken model shows up as a changed
# number in the diff, which a reviewer has to explain.
PASSING = {"before-hook.cfg": ("before", 983625), "after-hook.cfg": ("after", 1069497)}

# Each must fail on exactly this invariant. A failure for any other reason, a
# parse error included, is not a pass.
MUST_FAIL = {
    "regressions/auto-gate-ignores-stop-latch.cfg": "NoAutoEntryWhileLatched",
    "regressions/issue-239-no-commit-gate.cfg": "NoAutoWhenHookless",
    "regressions/issue-528-fault-refused.cfg": "FaultAlwaysCommits",
    "regressions/issue-762-after-hook.cfg": "SafeWhenSettledOutsideAuto",
    "regressions/issue-762-before-hook.cfg": "SafeWhenSettledOutsideAuto",
    "regressions/issue-762-mode-recheck-only.cfg": "SafeWhenSettledOutsideAuto",
    "regressions/issue-763-ordinary-during-fault.cfg": "SafeWhenSettledOutsideAuto",
    "regressions/stop-latch-not-rechecked.cfg": "SafeWhileStopHolds",
    "witnesses/fault-from-auto.cfg": "NeverFaultFromAuto",
    "witnesses/hookless.cfg": "NeverHookless",
    "witnesses/output-driven.cfg": "NeverDriven",
    "witnesses/stop-holds.cfg": "NeverStopHolds",
}

INVARIANTS = (
    "TypeOK",
    "SafeWhenSettledOutsideAuto",
    "FaultAlwaysCommits",
    "NoAutoWhenHookless",
    "NoAutoEntryWhileLatched",
    "SafeWhileStopHolds",
)

WEAKENING = re.compile(r"\b(ACTION_CONSTRAINTS?|CONSTRAINTS?|CHECK_DEADLOCK)\b")
OVERCLAIM = re.compile(r"formally verified|provably|proven safe|mathematically", re.IGNORECASE)
COVERAGE = re.compile(r"^<(\w+) line [^>]*>: (\d+):(\d+)$", re.MULTILINE)


class CheckError(Exception):
    """A check failed; the message says which and why."""


def pinned_tool() -> tuple[str, str]:
    match = re.search(r"tla_tools:\n\s+repository: \S+\n\s+ref: (\S+)\n\s+sha256: (\w+)", PINS.read_text())
    if match is None:
        raise CheckError(f"no tla_tools entry in {PINS}")
    return match.group(1), match.group(2)


def ensure_jar() -> None:
    ref, expected = pinned_tool()
    if not JAR.exists():
        JAR.parent.mkdir(exist_ok=True)
        url = f"https://github.com/tlaplus/tlaplus/releases/download/{ref}/tla2tools.jar"
        print(f"fetching {url}")
        with urllib.request.urlopen(url, timeout=120) as response:  # noqa: S310 - fixed https URL, hash checked below
            JAR.write_bytes(response.read())
    actual = hashlib.sha256(JAR.read_bytes()).hexdigest()
    if actual != expected:
        JAR.unlink()
        raise CheckError(f"tla2tools.jar sha256 is {actual}, pinned {expected}")


def java(tmp: Path, *args: str) -> subprocess.CompletedProcess[str]:
    # TLC unpacks its standard modules into Java's temp directory.
    command = ["java", f"-Djava.io.tmpdir={tmp}", "-XX:+UseParallelGC", "-cp", str(JAR), *args]
    return subprocess.run(command, cwd=HERE, capture_output=True, text=True, check=False)


def tlc(tmp: Path, config: str, *flags: str) -> subprocess.CompletedProcess[str]:
    meta = tmp / config.replace("/", "-")
    return java(tmp, "tlc2.TLC", "-workers", "1", "-metadir", str(meta), *flags, "-config", config, SPEC)


def check_no_weakening() -> None:
    for config in sorted(HERE.rglob("*.cfg")):
        found = WEAKENING.search(config.read_text())
        if found:
            raise CheckError(f"{config.relative_to(ROOT)} uses {found.group(1)}, which is not allowed")
    known = set(PASSING) | set(MUST_FAIL)
    actual = {str(path.relative_to(HERE)) for path in HERE.rglob("*.cfg")}
    if actual != known:
        raise CheckError(f"configurations not matching the manifest: {sorted(actual ^ known)}")


def expected_lines(hook: str) -> list[str]:
    fixed = ["INIT Init", "NEXT Next", "CONSTANT Ops = {o1, o2}", f'CONSTANT HookAt = "{hook}"']
    return fixed + [f"INVARIANT {name}" for name in INVARIANTS]


def check_config_is_exact(config: str, hook: str) -> None:
    # Compared as text, not parsed: a substitution or a block comment could make
    # TLC check something other than what a parser saw.
    lines = [line for line in (HERE / config).read_text().splitlines() if not line.startswith("\\*")]
    if lines != expected_lines(hook):
        raise CheckError(f"{config} differs from the configuration check.py expects")


def check_passing(tmp: Path, config: str, hook: str, states: int) -> None:
    check_config_is_exact(config, hook)
    result = tlc(tmp, config, "-coverage", "1")
    if result.returncode != TLC_OK:
        raise CheckError(f"{config}: expected exit 0, got {result.returncode}\n{result.stdout[-3000:]}")
    distinct = re.search(r"(\d+) distinct states found, 0 states left", result.stdout)
    if distinct is None or int(distinct.group(1)) != states:
        found = distinct.group(1) if distinct else "none"
        raise CheckError(f"{config}: expected {states} distinct states, found {found}")
    # new:taken counts per action, so an action never taken is a dead one.
    actions = COVERAGE.findall(result.stdout)
    if not actions:
        raise CheckError(f"{config}: TLC reported no action coverage")
    dead = [name for name, _, taken in actions if int(taken) == 0]
    if dead:
        raise CheckError(f"{config}: actions never taken: {dead}")
    print(f"ok   {config}: {states} states")


def check_must_fail(tmp: Path, config: str, invariant: str) -> None:
    result = tlc(tmp, config)
    wanted = f"Invariant {invariant} is violated"
    if result.returncode != TLC_INVARIANT_VIOLATED or wanted not in result.stdout:
        raise CheckError(
            f"{config}: expected '{wanted}' with exit 12, got {result.returncode}\n{result.stdout[-3000:]}"
        )
    print(f"ok   {config}: fails on {invariant}")


def check_frozen(base: str) -> None:
    # Regressions and witnesses are append-only: weakening one must be visible.
    args = ["git", "diff", "--name-status", base, "--", "tla/regressions", "tla/witnesses"]
    output = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=True).stdout
    changed = [line for line in output.splitlines() if not line.startswith("A")]
    if changed:
        raise CheckError(f"regressions and witnesses are append-only, but these changed: {changed}")


def check_wording() -> None:
    tracked = subprocess.run(["git", "ls-files", "*.md"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    for name in tracked.splitlines():
        text = (ROOT / name).read_text(errors="replace")
        found = OVERCLAIM.search(text)
        if found:
            raise CheckError(f"{name} says '{found.group(0)}', which a bounded model does not support")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="git ref whose regressions and witnesses must be unchanged")
    args = parser.parse_args()
    if shutil.which("java") is None:
        print("error: java is not on PATH; tla2tools.jar needs Java 11 or later", file=sys.stderr)
        return 2
    try:
        check_wording()
        check_no_weakening()
        if args.base:
            check_frozen(args.base)
        ensure_jar()
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for config, (hook, states) in PASSING.items():
                check_passing(tmp, config, hook, states)
            for config, invariant in MUST_FAIL.items():
                check_must_fail(tmp, config, invariant)
    except CheckError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print("model checks passed; this says nothing about the C++, see tla/README.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
