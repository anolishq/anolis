# RFC: Anolis Automation Platform (engine seam + run/experiment identity, epic #111)

Status: implemented in v0.1.24, v0.1.25 and v0.1.26.

## Problem

Two coupled gaps in the runtime.

BehaviorTree.CPP vocabulary reached the HTTP API, the schemas and the UI.
`GET /v0/automation/status` reported `bt_status`, tick counters and a
`current_tree` that was a file name, not a content identity. Any future engine
would have inherited that vocabulary.

There was no run or experiment identity. Nothing recorded which automation a
run executed, and nothing tied telemetry to a run. The obvious fix, a `run_id`
tag on `anolis_signal`, would multiply series cardinality by the number of
runs.

The guiding constraint was to build the seam and defer the platform: one
engine existed, so nothing was built for a second one.

## Decisions

### Engine seam

- `IAutomationEngine` is an internal C++ interface in `core/automation/`, and
  `BTRuntime` implements it. No other repository links the runtime's C++, so
  it need not be a cross-repo contract. The wire taxonomy is declared once, in
  the HTTP OpenAPI contract.
- Rejected: putting the seam or taxonomy in `anolis-protocol`. That repository
  is ADPP, the boundary below the runtime. Automation status is above it, and
  the HTTP contract already carries it.
- Rejected for now: an engine registry, capability declaration, a generic
  definition envelope and a separate run-admission interface. With one engine
  they have no caller.
- `load()` returns ok or an error, which is the hook a policy layer would use.
  It is also the single point where a definition is swapped, where live reload
  would attach.

### Status taxonomy

- Three separate axes: the global mode (`IDLE`, `MANUAL`, `AUTO`, `FAULT`),
  the engine's `execution_status`, and the run lifecycle (open or closed, with
  a `close_reason`). Mode gates whether the engine advances; it does not start
  or stop the engine. Success versus abort belongs to the run, not the engine.
- `execution_status` is `idle`, `running`, `blocked`, `failed`, `completed`
  or `unknown`. `paused` and `cancelled` were rejected: a behaviour tree cannot
  pause without changing the global mode, operator suspension is already
  `MANUAL` or `IDLE`, and abort is a close reason.
- The field is named `execution_status` because the response already has a
  required `status` object and does not allow additional properties.
- `execution_reason` is an optional enumerated qualifier, so a UI never parses
  diagnostic text.
- `engine_diagnostics` holds engine internals such as tick counters. It is
  declared unstable, and clients must not depend on its keys. Anything an
  operator needs is promoted to a neutral field instead.
- `completed` is emitted only when an engine declares a terminal completed
  state. The behaviour-tree engine does so only under
  `automation.on_terminal: stop`.
- Terminal behaviour (D9, #596) is `automation.on_terminal`. Under `repeat`, the
  default, a root that returned `SUCCESS` or `FAILURE` is ticked again from the
  root, as a control-loop tree needs. Under `stop` the tree stays loaded but
  idle: no more ticks and so no more calls, and `execution_status` reports
  `completed` or `failed` until AUTO is entered again or a definition is
  loaded. Either way the result is logged once per change, not once per tick.
  Repeat is the default so that no existing config changes behaviour; the
  original D9 decision, loaded-but-idle with auto-repeat as a later opt-in, is
  available as `stop`.

### Failure is not a fault

An unsuccessful outcome of the definition and an abnormal condition in the
engine are different things. An ordinary behaviour-tree `FAILURE` is
unsuccessful execution and shows only through `execution_status`; repeated for
more than ten ticks it reports `blocked`. A tick exception is an engine fault:
it emits `automation_fault` with a generic `locus`, never a tree node.

Rejected: renaming the old `bt_error` event to `automation_fault`. The old
event fired on both cases, so a rename would have published every `FAILURE`
as a fault.

### Definition identity

- A load reads the top-level file into memory, parses a candidate from the
  file path (so includes resolve), and stages its blackboard. It then hashes
  the in-memory bytes with SHA-256 and swaps the candidate in. A failed read,
  parse or staging step leaves the previous definition and version active.
- `automation_version` carries `engine_kind`, `id` (a human label, not
  identity), `digest` and `digest_scope`.
- `digest_scope` says what the hash covers, so the digest is not trusted more
  than it deserves. The behaviour-tree engine hashes only the top-level file
  (`top_level_file`), not included subtrees.

### Runs

- A run is an explicit operator primitive, opened with `POST /v0/runs` and
  closed with `POST /v0/runs/{run_id}/close`. It stays open across mode
  changes.
- Rejected: a run bound to AUTO. Telemetry is recorded in every mode and
  operator markers often happen in `MANUAL`, so an AUTO-bound run would orphan
  them and split one experiment across mode changes.
- A run is not required for AUTO, so existing deployments keep working.
  `run_id` on the automation status is null when no run is open.
- At most one run is open per runtime, so `run_id` on the status is singular
  and a second open is refused. Overlapping runs, if ever needed, would be a
  separate, explicit model.
- A run records the automation version (null for a manual-only run),
  `runtime_version`, an operator-supplied parameter snapshot and experiment
  label, the `tag_scope`, and the polling interval. Closed runs are immutable.

### Persistence

- Runs are stored as append-only JSONL under `runtime.data_dir`, with one
  writer that owns all writes: `runs/index.jsonl` for run records and
  `runs/<run_id>.events.jsonl` for each run's events. Every record carries a
  `schema_version`. Run open and close are written before the HTTP call
  returns. On restart, a torn or malformed line is skipped and a run left open
  is closed as `abandoned`.
- Rejected: SQLite. It would be a new dependency, and it is more than a
  low-rate append-only log needs.
- Faults and mode changes from the tick thread reach the journal through a
  bounded queue, so the tick thread never does file I/O. Overflow is counted.

### Telemetry correlation

- Telemetry is joined to a run by time window, half-open `[start, end)`, over
  the series the run's `tag_scope` names.
- Rejected: `run_id` as a fifth tag. Cardinality would grow as series times
  runs, without bound. The telemetry schema pins the tag set to exactly
  `runtime_name`, `provider_id`, `device_id` and `signal_id`, so a stray tag
  fails validation.
- `tag_scope` is supplied by the operator. Capturing it from engine activity
  was rejected for now: it needs instrumentation in the tree nodes and would
  still miss signals the tree never reads.
- The boundary is fuzzy by about one polling interval plus provider latency
  and jitter, because telemetry is stamped when the runtime observes a change.
  The run records its polling interval so an exporter can allow for it.
- Telemetry is emitted only when a value or quality changes, so a signal that
  is stable for the whole run may have no point inside the window. The window
  also spans time spent in `MANUAL`; `tag_scope` narrows what it covers.

### Run events

- Each run has an append-only event stream. Its `category` is a closed neutral
  set: `run_opened`, `run_closed`, `mode_change`, `parameter_change`,
  `automation_fault` and `annotation`. Operator markers are `annotation` with
  an open `type` and `payload`.
- Rejected: process markers (inoculation, feed, sample) in the core enum.
  That would leak domain vocabulary into core as tree vocabulary had leaked
  into the API.
- The durable key is `run_id` plus a per-run `sequence`. Live SSE event ids
  are a process-local counter that restarts with the process, so they cannot
  key history.

### Removing the old fields

The behaviour-tree status fields stayed beside the neutral ones in v0.1.24 and
v0.1.25, and the `bt_error` frame beside `automation_fault` in v0.1.25. Both
were removed in v0.1.26, once Workbench, the only consumer, had migrated. `GET /v0/automation/tree` still returns tree XML; it
stays until a neutral definition endpoint exists.

## What shipped

- v0.1.24: the engine seam and atomic load, the neutral status fields, the
  run registry, and the pinned telemetry tag set.
- v0.1.25: the run event stream and the `automation_fault` event.
- v0.1.26: removal of the behaviour-tree status fields and the `bt_error`
  frame.

The current descriptions:

- The engine interface:
  [`core/automation/automation_engine.hpp`](https://github.com/anolishq/anolis/blob/main/core/automation/automation_engine.hpp).
- The status, run and event schemas:
  [`schemas/http/runtime-http.openapi.v0.yaml`](https://github.com/anolishq/anolis/blob/main/schemas/http/runtime-http.openapi.v0.yaml),
  with the route list in the
  [runtime HTTP baseline](../contracts/runtime-http-baseline.md).
- Automation modes and the runtime surface: [Automation](../automation.md)
  and [System surfaces](../system-surfaces.md).
- `runtime.data_dir`: [Configuration](../configuration.md).
- Run export and Grafana run annotations live in
  [anolis-telemetry-export](https://github.com/anolishq/anolis-telemetry-export)
  (v0.1.1), which reads a run from `/v0/runs` and joins telemetry by window.

## Deferred or not implemented

- Include-closure hashing. Nothing closes a run as `superseded` when the
  digest changes.
- Build identity in run provenance. `runtime_version` is the version string
  only; there is no git SHA or build id.
- Opening a run automatically on entering AUTO.
- Durability against power loss. The journal flushes to the OS but does not
  `fdatasync`.
- A warning when run logs grow large. Cleanup is the operator's.
- `POST /v0/runs` is not idempotent, and `tag_scope` is stored as supplied,
  without normalisation or existence checks.
- `GET /v0/automation/tree` reads the definition file from disk on each call
  rather than returning the loaded snapshot, so it can differ from what is
  running.
- A second engine, policy and admission, live reload, pause and cancel, and
  any campaign hierarchy above runs.
