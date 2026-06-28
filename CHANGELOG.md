# Changelog

All notable changes to the `anolis` runtime are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

### Added

- Neutral run event/marker stream (Phase 3 of the automation-platform epic). Each
  run now has an append-only event stream persisted as `runs/{run_id}.events.jsonl`,
  keyed by `run_id` + a per-run monotonic `sequence`. A closed neutral lifecycle
  taxonomy (`run_opened`, `run_closed`, `mode_change`, `parameter_change`,
  `automation_fault`, `annotation`) carries runtime-owned events; operator markers
  ride as `annotation` with an open `{type, payload}` so no domain vocabulary is
  baked into core. New endpoints: `POST /v0/runs/{id}/events` (append an operator
  marker; rejected on a closed run) and `GET /v0/runs/{id}/events` (paginated,
  oldest-first, for exporters). Mode and parameter changes are folded into the
  stream. (anolishq/anolis#116)

### Changed

- The automation engine's abnormal-condition event is now the engine-neutral
  `automation_fault` (a generic `locus`, never a behaviour-tree node). The SSE
  stream renders both a neutral `automation_fault` frame and the **deprecated
  `bt_error`** alias frame for one release. An ordinary behaviour-tree `FAILURE`
  is no longer published as a fault — it is unsuccessful execution and surfaces
  only through `execution_status`; faults are reserved for tick exceptions and
  other engine-abnormal conditions. Faults are persisted to the open run's event
  stream off the tick thread via a bounded async queue (overflow-counted), never
  blocking or fsyncing on the 0.5 Hz tick thread. (anolishq/anolis#116)

## [0.1.24] - 2026-06-28

### Added

- Run / experiment registry (Phase 2 of the automation-platform epic): a `run` is
  an explicit operator primitive, decoupled from AUTO mode, with **at most one
  open run per runtime**. New endpoints: `POST /v0/runs` (open — pins the loaded
  automation version digest + build/timing provenance + operator-supplied
  `tag_scope`; rejected while a run is open), `POST /v0/runs/{id}/close`
  (idempotent), `GET /v0/runs` (paginated, newest-first), `GET /v0/runs/{id}`.
  Persisted as append-only JSONL under `runtime.data_dir` (new config key) via a
  single-writer journal that flushes open/close before the call returns, tolerates
  a torn final line, and closes any run left open by a prior process as
  `abandoned` on restart. `GET /v0/automation/status` now reports the open
  `run_id`. Telemetry is correlated to runs by time window — `run_id` is never a
  series tag. (anolishq/anolis#115)

- `GET /v0/automation/status` now also returns engine-neutral fields
  (Phase 1 of the automation-platform epic): `execution_status`
  (`idle|running|blocked|failed|completed|unknown`), optional `execution_reason`,
  `automation_version{engine_kind,id,digest,digest_scope}` (the immutable content
  digest of the loaded definition), `last_evaluation_at_epoch_ms`,
  `engine_diagnostics` (unstable/non-contractual), and `run_id` (null until the
  run registry lands). Additive: the behaviour-tree fields (`bt_status`,
  `ticks_since_progress`, `total_ticks`, `current_tree`, …) are retained as
  **deprecated mirrors** for one release. (anolishq/anolis#114)

### Changed

- Telemetry contract: pin the `anolis_signal` tag key-set to exactly
  `{runtime_name, provider_id, device_id, signal_id}` by setting
  `tags.additionalProperties: false` in `telemetry-timeseries.schema.v1.json`
  (fields stay forward-compatible). Machine-enforces the documented cardinality
  guard — a stray tag such as `run_id` is now a contract failure rather than a
  silently-accepted high-cardinality series. Conformant producers (the runtime
  sink only ever emits the 4 tags) are unaffected. Downstream:
  `anolis-telemetry-export` re-pins its vendored schema mirror on the next
  release that carries this change. (anolishq/anolis#113)

## [0.1.23] - 2026-06-16

### Changed

- Bump the vcpkg baseline to the vcpkg `2026.06.01` release: protobuf
  `5.29.5` → `6.33.4`, with abseil and the rest of the C++ dependency set
  refreshed. No source changes required.
- Centralize the vcpkg pin: the shared `setup-vcpkg` action now derives the
  vcpkg commit from `vcpkg-configuration.json`, so the per-workflow
  `VCPKG_COMMIT` env was removed.

### CI

- Migrate Windows build to Visual Studio 2026. The hosted `windows-2025` /
  `windows-latest` runner image moved from VS 2022 to VS 2026; update the
  `base-windows-msvc` preset generator `Visual Studio 17 2022` →
  `Visual Studio 18 2026` and move both Windows CI lanes from `windows-2022` to
  `windows-2025`. The plain `x64-windows` triplet inherits the image's default
  toolset (`v145`), so no triplet/toolset changes are required.
- Add CI OK aggregator gate: removed `paths-ignore`, added `dorny/paths-filter`
  to detect code-vs-docs changes, gated all jobs behind the filter, and added a
  final `ok` job as the sole required status check for `main` branch protection.

## [0.1.20] - 2026-04-25

### Fixed

- F7: `EventEmitter` was created with `max_subscribers = 32`, matching
  `HttpServer::MAX_SSE_CLIENTS`. When telemetry is enabled, `InfluxSink`
  subscribes to the same emitter during `init_telemetry`, consuming one slot
  and reducing effective SSE capacity to 31. The 32nd SSE client passed the
  HTTP-layer gate but received `nullptr` from `subscribe()`, resulting in a
  503 response. Fixed by raising the emitter cap to 33 (32 SSE + 1 internal
  subscriber) with an explicit budget comment.

## [0.1.19] - 2026-04-25

### Fixed

- F6: `SignalHandler::install()` now resets `shutdown_requested_` to `false`
  before registering OS signal handlers. Previously the static latch was never
  cleared, so any re-invocation of `install()` after a signal had fired (e.g.
  in-process re-initialization or unit tests) would leave a stale `true` and
  cause `Runtime::run()` to exit immediately on its first shutdown check.

## [0.1.18] - 2026-04-25

### Fixed

- F5: `HttpServer::handle_get_runtime_status` used a `static` local
  `start_time` that was initialised on the first call to the handler, not at
  server construction or start. `uptime_seconds` in `GET /v0/runtime/status`
  therefore reported time-since-first-request rather than time-since-start.
  Fixed by adding a `start_time_` member to `HttpServer`, set at the top of
  `HttpServer::start()` (when the socket binds), and using it in the handler.

## [0.1.17] - 2026-04-25

### Fixed

- F4: `ProviderSupervisor::record_crash` now clamps `attempt_index` to the last
  valid entry in `backoff_ms` when the vector is shorter than `max_attempts`,
  preventing an out-of-bounds vector access. `get_backoff_ms` is updated to use
  the same clamping logic for consistency (previously returned 0 on OOB).
  The YAML config path already rejects mismatched configs; this adds a second
  layer of defence for direct class construction.

## [0.1.16] - 2026-04-25

### Fixed

- F3: `EventEmitter::~EventEmitter()` now closes all subscriber queues before
  destruction. Surviving `Subscription` objects detect the closed queue and
  skip the raw-`this` unsubscribe callback in their destructors, preventing a
  use-after-free when a subscription outlives its emitter.
- `Subscription::unsubscribe()` guards the raw-`this` lambda call with
  `!queue_->is_closed()`, making the emitter-destroyed-first path safe without
  requiring shared ownership of the emitter.

## [0.1.15] - 2026-04-25

### Fixed

- F2: `base64_decode` in `core/http/json.cpp` now returns `std::nullopt` on
  illegal characters rather than silently returning a partial decoded string.
  `decode_value` propagates the error as HTTP 400 INVALID_ARGUMENT.
  Previously, any non-Base64 character (e.g. `#`, `!`, space) was treated as
  end-of-input and the truncated bytes were accepted as valid.
- Fixed signed integer overflow (UBSan) in `base64_encode` and `base64_decode`
  by changing the accumulator `val` from `int` to `unsigned int`.

## [0.1.14] - 2026-04-24

### Fixed

- F1: Argument range validation bugs in `CallRouter::validate_argument_range` and
  `encode_function_spec` (HTTP capabilities response).
  - Copy-paste error: `has_min` and `has_max` were identical `||` expressions,
    causing one-sided bounds to silently become two-sided (a spec declaring only
    `min=5.0` would phantom-enforce `max=0.0`).
  - Structural: zero-valued bounds were inexpressible under proto3 implicit presence.
  - Fix: `anolis-protocol` bumped to v1.2.0, which adds `optional` to the six
    `ArgSpec` bounds fields, generating `has_*()` presence methods used in both
    `call_router.cpp` and `json.cpp`.

## [0.1.13] - 2026-04-24

### CI

- Fixed `Windows Release (Core, strict)` build: suppressed MSVC C4702
  (unreachable code) from BT.CPP's `safe_any.hpp` `if constexpr` template
  instantiations. C4702 is emitted during the code-generation phase, so
  `/external:I` and `/external:anglebrackets` have no effect on it. The fix
  is `/wd4702` added to the MSVC branch of `anolis_apply_warnings()` in
  `cmake/Warnings.cmake`, covering all targets uniformly.

## [0.1.12] - 2026-04-24

### CI

- Fixed `validate-artifact` job: replaced `gh release download --latest` with
  `gh release view` to resolve the latest tag first, then pass it explicitly.
  The `--latest` flag is not available in the `gh` CLI version on the ubuntu-24.04
  runner image.

## [0.1.11] - 2026-04-23

### Dependencies

- Upgraded `behaviortree-cpp` from 4.6.2 to 4.8.4 (latest at vcpkg commit `66c0373`). The
  4.6.2 vcpkg portfile predated static library support and ignored `VCPKG_LIBRARY_LINKAGE`,
  always producing a shared `.so`. The 4.8.4 portfile correctly passes `BTCPP_SHARED_LIBS=OFF`
  when the triplet requests static linkage. Removed the `overrides` pin from `vcpkg.json`.
  No C++ API changes required — all APIs in use are unchanged across this version range.

## [0.1.10] - 2026-04-23

### CI

- Fixed `x64-linux-static` triplet: added missing `VCPKG_CMAKE_SYSTEM_NAME=Linux`.
  Without this line vcpkg on Linux falls back to Windows host detection and aborts configure
  with "Use of Visual Studio's Developer Prompt is unsupported on non-Windows hosts."

## [0.1.9] - 2026-04-23

### CI

- Fixed binary portability: added custom `triplets/x64-linux-static.cmake` vcpkg triplet
  (`VCPKG_LIBRARY_LINKAGE=static`, `VCPKG_CRT_LINKAGE=dynamic`) and applied it to the
  `ci-linux-release` configure preset via `VCPKG_OVERLAY_TRIPLETS`. All vcpkg dependencies
  (protobuf, yaml-cpp, openssl, behaviortree-cpp) are now statically linked into the released
  binary. glibc remains dynamic. The tarball contains a single self-contained executable.

## [0.1.8] - 2026-04-23

### CI

- Fixed `validate-artifact` job: switch release build to `x64-linux-static` vcpkg triplet so
  the published binary is fully statically linked against vcpkg dependencies (including
  `libbehaviortree_cpp`). The `x64-linux` triplet produces dynamic `.so` libraries that are
  not bundled in the tarball and are not available on a stock Ubuntu runner.

## [0.1.7] - 2026-04-23

### CI

- Added `validate-artifact` job to release workflow: downloads the published binary
  tarball, verifies SHA256, runs `--help` smoke, then starts the runtime with a
  provider-sim config and asserts `/v0/runtime/status`, `/v0/devices`, and
  `/v0/state` all respond correctly before graceful shutdown.

## [0.1.6] - 2026-04-23

### Changed

- `config/bioreactor/` and `behaviors/` removed from platform repo; all machine
  realizations now live in `anolis-projects`. Platform repo retains only
  `config/conformance/` (sim-based self-compliance fixture).
- `config/anolis-runtime*.yaml` moved to `examples/`.
- `behaviors/demo.xml`, `test_noop.xml` moved to `tests/integration/fixtures/behaviors/`.
- README and index documentation reorganised; README is now the human-facing landing page.

### CI

- Version-sync check wired: `version-locations.txt` added tracking `CMakeLists.txt`
  and `vcpkg.json`; CI calls reusable `version-sync` workflow from `anolishq/.github`.
- `vcpkg.json` version aligned to `v0.1.5` (was stale at `0.1.0`).
- `.anpkg` added to `.gitignore`.

## [0.1.5] - 2026-04-21

### Changed

- Bump `anolis-protocol` FetchContent reference from `v1.0.0` to `v1.1.3`.
- Consolidate OpenAPI schema README.

### CI

- Pin org reusable workflow refs from `@main` to `@v1`.
- Add metrics collection to release workflow; `metrics.json` uploaded as release asset on each `v*` tag.

## [0.1.4] - 2026-04-21

### Fixed

- Schema `$id` URIs corrected to `https://anolishq.github.io/schemas/anolis/...` in
  `runtime-config.schema.json` and `machine-profile.schema.json`. The v0.1.3 release
  bundles incorrectly contained `schemas.anolishq.dev` URIs.

## [0.1.3] - 2026-04-21

### Added

- Release workflow: three new schema bundles published as release assets on every `v*` tag:
  `runtime-config-schema`, `machine-profile-schema`, and `runtime-http-schema`. Each bundle
  includes the canonical schema file, baseline doc, fixture manifest, fixture tree, and
  validator script.
- Contract fixtures: added `manifest.yaml` to `tests/contracts/runtime-config/` and
  `tests/contracts/machine-profile/` listing all fixtures with expected outcomes, mirroring
  the telemetry-timeseries pattern.

## [0.1.2] - 2026-04-20

### Changed

- Release workflow: contract artifact publishing split into a dedicated `release-contracts` job,
  independent of the binary build job; schema lane made dependent on build success.
- Schema layout: `machine-profile.schema.json` and `runtime-config.schema.json` moved into
  domain subdirectories (`schemas/machine/` and `schemas/runtime/` respectively).
- Contract validators migrated from `tools/contracts/` into co-located
  `tests/contracts/<domain>/` directories alongside their fixture trees.
- `validate-doc-links.py` relocated from `tools/contracts/` to `tools/`.

## [0.1.1] - 2026-04-20

### Added

- Release workflow: packages telemetry schema bundle (`anolis-{VERSION}-telemetry-schema.tar.gz`)
  and `telemetry-schema-manifest.json` as release assets on every `v*` tag.

## [0.1.0] - 2026-04-20

First tagged release. The runtime was developed in full before tagging; this entry
summarizes the meaningful work that landed prior to `v0.1.0`.

### Added

- Runtime core: ADPP v1 proxy layer routing calls between the workbench and
  connected device providers over gRPC.
- Automation engine: behavior-tree (BT) execution with generic provider-agnostic
  nodes (`GetParameterInt64`, `CheckBool`, `PulseWindow`, `EmitGating`,
  `BuildArgsJson`). Monotonic BT timing and stage-transition hooks.
- Bioreactor Stage 1 automation wiring: stir/feed behavior tree, multi-channel
  DCMT/RLHT mapping to lab wiring (impeller, feed, base dosing, acid dosing).
- Telemetry export service: streamed NDJSON, CSV, and JSON export via HTTP;
  per-runtime scoping with `selector.runtime_names`; bounded CSV spooling with
  deterministic memory limit; manifest endpoint.
- `anolis_signal` tagged with `runtime_name` for multi-runtime Influx
  disambiguation.
- HTTP API v0 (Composer control): project lifecycle, provider attach/detach,
  machine-profile load, automation start/stop. OpenAPI baseline locked with
  conformance gates.
- Machine-profile contract: YAML schema, canonical bioreactor manifest, validation
  gate wired into CI and local verification.
- Runtime config JSON schema with validation gate in CI; pytest fixtures for
  config-schema edge cases.
- System Composer→Workbench extraction: `tools/system-composer` and
  `tools/workbench` removed from this repo; control contract migrated to
  `anolis-workbench`.
- Editable Python package layout (`core/` as an installable package); removed
  `sys.path` surgery from test and tool entry points.
- Package validator and contract script for the workbench handoff boundary.
- `ANOLIS_DATA_DIR` env var for data directory configuration; `REPO_ROOT` decoupled
  from assumptions about working directory.
- Release workflow: on `v*` tag, builds `ci-linux-release-strict`, packages
  `anolis-runtime` binary + source tarball + `manifest.json` + `SHA256SUMS`.
- Compatibility lane CI: validates runtime against a pinned `anolis-provider-sim`
  release tag; `dependency-pins.yml` locked to `v0.1.0` at first release.

### Changed

- Protocol submodule URL migrated from `FEASTorg/anolis` to
  `anolishq/anolis-protocol` after protocol extraction.
- Org renamed from `FEASTorg` to `anolishq` throughout.
- Operator UI extracted to separate `anolishq/anolis-operator-ui` repository.
- Telemetry export formats hardened: type-safe downsampling, timezone contract
  enforcement, env-based token overrides.
- DCMT PWM bounds aligned to `[-255, 255]` to match Nano hardware limits;
  bioreactor automation defaults updated.

### Fixed

- Multi-line CSV parsing bug in JSON export path; regression test added.
- `GetParameterInt64`→`GetParameter` fix for dual-dosing bioreactor automation
  tree load.
- Export e2e: module-mode startup, streamed format cleanup, response body surfaced
  on first-query failures.
- Composer runtime ownership: logs scoped to project; detached runtime
  status/stop reconciliation; restart conflict and project-switch safety guards.

[Unreleased]: https://github.com/anolishq/anolis/compare/v0.1.24...HEAD
[0.1.24]: https://github.com/anolishq/anolis/compare/v0.1.23...v0.1.24
[0.1.23]: https://github.com/anolishq/anolis/compare/v0.1.20...v0.1.23
[0.1.20]: https://github.com/anolishq/anolis/compare/v0.1.19...v0.1.20
[0.1.19]: https://github.com/anolishq/anolis/compare/v0.1.18...v0.1.19
[0.1.18]: https://github.com/anolishq/anolis/compare/v0.1.17...v0.1.18
[0.1.17]: https://github.com/anolishq/anolis/compare/v0.1.16...v0.1.17
[0.1.16]: https://github.com/anolishq/anolis/compare/v0.1.15...v0.1.16
[0.1.15]: https://github.com/anolishq/anolis/compare/v0.1.14...v0.1.15
[0.1.14]: https://github.com/anolishq/anolis/compare/v0.1.13...v0.1.14
[0.1.13]: https://github.com/anolishq/anolis/compare/v0.1.12...v0.1.13
[0.1.12]: https://github.com/anolishq/anolis/compare/v0.1.11...v0.1.12
[0.1.11]: https://github.com/anolishq/anolis/compare/v0.1.10...v0.1.11
[0.1.10]: https://github.com/anolishq/anolis/compare/v0.1.9...v0.1.10
[0.1.9]: https://github.com/anolishq/anolis/compare/v0.1.8...v0.1.9
[0.1.8]: https://github.com/anolishq/anolis/compare/v0.1.7...v0.1.8
[0.1.7]: https://github.com/anolishq/anolis/compare/v0.1.6...v0.1.7
[0.1.6]: https://github.com/anolishq/anolis/compare/v0.1.5...v0.1.6
[0.1.5]: https://github.com/anolishq/anolis/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/anolishq/anolis/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/anolishq/anolis/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/anolishq/anolis/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/anolishq/anolis/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/anolishq/anolis/releases/tag/v0.1.0
