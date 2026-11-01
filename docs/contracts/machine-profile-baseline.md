# Machine Profile Baseline

Status: Locked.

## Purpose

Define machine profile manifest structure and lock manifest behavior for validation tooling.

## Canonical Artifacts

1. Manifest schema: `schemas/machine/machine-profile.schema.json`
2. Validator: `tests/contracts/machine-profile/validate-machine-profiles.py`
3. Fixtures: `tests/contracts/machine-profile/`
4. Runtime config schema dependency: `schemas/runtime/runtime-config.schema.json`

## Locked Behavior Summary

1. Machine realizations live in external repos (e.g., `anolis-projects`).
2. Manifest file is `machine-profile.yaml`.
3. Manifest declares:
   - identity (`schema_version`, `machine_id`, `display_name`)
   - runtime profile entrypoints (`manual` required; optional `telemetry`, `automation`, `full`; any other variant name is also accepted)
   - provider config references
   - optional behavior asset references
   - runtime contract references and compatibility notes
   - optional pinned component versions (`components`: runtime/provider
     `repo`+`version` pairs and optional artifact `package`+`version` pairs,
     read by `tools/install.sh`; Renovate in `anolis-projects` opens PRs for the
     `repo`+`version` pairs only)
   - optional informational safety facts (`safety.estop_topology:
     power_cut | signal` — how the physical e-stop is wired, so operators and
     monitoring can interpret an e-stop event's software signature: `power_cut`
     shows as device blackout with `io_failed` accrual and recovery on
     release; `signal` shows as `estop=true` in device state with the bus
     alive. Both wirings are first-class; nothing may branch runtime behavior
     on this field beyond presentation/interpretation, and absence is valid)
   - an optional operator panel (`operator`, below): the readings an operator
     watches, the automation's settings in labelled groups, and what starting
     and stopping a run does. Presentation only: nothing in the
     runtime reads it, and absence is valid
4. Validator enforces:
   - schema correctness
   - referenced file existence
   - referenced runtime profile schema compatibility

## The `operator` block

Read by the workbench's operator panel (anolishq/anolis-workbench#232). Every
key is optional; a profile without the block is valid and behaves as before.

<!-- validate: machine-profile fragment -->
```yaml
operator:
  readings:                       # shown large, in this order
    - { signal: ezo0/ph0/ph_value, label: pH, decimals: 2 }
    - { signal: bread0/rlht0/t1_c, label: Temperature, unit: "°C", decimals: 1 }
  settings:                       # automation parameters, grouped and labelled
    - group: Stirring
      items:
        - { parameter: impeller_pwm, label: Speed }
  run:                            # what Start run and Stop run do
    label: Batch                  # the section's heading; default "Run"
    start:
      enable: [impeller_enable]
      mode: AUTO
      require:                    # checked before Start runs
        - { signal: bread0/dcmt0/motor1_brake, equals: false, message: "Release the impeller's brake", when: impeller_enable }
    stop: { disable: [impeller_enable], mode: IDLE }
  actions:                        # buttons; each press adds one to its counter
    - label: Sample now
      parameter: sample_requests
      require:                    # checked before a press
        - { signal: bread0/dcmt0/motor1_brake, equals: false, message: "Release the sample pump's brake" }
  trip_recovery:                  # steps after a watchdog-trip clear
    - device: bread0/dcmt0
      after_clear:
        - { label: Release both brakes, function: set_brake, args: { motor1_brake: false, motor2_brake: false }, confirm: true }
```

1. `readings[]`: `signal` is `provider/device/signal` and `label` is required;
   `unit` (non-empty) and `decimals` (0 to 6) are optional.
2. `settings[]`: a non-empty `group` name and at least one item; each item
   names an automation `parameter` and a non-empty `label`, with an optional
   `unit`.
3. `run.label` (optional, non-empty): what the operator panel heads the run
   section with, such as `Batch` or `Print`. The panel's default is "Run".
4. `run.start` and `run.stop`: the `mode` to enter (required). `start.enable`
   lists the groups' on switches (bool parameters): Start runs only if at
   least one is on, and doesn't change them (0.9.0; before, Start set them
   true). `stop.disable` lists the bool parameters Stop sets false. Start
   takes `IDLE`, `MANUAL` or `AUTO`; Stop takes `IDLE` or `MANUAL` only, so a
   Stop never leaves automation running.
5. `run.start.require[]` (optional): conditions that must hold before
   Start runs. Each names a `signal` (`provider/device/signal`), the value it
   must read (`equals`: a bool, number or string) and a non-empty `message`,
   the fix the operator sees when it doesn't. Start refuses while any does
   not hold. An optional `when` names a bool parameter, normally one of
   `start.enable`: the condition then applies only while that parameter reads
   true at Start, so a condition about one output does not refuse a run that
   leaves it off (0.9.0).
6. `experiment` (0.7.0 and 0.8.0) is a deprecated alias of `run`, with the
   same shape, accepted through 0.9.x. A profile carries one or the other: one
   with both is refused.
7. `actions[]` (optional, 0.9.0): buttons. Each has a non-empty `label` and
   names an int64 automation `parameter`, a counter the press increases by
   one. A tree reads it to run something once per press, such as
   `RequestedPulseWindow` (see `docs/automation.md`). An optional `require[]`
   (0.10.0) takes the same entries as `run.start.require[]`: a press is
   refused, and the counter left as it is, while any does not hold. A `when`
   applies at the press.
8. `trip_recovery[]` (optional): what the operator panel offers after
   clearing a device's watchdog trip. The clear itself is the provider SDK's
   reserved `clear_watchdog_trip` function, which needs no entry. Each entry
   names a `device` (`provider/device`) and its `after_clear` steps, in order:
   a non-empty `label` and `function`, optional `args` as plain bool, number or
   string values by argument name, and `confirm: true` for a step that
   actuates, which then takes a second click.
9. Unknown keys anywhere in the block are refused. The schema checks shape only:
   that a named signal or parameter exists in the machine's variants is the
   consumer's check.

## Validation Gates

1. `python3 tests/contracts/machine-profile/validate-machine-profiles.py`
2. Runtime config contract validation remains in place for referenced profiles.

## Consumers

The schema in this repo is the single source; it ships in every release as
`anolis-<version>-machine-profile-schema.tar.gz`. Downstream consumers (a
schema change here must be released, then propagated):

1. `anolis-workbench` — vendored copy byte-locked to a release artifact
   (`contracts/upstream/anolis/machine-profile.lock.json`); re-lock on
   schema-bearing releases.
2. `anolis-projects` — CI fetches the artifact at the release pinned in
   `schema-source.json` (Renovate bumps the pin).
3. Docs site (`anolishq.github.io`) — injects the artifact at its pinned
   release for the published contract page.

## Drift Notes and Change Rule

1. Keep machine profile schema additive when possible.
2. Path/layout changes require coordinated updates to manifest schema, validator, package docs, and fixtures.
3. Do not introduce packaging-repo split semantics in this baseline.
