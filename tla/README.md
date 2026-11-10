# Mode dispatch model

**Status: non-normative.** `ModeDispatch.tla` is a bounded model of a design. The
code and `docs/safety.md` are authoritative. If the model and either of them
disagree, that is a defect to triage in one of the three. Promotion conditions and
the 90-day review are tracked in anolishq/anolis#772.

A passing check says nothing about the C++. Describe this as a bounded model of a
design, never as more than that: `check.py` fails on stronger wording in tracked
Markdown.

## What it covers

How these interleave, for two ordinary `set_mode` callers and one FAULT caller:

- `ModeManager`'s request, before-callback, commit and after-callback phases
- the behaviour tree's tick and a node waiting for the provider lock
- a leave-AUTO hook declared before or after the commit
- the software-stop latch and its ladder, and a restart that leaves AUTO uncovered

Each guard under test is a named operator. A file in `regressions/` replaces one
with the behaviour of a past or suspected bug.

The code paths it stands for are `core/automation/mode_manager.cpp`,
`core/control/call_router.cpp` (from the provider lock onwards),
`core/automation/bt_runtime.cpp` (the tick loop) and
`core/runtime/actuation_gate.cpp`.

## What it does not cover

Device loss and the device-loss latch, provider supervision, ADPP, timing, argument
validation, and failure counting. A provider is an environment that answers, and
one output on one provider stands for the machine.

Hooks are abstracted hard. A leave-AUTO hook makes the output safe unless the
`IDLE` gate refuses its call (#776). An after-hook meets that gate if `IDLE`
commits before it runs; a before-hook only after another exit from AUTO has run
its own hooks. A leave-AUTO hook is assumed to cover every exit from AUTO. A
hook on entering AUTO does not actuate, and no other refusal of a hook's call is
modelled, `allowed_modes` included. The stop's ladder always makes the output
safe: a stop chain hold that sends nothing, a plan that drives nothing, and a
latch restored at startup are not modelled.

A FAULT request made while already in FAULT returns at once, as `set_mode` does,
but a recovery from FAULT that it overlaps then does not commit (#774). A restart
that faults a machine in AUTO and a transition vetoed by a callback are not
modelled. There is no liveness property: `FaultAlwaysCommits` only says a FAULT
request is not refused at its commit and not overtaken by a recovery.

## Run it

```sh
just test-tla
```

This needs Java 11 or later. `check.py` fetches the `tla2tools.jar` pinned in
`.ci/dependency-pins.yml` into `.tla-tools/`, checks its hash, and fails on any
mismatch.

## Rules

- `CONSTRAINT`, `ACTION_CONSTRAINT` and `CHECK_DEADLOCK` are not allowed.
- `regressions/` and `witnesses/` are append-only. CI fails if an existing file
  changes. Add a new one instead, and say why in the PR.
- A new invariant needs a regression that makes it fail. The invariant set is pinned
  in `check.py`.
- `check.py` requires the exact content of the two passing configurations. Invariant
  and guard text in `ModeDispatch.tla` is not frozen, so a reviewer has to read any
  change to that file.
- State counts are pinned in `check.py`. If one changes, say why in the PR.
- Every action must be taken at least once. An action that never fires is a dead
  part of the model.
- A counterexample that applies to the code becomes a deterministic component test,
  in the same PR as the fix. See `tests/unit/mode_dispatch_test.cpp`.
- Provider models and other repositories' models are not imported here.
