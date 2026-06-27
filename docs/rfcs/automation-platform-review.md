# Automation Platform RFC Audit and Critique

Reviewed: 2026-06-27

Primary artifact: `working/automation-platform-rfc.md`

Associated artifacts reviewed:

- `working/automation-platform-rfc/*.md`
- `working/backlog-research/06-automation-engine-seam.md`
- `working/backlog-research/07-tag-experiments-runs-time-markers.md`
- Runtime code/docs/contracts in `anolis/`
- Consumer touchpoints in `anolis-workbench/`, `anolis-operator-ui/`, and `anolis-telemetry-export/`

## Executive Verdict

The RFC is directionally right and much better than the earlier proposal set. Its synthesis correctly rejects the most dangerous options: `anolis-protocol` as the home for automation status, AUTO-bound runs as the identity model, SQLite as the default persistence layer, `run_id` as an InfluxDB tag, and a capability-heavy multi-engine registry before a second engine exists.

I would not send it directly to implementation yet. It needs one revision pass to harden the seam shape against the current `BTRuntime` threading model, make the run registry operationally concrete, turn the telemetry-cardinality rule into a machine guard, and define how much BT detail is allowed to remain on the wire via `engine_details`.

The biggest risk is not conceptual. The biggest risk is that implementation teams read the RFC as permission to build "generic automation platform" surface area while the actual runtime still has one private-threaded BT engine, no persistent data directory, no run store, no schema-generated types, and two frontends with different contract-sync paths. Keep the first implementation boring.

## What The RFC Gets Right

1. **Internal C++ seam, OpenAPI wire contract, no `anolis-protocol` change.**
   This is the correct layer split. `anolis-protocol` is ADPP provider protobuf below the runtime. Automation mode/status/run identity is above-runtime HTTP surface. A proto module would create a second distribution channel for data already carried by `/v0`.

2. **`execution_status`, not `status`.**
   `AutomationStatusResponse` already has required `status: Status{code,message}` and `additionalProperties:false`. The RFC fixed the collision caught in the critiques.

3. **Three-axis model is the right mental model.**
   Keep global mode (`IDLE/MANUAL/AUTO/FAULT`), per-engine execution status, and run lifecycle separate. This matches the current `ModeManager` and prevents engine health from becoming a backdoor mode model.

4. **Explicit runs decoupled from AUTO are essential.**
   The final RFC correctly rejects contiguous-AUTO intervals as the run boundary. Bioreactor runs and operator markers can span MANUAL interventions; telemetry is not AUTO-gated.

5. **Telemetry correlation by time window, not `run_id` tag.**
   Correct. `anolis_signal` series identity must remain bounded by hardware topology: `runtime_name`, `provider_id`, `device_id`, `signal_id`.

6. **JSONL is the right default persistence choice.**
   The runtime has no SQLite dependency today. For low-rate immutable run/event audit records, append-only JSONL plus an in-memory index is the right first store.

7. **Runtime version belongs in run provenance.**
   The final RFC correctly adds this. Behavior is not only XML bytes; it also depends on compiled BT node implementations and runtime version.

8. **Operator UI sync correction is included.**
   The final RFC correctly notes that workbench has a lock-file release-artifact path, while operator-ui is manually maintained (`js/contracts.js` plus fixtures).

## Required Revisions Before Implementation

### 1. Define the Phase 0 seam around the current private BT tick thread

`BTRuntime` currently owns its own thread (`tick_loop`) and gates ticking on `ModeManager::AUTO`. The RFC's seam sketch has `start()` but drops the existing `tick_rate_hz` argument, and earlier proposals mixed "engine owns thread" with "supervisor calls advance/poll".

Pick one for Phase 0:

- Keep the BT thread inside the engine.
- Preserve `start(int tick_rate_hz)` semantics, either as `start(AutomationStartOptions)` or by injecting tick config before start.
- Do not introduce supervisor-driven `advance()` in Phase 0.
- Demote `tick()->BT::NodeStatus` from public API only if tests get a replacement test hook.

Also note that C++20 has no `std::expected`. If the RFC says `Result<T>`, define a tiny local result type or keep the existing `bool + out + error` style. Do not sneak in a dependency or C++23 expectation.

### 2. Treat the tree mutex as a latent reload safety fix, not "zero behavior"

The current race is latent because `load_tree()` is init-only in production. Adding an active-definition mutex is still worthwhile, but it changes locking around the tick path. Phase 0 should call this a correctness hardening and test it under TSAN rather than claiming it is behavior-free.

Minimum implementation guidance:

- Guard `tree_`, `tree_loaded_`, `tree_path_`, and any future definition artifact together.
- Avoid holding the active-definition lock while doing slow file reads or XML parsing.
- Keep `health_mutex_` separate unless there is a reason to merge.
- Add a focused test for stop-load-start and, if live load remains private, do not expose live reload yet.

### 3. Make the digest scope honest in the wire contract

The RFC requires sha256 over the resolved BT include closure. That is the right provenance target, but it is not a small refactor. `BehaviorTreeFactory::createTreeFromFile()` hides include resolution behind the library. If Phase 1 cannot walk the include closure, do not publish `automation_version.digest` as if it fully identifies the executed program.

Safer staged contract:

- `automation_version.digest`: only if closure hashing is implemented.
- Or publish `automation_version.source_digest` with `digest_scope: "top_level_file"` as a known limitation.
- Always include `runtime_version`.
- Prefer adding `runtime_build` or `git_sha` to `anolis_build_config.hpp.in`; `ANOLIS_VERSION` alone is weak provenance when local builds or patched binaries exist.

Do not let "digest" become the new `current_tree`: a field that sounds authoritative but under-captures what actually ran.

### 4. Harden telemetry tag cardinality in CI

Current facts:

- `schemas/telemetry/telemetry-timeseries.schema.v1.json` requires the four known tags.
- But `tags.additionalProperties` is currently `true`.
- `validate-telemetry-timeseries.py` only validates fixtures with JSON Schema and does not assert the exact tag-key set.

The RFC's Phase 2 says to harden this. Make it a prerequisite before any run correlation work lands. Add a validator assertion that logical `anolis_signal` rows contain exactly:

```text
runtime_name, provider_id, device_id, signal_id
```

This preserves forward-compatible field metadata while preventing the tempting `run_id` tag shortcut.

### 5. Specify run storage as an actual runtime surface

JSONL is a good choice, but the RFC does not define where it lives or how it behaves under restart.

Before implementation, decide:

- Config key: likely `runtime.data_dir` or `runs.data_dir`.
- Default path behavior for dev vs systemd deployments.
- File layout, e.g. `runs/index.jsonl` plus `runs/{run_id}.events.jsonl`, or one append-only log.
- Atomic append behavior and corruption handling for partial last lines.
- Startup index rebuild behavior.
- Close-on-restart rule for any open run: `close_reason: "abandoned"`.
- Retention/rotation: even a simple max age/size policy or explicit "operator owns cleanup" statement.

Without this, `/v0/runs` becomes a design promise without operational semantics.

### 6. Avoid synchronous disk writes on the BT tick path

The BT loop runs at 10 Hz by default and can emit `BTErrorEvent` from the tick thread. If the run registry subscribes to engine events or writes `automation_fault` markers, do not do synchronous JSONL fsync work on the tick thread.

Use a bounded queue or write from the HTTP/runtime thread where possible. If the first implementation writes synchronously, explicitly document that only run open/close and operator markers are persisted in Phase 2, while engine fault persistence is Phase 3 with async buffering.

### 7. Budget the EventEmitter subscriber count

`Runtime::init_core_services()` currently creates `EventEmitter(100, 33)` with an explicit budget: 32 SSE clients plus 1 telemetry sink. If a run registry subscribes to the event bus to capture mode changes, parameter changes, or automation faults, the budget must change or the registry must use direct callbacks.

This is exactly the kind of small runtime invariant that will cause confusing failures if the RFC does not call it out.

### 8. Treat `engine_details` as hazardous

`engine_details` is useful as a migration pressure valve, but it is also where BT vocabulary can quietly re-enter the wire. If it is in OpenAPI as a free object, frontends will eventually depend on `ticks_since_progress` because it is available.

Recommendation:

- Name it `diagnostics` or `engine_diagnostics`, not `details`.
- Mark it explicitly unstable and non-contractual in docs.
- Keep it optional.
- Do not require UIs to render it.
- Avoid adding BT counters to examples unless the examples are clearly BT-specific.

If a field matters to the operator UX, promote it deliberately to a neutral field. If it does not, keep it out of stable fixtures.

### 9. Clarify `blocked` versus BT failure

Current BT `STALLED` means repeated `BT::NodeStatus::FAILURE` for more than 10 ticks. Calling that `blocked` can be misleading: "blocked" sounds like waiting for an external condition; repeated FAILURE can also mean a hard logic error.

Options:

- Map `BT_STALLED -> failed` when last error/failure is persistent.
- Map `BT_STALLED -> blocked` only if the BT adapter can distinguish "waiting" from "failing".
- Keep `blocked` but document that for BT v1 it means "not advancing", not necessarily benign waiting.

This is not a blocker, but it needs honest semantics in the RFC and OpenAPI description.

### 10. Define `completed` as future-capable but not guaranteed for BT

BT logs `SUCCESS`, but the loop keeps reticking. The final RFC notes `completed` will be rare. That should become a formal guarantee:

- `completed` means the engine declares a terminal completed state.
- The BT adapter may never emit it until terminal capture is implemented.
- A run can close with `close_reason: completed` only when the engine explicitly reports terminal completion, not merely because the operator leaves AUTO.

### 11. Add a run `tag_scope` model before claiming telemetry attribution

Time-window join is correct, but it means "signals during the run window", not "signals caused by the automation". The RFC acknowledges this but still leaves `tag_scope` vague.

For v1, prefer operator-supplied or config-derived `tag_scope`:

```json
{
  "runtime_name": "bioreactor-telemetry",
  "provider_ids": ["bread0", "ezo0"],
  "device_ids": ["rlht0", "ph0"],
  "signal_ids": []
}
```

Auto-capturing touched devices from BT activity is attractive, but it requires instrumentation in `ReadSignalNode` and `CallDeviceNode`, and it will miss signals that matter but are not read by the tree. Do not make auto-capture required for the first run registry.

### 12. Decide how deprecated SSE aliases are actually emitted

Today one `BTErrorEvent` serializes to one SSE frame named `bt_error`. A one-release alias for `automation_fault` is not just a schema change. It likely requires either:

- Emitting two SSE frames for one internal event, or
- Introducing a new event variant and preserving old `BTErrorEvent`, or
- Teaching the SSE serializer to output aliases for selected variants.

Pick the migration mechanism before promising an alias.

### 13. Bring `/v0/telemetry/status` into the platform picture

The reviewed branch already adds `GET /v0/telemetry/status`. That endpoint is adjacent to the run/telemetry story. The RFC should mention it in Phase 2/3 as a useful health dependency for any future "run telemetry is exportable" UI.

No design change is needed, but the docs/OpenAPI route inventory must remain synchronized when adding `/v0/runs`.

## Open Decision Recommendations

### Run-open ergonomics

Recommendation: make `POST /v0/runs` canonical. Allow auto-open-on-AUTO as a convenience, but do not require a run for AUTO in the first release.

Reason: mandatory runs would change current automation behavior; optional runs let current deployments continue while giving experiments a first-class path. If no run is open, `/v0/automation/status.run_id` is null and telemetry remains uncorrelated.

### Digest closure cost

Recommendation: do not call a digest authoritative until closure hashing is implemented. If closure hashing is too much for Phase 1, ship a clearly scoped top-level file digest as provisional and do not use it to enforce run supersession yet.

### Runtime version source

Recommendation: include both `ANOLIS_VERSION` and build identity. Add a build-time optional `ANOLIS_GIT_SHA` or `ANOLIS_BUILD_ID` field. For release builds it should be the release tag/SHA; for local builds it can be `unknown` but must be present.

### `tag_scope`

Recommendation: operator-supplied in v1, with optional runtime validation that referenced providers/devices exist. Auto-capture can come later after BT nodes emit read/write usage.

### JSONL retention

Recommendation: first release may document operator-owned cleanup, but the runtime should have a data directory and a startup warning when run logs exceed a configurable size. Do not leave the storage path implicit.

### Operator UI lock mechanism

Recommendation: do not block the RFC on bringing operator-ui under the workbench lock mechanism, but make the manual edits an explicit checklist item in every wire phase. A separate follow-up to add lock/sync to operator-ui is worthwhile.

### Boundary granularity

Recommendation: accept the ~2s ambiguity for the first historian use case. Document it in the run API and exporter docs. If point-exact attribution becomes required, use a non-indexed field or a separate annotation measurement, never a tag on `anolis_signal`.

## Phase-by-Phase Guidance

### Phase 0: Internal seam and BT hardening

Good first phase if tightly scoped.

Must include:

- Interface in `core/automation/`.
- Runtime/HTTP depending on interface where practical.
- `BT::NodeStatus` removed from non-test public surface or isolated behind a test-only hook.
- `start` still preserves `tick_rate_hz`.
- Active tree state guarded if any reload-shaped API appears.
- No HTTP/OpenAPI behavior change.

Avoid:

- Engine registry.
- Capabilities endpoint.
- Definition envelope.
- Pause/cancel.
- Run registry.

### Phase 1: Neutral status and automation version, additive

Good, but only after digest naming is honest.

Add optional fields first:

- `execution_status`
- `automation_version`
- `last_advance_ms`
- optional `run_id`
- optional `engine_diagnostics`

Keep deprecated mirrors:

- `bt_status`
- `last_tick_ms`
- `ticks_since_progress`
- `total_ticks`
- `current_tree`

Add tests:

- C++ enum/string mapping to OpenAPI enum.
- Example fixtures for both old and new fields.
- Consumer fixture updates for workbench and operator-ui.

### Phase 2: Run registry

Good, but make it a runtime subsystem, not just HTTP handlers.

Minimum viable shape:

- `POST /v0/runs`
- `POST /v0/runs/{id}/close`
- `GET /v0/runs`
- `GET /v0/runs/{id}`
- JSONL persistence and startup index rebuild.
- `abandoned` close on restart.
- `tag_scope`.
- open/close events.

Defer:

- Full marker vocabulary.
- Automatic touched-device inference.
- Exporter join implementation, unless a consumer is ready.

### Phase 3: Neutral event and marker stream

Good, but keep domain markers open.

Use a closed lifecycle enum plus open domain marker:

- lifecycle: `run_opened`, `run_closed`, `mode_change`, `parameter_change`, `automation_fault`, `annotation`
- domain marker: `{category, type, payload}`

Avoid hard-coding bioprocess terms like `inoculation`, `feed`, and `sample` into the core enum. Those are important application markers, but they should be data, not runtime vocabulary.

### Phase 4: Deprecation removal

Make this contingent on real consumer migration evidence:

- workbench updated and pinned.
- operator-ui updated manually or given lock sync.
- contract examples updated.
- old SSE alias tested during the compatibility window.

Do not remove `bt_status` or `/v0/automation/tree` just because a release passed. Remove them only after both frontends no longer read them.

## Additional Code/Docs Drift Found During Audit

These are not necessarily RFC blockers, but they matter for execution:

- `docs/architecture.md` still says Language: C++17 while the repo builds C++20.
- `docs/http-api.md` says "No auth model in v0", while current runtime has Bearer auth and non-loopback bind protection documented in `docs/security.md`.
- The telemetry baseline prose says the four tags are the locked identity, but the schema and validator do not enforce exact tag keys.
- `AutomationStatusResponse` currently has a required field list, so additive wire migration must update examples and validators carefully.
- `BTRuntime::get_tree_path()` returns a const reference to unguarded `tree_path_`; this is harmless today only because load is init-only.
- `BTErrorEvent.node` is always empty in current code; do not design future semantics around node-level precision until the BT adapter can provide it.

## Recommended Revised RFC Summary

If I were tightening the RFC before issue creation, I would rewrite the implementation thesis as:

> Extract a minimal internal automation-engine interface around the existing private-threaded BT runtime, preserving current behavior. Add a neutral additive HTTP status view and a truthful automation version record. Introduce explicit operator-opened runs persisted as append-only JSONL, with optional AUTO convenience, run windows correlated to telemetry by time and tag scope, and no change to `anolis_signal` tags. Use OpenAPI as the wire source of truth; update workbench via lock repin and operator-ui manually. Defer engine registry, capabilities, generic definition envelope, pause/cancel, live reload, and policy until a second engine, reload endpoint, or governance requirement actually lands.

That keeps the spirit of the RFC while making the first implementation smaller and harder to misuse.

## Bottom Line

Proceed with the RFC after a revision pass. The strategic architecture is sound. The implementation needs tighter guardrails around:

1. Current BT threading and tick-rate ownership.
2. Digest truthfulness.
3. JSONL storage semantics.
4. EventEmitter/subscriber and async write behavior.
5. Machine enforcement of the telemetry tag set.
6. Cross-repo frontend migration reality.

Handle those, and this becomes a strong platform step rather than an attractive abstraction pile.
