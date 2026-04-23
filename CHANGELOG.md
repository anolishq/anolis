# Changelog

All notable changes to the `anolis` runtime are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

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
