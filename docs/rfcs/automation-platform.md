# RFC: Anolis Automation Platform (#6 abstract engine seam + #7 run/experiment identity)

> **Status: DRAFT for owner review (rev 3).** Produced 2026-06-27 by a 4-phase
> research workflow (5 investigations → 4 design proposals → 4 adversarial
> critiques → synthesis), then hardened against **two** implementation reviews
> (rev 2 = `automation-RFC-audit-and-critique.md`; rev 3 folds in a second deeper
> review — failure≠fault, run cardinality, atomic load/truthful definition,
> operator-ui retired, structured BuildInfo, RunJournal durability). Grounded in the live
> codebase; claims are path-cited. **Tracked as epic anolishq/anolis#111**
> (children #112–#117 + anolis-telemetry-export#31). **All decisions D1–D9
> confirmed 2026-06-27.** Full research trail (5 investigations / 4 proposals /
> 4 critiques) + the two implementation reviews are retained in the project's
> working notes.
>
> This is a **plan**, not an implementation. The "Decisions" section records the
> settled calls; the **Pre-implementation semantic checklist** is the gate before
> Phase 1 coding.
>
> **Guiding constraint (from the audit): keep the first implementation boring.**
> The runtime today has *one* private-threaded BT engine, no persistent data dir,
> no run store, and no schema-generated types. The biggest risk is reading this RFC as license to build
> "generic automation platform" surface area ahead of any second engine, reload
> endpoint, or governance requirement. Build the seam; defer the platform.

## Summary

This RFC consolidates two coupled backlog items: (#6) abstract the automation engine behind a generic `IAutomationEngine` seam with a neutral status taxonomy so BehaviorTree.CPP (BT) vocabulary stops contaminating the HTTP API, schemas, UI, run records, and the future policy layer; and (#7) introduce first-class run/experiment identity that records the immutable automation version/digest each run executed and correlates telemetry to runs without exploding InfluxDB cardinality.

The plan: extract a thin internal C++ seam around today's *private-threaded* `BTRuntime`, **preserving current behavior** (the one genuine code change is a correctness fix — a mutex closing an existing lock-free race — verified under TSAN, not "behaviour-free"); then layer neutral *additive* wire fields and a *truthful* automation-version record; then introduce **explicit operator-opened runs** (with optional AUTO convenience) persisted as append-only JSONL, correlated to telemetry by time window + tag scope with **no change to `anolis_signal` tags**; then a neutral event/marker stream. Each phase is independently shippable. OpenAPI is the wire source of truth; the seam stays internal to `core/automation/` and `anolis-protocol` is untouched. Engine registry, capabilities, a generic definition envelope, pause/cancel, live reload, and policy are **deferred until a second engine, a reload endpoint, or a governance requirement actually lands.**

## Background — what exists today (verified)

- `core/automation/` is BehaviorTree.CPP-shaped end to end. `BTRuntime` (bt_runtime.hpp) owns `BT::BehaviorTreeFactory` + `BT::Tree`, exposes a `BTStatus {BT_IDLE,BT_RUNNING,BT_STALLED,BT_ERROR}` enum and an `AutomationHealth` struct, and leaks `BT::NodeStatus` through `tick()` in its public signature. There is no `IAutomationEngine` seam.
- The tree loads **once** at startup: `Runtime::init_automation` (runtime.cpp:380) calls `BTRuntime::load_tree(config_.automation.behavior_tree)`. `tree_`/`tree_loaded_`/`tree_path_` are read by the tick thread with **no mutex** — calling `load_tree` live is a data race.
- `ModeManager` is a separate global state machine `{IDLE,MANUAL,AUTO,FAULT}` with vetoing before-callbacks (FAULT non-vetoable). It gates ticking (the tick loop skips `tickOnce()` when mode != AUTO) but never starts/stops the engine — it is orthogonal to engine lifecycle.
- BT vocabulary leaks onto four wire surfaces: `GET /v0/automation/tree` returns raw BT XML; `GET /v0/automation/status` emits `bt_status`, `ticks_since_progress`, `total_ticks`, and a `current_tree` that is a **filename basename, not a content hash** (system_handlers.cpp:414-423); an SSE `bt_error` event (`node` always empty); and the `AutomationConfig` `behavior_tree`/`tick_rate_hz` config keys.
- `AutomationStatusResponse` (runtime-http.openapi.v0.yaml, verified) already has a **required `status` property of type `Status {code,message}`** alongside `bt_status` etc., under `additionalProperties:false`.
- **One consumer now (review 2 correction):** `anolis-workbench` is the **sole** UI consuming the taxonomy over the wire — it pins the schema by sha256 in `contracts/upstream/anolis/runtime-http.lock.json` (verified) and DOMParser-parses the BT XML in `Operate.svelte`. `anolis-operator-ui` was **decommissioned and archived** in the operator-ui sunset (#2; repo is private/read-only), so it is **not** a migration target. The earlier "two UIs / two sync paths" framing is obsolete.
- Telemetry identity is a **locked v1 schema**: measurement `anolis_signal` + exactly 4 low-cardinality tags (`runtime_name,provider_id,device_id,signal_id`), gated by `validate-telemetry-timeseries.py`. There is **no `run_id`/experiment concept anywhere** in core, and **no relational store** — InfluxDB is the only historian.
- `anolis-protocol` is ADPP provider protobuf only (`proto/anolis/deviceprovider/v1/*`) — the runtime↔provider boundary **below** the runtime.

## Goals

1. A neutral `IAutomationEngine` seam in core, with `BTRuntime` as the first implementation, that bakes in **no** BT assumptions (no tree/node/tick-XML vocabulary, no `BT::NodeStatus` in the public API).
2. A neutral, wire-published execution-status taxonomy that survives a future state-machine/recipe/schedule/manual-procedure engine.
3. First-class run identity recording the **immutable content digest** of the automation each run executed.
4. Telemetry correlated to runs **without** touching the locked 4-tag v1 schema or growing series cardinality.
5. Named hooks for #4 (policy/governance) and #5 (dynamic reload) without building them.

## Non-goals

- A second engine implementation, an engine registry, or capability declaration.
- Pause/cancel as first-class engine ops, a multi-definition catalog, or live hot-swap reload.
- A relational store; any campaign/experiment hierarchy owned by the runtime; a policy engine.

## Seam home — decision

**The `IAutomationEngine` interface and the neutral C++ enum live in `core/automation/` (internal). The JSON taxonomy + run DTOs are declared once in `anolis/schemas/http/runtime-http.openapi.v0.yaml`. `anolis-protocol` is not touched.**

Two artifacts, two homes — do not conflate them:

- The engine abstraction has zero cross-repo consumers (nothing links anolis C++). An interface with one in-process caller has no reason to become a cross-repo contract. This matches the Step Functions precedent: `DescribeExecution` returns neutral status + identifiers; the ASL program grammar is never exposed.
- `anolis-protocol` is the wrong home: its charter is ADPP provider protobuf below the runtime↔provider boundary. Automation status is an above-the-runtime concern; putting it there is a layer/category error and opens a second distribution channel for data the HTTP contract already carries.

The only cross-repo tendril is **Workbench** consuming the status taxonomy over `/v0`. That is already an HTTP/OpenAPI concern. The single C++↔OpenAPI mapping point — the hand-written switch at `system_handlers.cpp:390-405`, with no generator — is the one place the two representations can drift and is pinned by a **conformance test**.

**Distribution (simplified by the operator-ui sunset):** there is now **one** sync path — Workbench re-pins its lock file (`contracts/upstream/anolis/runtime-http.lock.json`) when the taxonomy changes. operator-ui is archived (#2) and not in scope. (The telemetry schema has a *separate* mirror/lock in `anolis-telemetry-export` — see issue #113.)

## The seam (internal C++)

```cpp
// core/automation/automation_engine.hpp  (INTERNAL; no other repo links anolis C++)
namespace anolis::automation {

enum class AutomationStatus { Idle, Running, Blocked, Failed, Completed, Unknown };

struct AutomationVersion {
  std::string engine_kind;   // "behavior_tree" today
  std::string id;            // human label (e.g. tree basename) — NOT identity
  std::string digest;        // see "Version digest" — may be source-only + scope in v1
  std::string digest_scope;  // "include_closure" | "top_level_file" (honesty marker)
};

struct AutomationStatusView {
  AutomationStatus status = AutomationStatus::Idle;
  AutomationVersion version;
  uint64_t last_advance_ms = 0;        // generic liveness (was last_tick_ms)
  std::optional<std::string> last_error;
  nlohmann::json engine_diagnostics;   // UNSTABLE/non-contractual: {ticks_since_progress, total_ticks, stall_suspected}
};

// Tiny local result — C++20 has NO std::expected; do not pull in a dep or assume C++23.
// (Mirrors the existing bool+error idiom, e.g. ModeManager::set_mode(mode, error&).)
struct LoadOutcome { bool ok = false; std::string error; AutomationVersion version; };

class IAutomationEngine {
 public:
  virtual ~IAutomationEngine() = default;
  virtual std::string_view engine_kind() const = 0;
  // Validate+load, compute digest, swap the active definition under the engine's
  // own lock. The bool+error IS the #4 admission hook; load() is the #5 swap point.
  virtual LoadOutcome load(const AutomationDefinitionRef&) = 0;
  virtual bool start(std::string& error) = 0;  // BT: spawn tick thread. Tick rate is
                                               // ENGINE-CONSTRUCTION config (BtAutomationEngine
                                               // ctor takes tick_rate_hz) — preserved from today's
                                               // start(int tick_rate_hz=10); NOT on the neutral API.
  virtual void stop() = 0;                     // BT: join + halt (graceful)
  virtual bool is_running() const = 0;
  virtual AutomationStatusView status() const = 0;                  // MUST NOT return BT::NodeStatus
  virtual std::optional<DefinitionArtifact> definition() const = 0; // opaque {media_type,digest,bytes}
  virtual void set_event_sink(std::shared_ptr<events::EventEmitter>) = 0;
};
} // namespace
```

`BTRuntime::tick()->BT::NodeStatus` is demoted off the public API — to private, or behind a **test-only hook** so `bt_runtime_test`/`bt_nodes_test` keep their tick-level assertions (do not strand the tests). The BT tick **thread stays inside the engine**; Phase 0 introduces **no** supervisor-driven `advance()`. `BTStatus`, tick counters, and the stall heuristic become engine-private and map to the neutral views. **Deliberately absent in v1 (YAGNI):** `validate()`/`admit()` as interface methods (the `LoadOutcome` bool+error already is the admission hook), `pause()`/`cancel()`, `capabilities()`, an `EngineRegistry`, and a catalog.

## Status taxonomy — three orthogonal axes

| Axis | Where | Values | Notes |
| --- | --- | --- | --- |
| Global mode | ModeManager / wire `RuntimeMode` | IDLE, MANUAL, AUTO, FAULT | unchanged; gates ticking, not engine lifecycle |
| Execution status | `/v0/automation/status` `execution_status` | idle, running, blocked, failed, completed, unknown | per-engine health; cross-industry intersection |
| Run lifecycle | run record | open, closed + `close_reason` | provenance; success-vs-abort lives here |

BT adapter mapping: BT_IDLE→idle; BT_RUNNING→running; BT_STALLED→blocked; `unknown` is a safety floor. Honesty caveats the OpenAPI `description` must carry:

- **`failed` ≠ fault (the central correction from review 2 — verified).** Today `BTErrorEvent` is emitted on **both** an ordinary root `BT::NodeStatus::FAILURE` ("BT returned FAILURE", `bt_runtime.cpp:166`) **and** a caught tick `std::exception` (empty node, `bt_runtime.cpp:192`) — but these are different things and the neutral contract must split them:
  - **`execution_status = failed`** — the automation *definition* reached an **unsuccessful terminal outcome** (an engine-defined execution result).
  - **`automation_fault`** (the event, not a status) — the **engine itself** hit an abnormal condition: tick exception, corrupt/unparseable definition, invariant violation. This is **not** an `execution_status` value.
  So Phase 3 must **not** simply rename `bt_error`→`automation_fault` — that would publish FAILURE-as-fault, which is wrong. The BT adapter must classify the two emission sites.
- **`blocked` is not "benign waiting".** Today BT_STALLED = `FAILURE` repeated >10 ticks, which can equally mean a hard logic error; and the "progress" heuristic is weak (every `RUNNING` tick resets the counter, even if parked on a wait node for hours). For BT v1, `blocked` means **"not advancing"**, documented as such, with `engine_diagnostics.stall_suspected` carrying the raw heuristic. A later BT refinement may map persistent failure → `failed`.
- **`completed` is a guarantee, not a heuristic.** Emitted **only** when the engine declares a *terminal completed* state. BT re-ticks after `SUCCESS` and has no terminal capture, so the adapter may **never** emit it until that lands — and a run closes `completed` **only** on explicit engine terminal completion, never merely because the operator left AUTO. **Open decision (D9):** what a terminal BT result (SUCCESS/FAILURE) *does* — stop the instance, leave the engine loaded-but-idle, or auto-repeat — is an **execution-policy** choice, not a serialization detail; settle before Phase 1.

**v1 excludes `paused` and `cancelled`:** BT can't honor pause without flipping the global AUTO gate; operator suspension is ModeManager MANUAL/IDLE; cancel/success-vs-abort lives in the run `close_reason`. Enums add values additively later if consumers map unrecognized values to `unknown` (documented).

**Optional `execution_reason` (coarse, enumerated — not free text).** So operators/UIs never parse diagnostic strings, pair the coarse `execution_status` with an optional reason: `{mode_gate, waiting, terminal_failure, engine_fault, no_definition, stopped}`.

**Field naming — avoids a verified collision + fixes misleading legacy names.** `AutomationStatusResponse` already has a required `status` (`Status{code,message}`), so the neutral field is **`execution_status`**, not `status`. The legacy `enabled` (= "mode is AUTO") and `active` (= "BT thread running", though that thread runs outside AUTO and merely skips ticks) are **misleading and must not survive unchanged** into the neutral contract — express them via `execution_status` + the global mode field. New additive fields: `execution_status`, `execution_reason?`, `automation_version{engine_kind,id,digest,digest_scope}` (nullable — a manual-only run may have no automation), `last_evaluation_at_epoch_ms` (renamed from the ambiguous `last_advance_ms`; it is *last engine evaluation*, **not** last observable progress — the engine has no progress token today), `engine_diagnostics`, optional `run_id`. `bt_status`/`ticks_since_progress`/`total_ticks`/`current_tree` are retained as deprecated mirrors for one release; **if the new runtime always emits the neutral fields, they are `required` in the new canonical contract** ("additive" = old clients ignore extras, *not* that the new server fields are optional).

**`engine_diagnostics` is the hazard field** (the one place BT vocabulary can re-leak onto the wire). Rules: marked **explicitly unstable / non-contractual** in the OpenAPI `description`; **optional**; UIs are **not** required to render it and must **not** depend on its keys (e.g. `ticks_since_progress`); BT counters do **not** appear in non-BT example fixtures. If a diagnostic genuinely matters to operator UX, it gets *promoted deliberately* to a neutral field; otherwise it stays out of stable fixtures. This is review discipline, not just a schema annotation.

## Run / event model

Hierarchy: experiment/campaign (operator label; runtime stays ignorant of campaign structure) → run(`run_id`) → `automation_version?` + `runtime_version` → params → events/markers → artifacts. MLflow-shaped, no scheduler. **`run_id` is a UUIDv7 or ULID** (time-sortable, collision-safe; format frozen in the contract). `automation_version` is **nullable** — a manual-only run (no automation loaded) legitimately has none; do not fabricate one.

**Run cardinality — at most ONE open run per runtime instance (v1) [D8].** This is the new decision the review surfaced and it must be explicit, because it determines whether `/v0/automation/status.run_id` can be singular (yes), how open-conflicts are handled (a second open is rejected while one is open), event ownership, and telemetry attribution. `tag_scope` still selects which signals the single open run considers its own. Multi-run overlap, if ever needed, becomes an **explicit** many-to-many model later — not something that emerges accidentally from loose API behaviour.

**Run boundary — decoupled from AUTO mode (central correction).** A run is an explicit operator/experiment primitive, not a contiguous-AUTO interval. Telemetry is not AUTO-gated and #7's markers (inoculation, feed, sample, intervention) routinely occur in MANUAL; an AUTO-bound run orphans that telemetry and fragments one experiment across AUTO/MANUAL toggles.

- **Open:** `POST /v0/runs` (idempotent) pins the loaded digest + runtime_version + the operator-supplied params snapshot + experiment label + normalized `tag_scope` (the #4 admission attach point). Rejected if a run is already open (D8). **Explicit operator-created runs ship first**; auto-open-on-AUTO is a *separate, config-gated* convenience added **after** the explicit lifecycle is proven — not intertwined with the core registry.
- **Stays open** across leave-AUTO, pause, and transient FAULT→recovery (recorded as events; windows stay contiguous).
- **Close:** explicit `POST /v0/runs/{id}/close` (idempotent; `close_reason` from a defined enum: `operator_stop|completed|failed|superseded|abandoned`), an engine terminal state, or a digest-changing reload (superseded). On restart, a still-open run closes `abandoned` with a recovery marker.
- run is 1:1 with automation_version: once the digest is authoritative (D2), a reload that changes it closes (superseded) and opens a successor.

**Atomic load + truthful definition snapshot (Phase 0 — review 2).** Today `load_tree()` assigns `tree_path_`, parses, mutates `tree_`, *then* sets `tree_loaded_` — a failed parse leaves inconsistent state — and `/v0/automation/tree` **re-reads the file from disk each call**, so the displayed XML can differ from what is actually running. The engine must instead: **(1)** read the complete definition into immutable bytes; **(2)** digest *those bytes*; **(3)** parse/instantiate a *candidate*; **(4)** build its `AutomationVersion`; **(5)** atomically swap active definition + version **only after all prior steps succeed** (a failed load preserves the previous valid definition+version). The engine retains the loaded snapshot (or enough to return it truthfully); the digest is **never recomputed from a path later**. `digest_scope:"top_level_file"` is acceptable provisionally but must still hash the *exact top-level bytes* that produced the active definition.

**Version digest (#7 ↔ #5) — staged for honesty.** The *target* is sha256 over the **resolved include closure** (main XML + all `createTreeFromFile` subtree includes), because hashing a single path re-creates the "filename, not identity" problem. But that is real, unscoped work — `BehaviorTreeFactory::createTreeFromFile()` hides include resolution, so the loader must walk includes itself. So **do not let `digest` become the new `current_tree`** (an authoritative-sounding field that under-captures what ran):

- If closure hashing is implemented in Phase 1 → `digest` with `digest_scope: "include_closure"`.
- If not → publish `digest` with `digest_scope: "top_level_file"` as an explicit, provisional known-limitation, and **do not use it to enforce run supersession** (the digest-change-closes-run rule waits until the digest is authoritative).
- **Always** record build provenance — identical XML against a different compiled `bt_nodes` registry behaves differently, so source digest alone is incomplete. Use a **structured `BuildInfo` object** (not an opaque concatenated string): `{version, git_sha, build_id, dirty?}`, **injected at build time** into `anolis_build_config.hpp.in` (no runtime git lookup; release → tag/SHA, local → `"unknown"`/`dirty:true` but always present). The **same `BuildInfo`** is reused in `/v0/runtime/status`, automation status, run provenance, and exported run manifests, so build-identification conventions don't drift across subsystems. (`ANOLIS_VERSION` alone is weak provenance against local/patched builds.)

The digest is form-agnostic, so a future state-machine/recipe engine supplies its own with no schema break.

**Event/marker stream:** one append-only stream keyed by run_id. To avoid baking bioprocess vocabulary into core (a leak as bad as the BT one), type is split: a **closed neutral lifecycle enum** `{run_opened, run_closed, mode_change, parameter_change, setpoint_change, pause, resume, automation_fault, annotation}` + an **open domain marker** `{category, type:string, payload}` for #7 operator markers.

- **`automation_fault` is engine-abnormal only** (see the failure-vs-fault split above) — a `locus` (not BT `node`), the exception/corrupt-definition/invariant cases. An ordinary BT `FAILURE` is **not** a fault; it surfaces as `execution_status=failed`. The BT adapter classifies its two current `BTErrorEvent` emission sites accordingly.
- **Durable run-event identity ≠ the live EventEmitter IDs.** `EventEmitter` assigns *process-local* monotonic ids that reset on restart (for live gap detection), unsuitable as durable history keys. A persisted run event carries: `run_id`, a **per-run monotonic `sequence`**, `occurred_at_epoch_ms`, `recorded_at_epoch_ms`, `category/type`, `payload`, and an event `schema_version`. (`run_id + sequence` is the durable key; a UUID is optional.)
- **One canonical internal `AutomationFaultEvent`, persisted once.** During the compat window the SSE serializer *renders* the legacy `bt_error` frame, but persistence records the canonical event a single time — do **not** emit two internal events (that would double-write journal/telemetry/downstream). The frontend's client-side `bt_status:"ERROR"` fabrication (Operate.svelte:573) is removed.

**Persistence — an actual runtime subsystem, not just HTTP handlers.** Append-only JSONL + an in-memory index for `GET /v0/runs`. SQLite is rejected (net-new dependency, verified absent; over-built for a low-rate append-only audit log on a store-less runtime). Operational semantics to pin **before** Phase 2 implementation (so `/v0/runs` is not a design promise without behaviour):

- **Data dir:** a config key (`runtime.data_dir`, runs under `…/runs/`) with a sane default for dev vs systemd deployments. The runtime has **no** data dir today — this is a new operational surface.
- **Layout:** `runs/index.jsonl` (one line per run open/close/state) + `runs/{run_id}.events.jsonl` (per-run event stream).
- **Durability contract** (more than "atomic append"): a **single `RunJournal` writer/owner** per runtime is the *only* persistence path (no ad-hoc writes from HTTP markers, mode/parameter callbacks, or engine faults); concurrent HTTP requests synchronize on it; `O_APPEND`; a defined `flush`/`fdatasync` policy; a `schema_version` on **every** durable record; a max record size; malformed-line handling beyond the final torn line; explicit **disk-full / permission-error health counters**; idempotent close; defined `run_id` format (UUIDv7/ULID); list **pagination** on `GET /v0/runs`.
- **Write discipline:** **run open/close are synchronously durable-flushed *before* the HTTP call reports success** (scientific provenance must not lie about a persisted run); high-frequency events go through a **bounded async queue** with overflow counters. **Never** fsync on the BT tick thread (10 Hz, can emit faults) — all persistence is the `RunJournal`'s single path.
- **Restart rule:** any still-open run is closed `abandoned` with a recovery marker; the in-memory index is rebuilt from the logs at startup.
- **Retention/rotation:** v1 may state **operator-owned cleanup**, but the runtime emits a **startup warning** when run logs exceed a configurable size. The storage path is never implicit.

Closed runs are immutable.

**EventEmitter budget invariant.** `Runtime::init_core_services` creates `EventEmitter(100, 33)` — an explicit budget of 32 SSE clients + 1 telemetry sink (verified). If the run registry subscribes to the bus to capture mode/parameter changes or faults, it consumes a slot → either **raise the budget** (and update the comment) or have the registry use **direct callbacks** (preferred for v1; avoids spending an SSE slot on an internal consumer). Flagged because it will cause confusing "max subscribers" failures otherwise.

**Telemetry correlation — no cardinality blowup:** `run_id` is never a 5th tag (cardinality would be series × runs, unbounded). The locked v1 4-tag schema stays frozen. Correlation is a **time-window join**: the run registry maps run_id → {start_ts, end_ts, tag_scope}.

**`tag_scope` is operator-supplied in v1** (auto-capture deferred). It scopes which series a run "owns", e.g.:

```json
{ "provider_ids": ["bread0", "ezo0"], "device_ids": ["rlht0", "ph0"], "signal_ids": [] }
```

Optional runtime validation may check the referenced providers/devices exist. **Auto-capturing touched devices from engine activity is deferred** — it needs instrumentation in `ReadSignalNode`/`CallDeviceNode` and still misses signals that matter but the tree never reads; it must not be a v1 prerequisite.

Honest caveats (documented in the run API + exporter docs):

- The window is wall-clock and includes MANUAL/pause gaps, so "in window" ≠ "caused by the run" (`tag_scope` narrows it).
- **Boundary ambiguity is not a "~2s" constant** (review 2 correction). Telemetry events are timestamped when the **runtime observes** the change (not the provider's value timestamp) and are emitted **only on value/quality change**. So the real ambiguity is **≈ polling interval + provider latency + scheduler/write jitter** — express it that way and **record the configured polling interval in run provenance** so an exporter can reason about it.
- Use **half-open `[start, end)`** intervals so adjacent runs don't double-attribute a point at an exact boundary.
- A signal that is **stable across the whole run** may yield **no point** in `[start, end)` (change-only emission). A scientifically useful export therefore needs either a **seed = last known value immediately before `start`**, or an explicit "exports are change events only" declaration. (This is exporter behaviour — see the cross-repo split below.)

If point-exact attribution is ever truly required it rides as a non-indexed **field** (`fields.additionalProperties:true` permits it) or a separate annotation measurement — **never** a tag on `anolis_signal`. Markers render as Grafana annotation regions (a cross-repo exporter deliverable, not part of the run registry).

**Verified prerequisite (machine-enforce the rule):** `schemas/telemetry/telemetry-timeseries.schema.v1.json` *requires* the 4 tags but sets `tags.additionalProperties: true`, and `validate-telemetry-timeseries.py` does not assert an exact key-set — so a `run_id` tag would pass CI today. **Before any run-correlation work lands**, harden the validator to assert the logical tag-key set is *exactly* `{runtime_name, provider_id, device_id, signal_id}` (preserving forward-compatible *fields*). This converts the prose cardinality guard into a guard rail.

## Hooks for #4 (policy) and #5 (reload)

- **#4:** `load()`/`start()` already return `LoadOutcome`/`bool+error`, mirroring `ModeManager::BeforeModeChangeCallback` (bool+reason, FAULT non-vetoable) — **that bool+error IS the admission hook**. Run-open is the attach point; the run record (run_id + digest + runtime_version + params + label) is the governed subject. v1 always admits. No authz vocabulary enters the neutral enum; ModeManager's veto stays the mode-axis admission, the run hook is the run/definition axis — same shape, not merged. A separate `IRunAdmission` interface is **not** introduced now (a full interface for a default-allow impl is a hook-for-a-hook).
- **#5:** `load()` is the single synchronized active-definition swap point. Phase 0 adds an **engine-owned mutex** guarding `tree_`/`tree_loaded_`/`tree_path_` (and any future definition artifact) **together** — taken **only** around the swap, **never** while doing slow file reads / XML parsing; `health_mutex_` stays separate. This is a **correctness hardening** of an existing latent lock-free race (today benign only because `load_tree` is init-only), **tested under TSAN** with a focused stop→load→start test — *not* framed as "behaviour-free". v1 reload = stop→load→start (no live hot-swap; live reload stays private until #5). A later live quiescence swap drops in without changing the seam or callers. A digest change drives close-superseded/open-successor **once the digest is authoritative** (see digest staging).

## Phasing

- **Phase 0 — internal seam + BT threading hardening. No HTTP/OpenAPI behaviour change.** Introduce `IAutomationEngine` in `core/automation/`; `BtAutomationEngine` wraps `BTRuntime`; route `Runtime::init_automation` + HTTP through the interface where practical. **Keep the BT thread inside the engine; preserve tick-rate semantics** (`start` parameterless on the interface, tick rate injected at engine construction); **no supervisor `advance()`**. Demote `tick()->BT::NodeStatus` off the public API (private or test-only hook — do **not** strand `bt_runtime_test`/`bt_nodes_test`). Add the active-definition mutex as a **TSAN-tested correctness fix** with a stop→load→start test. Do **not** add: engine registry, capabilities endpoint, definition envelope, pause/cancel, or the run registry.
- **Phase 1 — neutral status + automation version on the wire (additive) — only after digest naming is honest.** Add optional `execution_status`, `automation_version{…,digest_scope}`, `last_advance_ms`, `run_id`, `engine_diagnostics`; **keep** `bt_status`/`last_tick_ms`/`ticks_since_progress`/`total_ticks`/`current_tree` as deprecated mirrors. Tests: **C++↔OpenAPI enum-mapping conformance test** pinning the hand-written switch (`system_handlers.cpp:390-405`); example fixtures for old + new fields; **Workbench** fixture/type updates + lock repin (sole consumer; operator-ui retired). Remove the client-side `bt_status:"ERROR"` fabrication (`Operate.svelte:573`).
- **Phase 2 — run registry as a runtime subsystem (not just handlers).** `POST /v0/runs`, `POST /v0/runs/{id}/close`, `GET /v0/runs`, `GET /v0/runs/{id}`; **operator-supplied `tag_scope`**; JSONL persistence with the layout/durability/restart semantics above; `abandoned` close on restart; open/close events; auto-open-on-AUTO convenience (never auto-close on leave-AUTO). **Prerequisite:** the telemetry-validator tag-key hardening (above) lands first. Writes happen off the tick thread. *Defer:* full marker vocabulary, auto touched-device inference, exporter join (until a consumer is ready).
- **Phase 3 — neutral event/marker stream.** Rename SSE `bt_error`→`automation_fault` (generic `locus`) with `bt_error` kept as a deprecated alias one release — **pick the alias mechanism first** (a one-event→two-frames emit, a new variant alongside `BTErrorEvent`, or an alias-aware SSE serializer); engine faults persisted via the async bounded queue. Closed neutral lifecycle enum + **open** `{category, type, payload}` domain marker — **no** bioprocess terms (`inoculation`/`feed`/`sample`) baked into the core enum. `POST /v0/runs/{id}/events` for operator markers; fold mode/parameter changes in; Grafana annotations.
- **Phase 4 — deprecation removal, contingent on real consumer-migration evidence.** Drop deprecated BT fields and the `bt_error` alias **only** once **Workbench** no longer reads them (updated + lock-pinned, examples updated, alias exercised across **≥ one published release**, not merely "the next merge"). Do **not** remove `bt_status`/`/v0/automation/tree` just because a release passed; `/v0/automation/tree` stays until a neutral definition endpoint exists.
- **Deferred behind hooks:** #4 policy; #5 live reload; a second engine; EngineRegistry/Capabilities; pause/cancel; the engine-typed definition envelope (keep `/v0/automation/tree` BT-only until engine #2); a catalog; campaign hierarchy.

## Alternatives considered

- **anolis-protocol for the seam/taxonomy** — rejected (layer/category error; second distribution channel).
- **SQLite run store** — rejected as default (net-new dependency, over-built for an append-only log).
- **AUTO-bound run boundary** — rejected (orphans MANUAL telemetry/markers, fragments experiments).
- **run_id as a 5th tag** — rejected (unbounded cardinality).
- **Multi-engine registry + capabilities + IRunAdmission + definition envelope from day one** — rejected as YAGNI for a single BT on a 0.5 Hz loop.
- **`paused`/`cancelled` in the v1 enum** — rejected (BT can't honor pause without flipping global mode; covered by ModeManager / close_reason).
- **Frozen bioprocess marker enum in core** — rejected (domain leak); use neutral lifecycle enum + open marker.
- **Naming the field `status`** — rejected (collides with the existing required Status-typed property under additionalProperties:false).

## Known drift to fix alongside (found during audit — not blockers, but real)

- `anolis/docs/architecture.md` still says **Language: C++17**; the repo builds **C++20**.
- `anolis/docs/http-api.md` says **"No auth model in v0"**, but the runtime has Bearer auth + non-loopback bind protection (`docs/security.md`). Correct it when touching the automation endpoints.
- The telemetry baseline **prose** calls the 4 tags the locked identity, but the **schema/validator do not enforce** the exact key-set (the Phase-2 prerequisite above).
- `BTRuntime::get_tree_path()` returns a const-ref to **unguarded** `tree_path_` — harmless only because load is init-only today (the Phase-0 mutex covers this).
- `BTErrorEvent.node` is **always empty** in current code — do **not** design future semantics around BT-node-level precision until the adapter can actually provide it.
- **Route inventory must stay in sync:** the runtime already added `GET /v0/telemetry/status` (anolis#110). It's adjacent to the run/telemetry story (a health dependency for any "run telemetry is exportable" UI). No design change, but keep `docs/http-api.md` + the OpenAPI route list synchronized when `/v0/runs` lands.

## Decisions needed (owner) — each with a recommended default

| # | Decision | Recommended default | Why it matters |
| --- | --- | --- | --- |
| D1 | **Run-open ergonomics:** is a run mandatory for AUTO, or optional? | **`POST /v0/runs` canonical; auto-open-on-AUTO is pure convenience; a run is NOT required for AUTO in v1.** If none is open, `automation/status.run_id` is null and telemetry is simply uncorrelated. | Mandatory runs would change current automation behaviour; optional keeps existing deployments working while giving experiments a first-class path. |
| D2 | **Digest scope for BT:** ship the include-closure hash in Phase 1, or a provisional top-level hash? | **Ship `digest_scope:"top_level_file"` as provisional if closure-walking isn't ready; do NOT enforce run supersession on it until it's `include_closure`.** | Closure-walking is real work BT.CPP doesn't expose; the field must not over-claim what ran. |
| D3 | **`runtime_version` source:** `ANOLIS_VERSION` only, or + build identity? | **Both — add a build-time `ANOLIS_GIT_SHA`/`ANOLIS_BUILD_ID` (release→tag/SHA, local→`"unknown"` but present).** | Version string alone is weak provenance against local/patched builds. |
| D4 | **`tag_scope`:** operator-supplied or auto-captured? | **Operator-supplied in v1 (optional existence validation); auto-capture deferred** until BT nodes emit read/write usage. | Auto-capture needs node instrumentation and still misses unread-but-relevant signals; shouldn't gate v1. |
| D5 | **JSONL retention:** capped or operator-owned? | **Operator-owned cleanup in v1 + a startup warning over a configurable size; the data dir is explicit, never implicit.** | Avoids `/v0/runs` being a storage promise with no operational story. |
| D6 | ~~operator-ui contract sync~~ | **N/A — operator-ui was decommissioned + archived in #2.** Workbench is the sole consumer (one lock-repin sync path). | Removes a whole migration target the earlier drafts assumed. |
| D7 | **Boundary granularity:** accept the (reframed) ambiguity? | **Accept for the historian use case; document.** Ambiguity ≈ polling interval + provider latency + jitter (not a ~2s constant); record polling interval in provenance; half-open `[start,end)`. Point-exact, if ever needed → non-indexed field, never a tag. | Change-only, observe-time-stamped telemetry makes exact boundary attribution inherently fuzzy. |
| **D8** ✅ | **Run cardinality:** one open run per runtime, or multiple/overlapping? | **CONFIRMED: at most ONE open run per runtime in v1.** Singular `run_id`; second open rejected while one is open; multi-run becomes an explicit many-to-many model later if ever needed. | Determines `/automation/status.run_id` singularity, conflict handling, event ownership, attribution, reload behaviour. |
| **D9** ✅ | **Terminal BT behaviour:** what does a root SUCCESS/FAILURE do? | **CONFIRMED: engine stays loaded-but-idle on terminal** (no silent re-tick, no auto-repeat); auto-repeat is an explicit opt-in later. This defines when `completed`/`failed` are emitted and when a run may close on engine terminal. | Today BT re-ticks after SUCCESS; "completed" semantics + run auto-close depend on this. |

**All decisions D1–D9 confirmed by the owner (2026-06-27)** — including the failure-vs-fault split. D8 = one open run per runtime (v1); D9 = engine stays loaded-but-idle on a terminal result. The **Pre-implementation semantic checklist** below is the remaining gate before Phase 1 coding, and it is now fully specified.

## Pre-implementation semantic checklist (resolve before Phase 1 — review 2)

The phases share a few semantics that, left implicit, would get *renamed rather than corrected*. Pin these in the epic (#111) before coding:

1. **Fault vs unsuccessful execution** — `automation_fault` (engine abnormal) vs `execution_status=failed` (definition terminal-unsuccessful). [done — see taxonomy]
2. **Terminal execution behaviour** — D9.
3. **Run cardinality + scope** — D8; normative `tag_scope` shape (omitted ⇒ ? ; selector composition; wildcards?; validated at open?; frozen normalized scope in the record).
4. **Run IDs, close reasons, parameter provenance** — UUIDv7/ULID; close-reason enum; params as an open-time snapshot (vs later parameter-change events).
5. **Timestamp + interval semantics** — `last_evaluation_at_epoch_ms`; observe-time stamping; half-open `[start,end)`; polling interval in provenance.
6. **JSONL journal durability + schema versioning** — single `RunJournal` writer; sync open/close before HTTP success; async event queue; `schema_version` per record; disk-error counters.
7. **Loaded-definition snapshot + digest** — atomic staged load; digest from the exact loaded bytes; `/v0/automation/tree` returns the snapshot, not a disk reread.
8. **Split runtime event persistence from exporter/Grafana work** — the latter is a cross-repo deliverable in `anolis-telemetry-export` (its own issue).
