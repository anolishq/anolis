# Changelog

All notable changes to the `anolis` runtime are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

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
