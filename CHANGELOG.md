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

- `GET /v0/devices/{provider}/{device}/capabilities` now surfaces each
  function's actuation classification: a `category` field
  (`UNSPECIFIED`/`READ`/`CONFIG`/`ACTUATE`, read from the provider's
  `FunctionPolicy`) and a fail-closed `actuating` flag — true unless the
  function explicitly declares `READ`, so an untagged (`UNSPECIFIED`) or
  `CONFIG` function counts as actuating. Both fields are required in the v0
  capabilities schema. This lets clients enumerate a device's actuating
  outputs and is the shared dependency for the runtime software safe-state
  command and the refuse-hookless install gate (#218).

## [0.1.38] - 2026-07-22

### Fixed

- `install.sh --with-observability`: `influx setup` could fail on a cold data
  dir (first boot). InfluxDB 2.x reports `/health` OK before its KV/onboarding
  store is ready to service setup, so the single-shot setup call raced that
  window; a `>/dev/null` on the call then masked the error — the operator saw
  only "influx setup failed" while Grafana provisioning was skipped and the
  runtime/read tokens were left unwired. The setup is now retried with a
  bounded backoff that absorbs the readiness window, its output is captured so
  a genuine failure is diagnosable (with the generated admin secrets scrubbed),
  and an idempotency guard treats a server-side onboard that still returns a
  non-zero CLI exit as success — safe because the admin token is pinned before
  the loop, so downstream scoped-token creation still authenticates. Verified
  from zero on a Pi: fresh influxd first-boot now provisions turnkey (setup,
  tokens, Grafana datasource, dashboards, end-to-end telemetry) (#213).

## [0.1.37] - 2026-07-21

### Added

- New Grafana dashboard `I/O & Watchdog Health` (`anolis-io-health`) in the
  observability asset (#162): per-device io counters and watchdog state, I/O
  failure rate and retried-attempts slope (the bus-degradation leading
  indicator), watchdog-trip annotations derived from `watchdog_trip_count`
  steps, device staleness, and provider degraded/failed-device panels — all
  over the `anolis_*_health` measurements added in #203.
- Observability asset: the Grafana datasource is now provisioned from
  environment variables (`INFLUX_URL`/`INFLUX_ORG`/`INFLUX_BUCKET`/
  `INFLUX_TOKEN`) instead of a hardcoded Docker-DNS URL and `dev-token`,
  so the same provisioning files serve the compose stack and the native
  install path (#162). The compose file supplies the values from `.env`
  with the previous defaults, so existing docker workflows are unchanged.
  The observability README gained a data-retention section (30 d native
  default, override command, and the trade-offs of raising it).
- `install.sh --with-observability` is implemented (#162): provisions the
  co-located observability stack natively — influxdb2 + grafana from the
  vendor apt repos as systemd services (no Docker on the device), a
  non-interactive InfluxDB bootstrap (org/bucket `anolis`, **30 d bucket
  retention**, generated admin credentials in a 0600 root env file), and
  **scoped tokens**: a write-only token wired into the runtime's env file
  and a read-only token for Grafana (via a systemd drop-in) and
  telemetry-export. Dashboards/datasource provisioning come from the
  release observability asset at the installed runtime version. Online-only
  (warn-skips offline, preserving the `--local` guarantee); services start
  even under `--no-start` (that flag gates only the runtime); uninstall
  disables the services and removes provisioning but deliberately keeps the
  apt packages and recorded data, with purge steps printed. The compose
  stack remains the separate-host topology.

- Provider/device health metrics are now ingested into InfluxDB (#203): when
  telemetry is enabled, a snapshot task writes `anolis_provider_health` and
  `anolis_device_health` rows every `telemetry.health_interval_ms` (default
  15000, `0` disables). Device rows carry the typed reserved-key metrics
  (`io_*`, `watchdog_*`, `sample_*`/`call_*`, `missing`/`excluded`) parsed
  from provider health reports via a strict allowlist; provider rows carry
  availability, lifecycle, `degraded`/`failed_device_count`, supervision, and
  startup counters. New contract
  `schemas/telemetry/telemetry-health-timeseries.schema.v1.json` ships in the
  telemetry schema release artifact (the `anolis_signal` v1 file is
  unchanged). `GET /v0/providers/health` and the ingestion path now share one
  snapshot collector, so the HTTP and timeseries views cannot drift.

## [0.1.36] - 2026-07-20

### Added

- `CallDeviceNode` gained an optional `reason` input, logged as
  `(reason=first|force|changed|keepalive)` on success when wired from
  `EmitOnChangeOrInterval`'s existing reason output (#196). Makes real
  command changes journal-distinguishable from liveness re-emissions;
  trees that don't wire the port are unchanged.
- Machine profiles may declare `safety.estop_topology: power_cut | signal`
  (#197) — an optional, purely informational record of how the physical
  e-stop is wired, with both wirings' software signatures documented in
  the machine-profile baseline. Nothing branches runtime behavior on it;
  absence is valid. Additive schema change (downstream: anolis-projects
  `schema-source.json` pin, workbench schema re-lock).

### Changed

- Release-asset download failures now say what to do next (#138 matrix row
  13): the error names the asset and points at the release page, suggests
  `GITHUB_TOKEN` for rate-limited unauthenticated downloads, and reminds that
  `--stage`/`--local` covers fully-offline installs.

## [0.1.35] - 2026-07-15

### Added

- Mode-transition hook executions are now logged on success (#188), one INFO
  line per call (`Before/After-transition hook (AUTO -> MANUAL):
  bread0/dcmt0 set_open_loop OK`). The safe-state hooks that zero actuators
  on AUTO exit were running silently — failures logged, successes left no
  evidence, which is both an operator-confidence problem during supervised
  actuation sessions and how the hooks were mistaken for missing in the
  first place.

### Fixed

- `install.sh` no longer overwrites `.prev` on a same-version re-run (#184).
  The backup phase previously snapshotted the running binaries on every
  install, so an idempotent re-run after an upgrade silently replaced the
  real rollback point with a copy of the current binaries — `--rollback`
  then "succeeded" as a no-op. The backup is now skipped when the incoming
  `bin/` payload is byte-identical to what is installed.

## [0.1.34] - 2026-07-14

### Added

- `/v0/providers/health` now carries the provider's own health report (#185).
  The runtime fetches ADPP `GetHealth` from each available provider on demand
  and merges it into the response: a per-provider `reported` block (state,
  message, metrics) plus `degraded` and `failed_device_count`; per-device
  `reported` blocks (state, message, metrics, `last_seen_epoch_ms`) alongside
  the runtime's own freshness view; and devices the provider reports but the
  registry does not carry — e.g. expected devices that failed their startup
  probe — appended with `registered: false`. Previously a provider could sit
  at `AVAILABLE` with a silently missing configured device, and
  provider-reported metrics (such as bread 0.3.3's `io_*` counters and real
  `last_seen`) were invisible over HTTP.

## [0.1.33] - 2026-07-14

### Fixed

- `install.sh` no longer fails with `Text file busy` when installing over a
  running service (#181). Re-install, upgrade, and `--rollback` all wrote the
  new binaries onto the executing ELFs in place; binaries are now staged to a
  temp name and renamed over the destination, so the running process keeps its
  old inode until the restart both flows already perform. Found on the #138
  bench — every prior hardware install had followed an `--uninstall`, so the
  in-place path had never run against a live service.
- `install.sh` repairs `/etc/hosts` even when the device already carries its
  `anolis-<serial>` hostname (#180). The early-return in `phase_hostname`
  skipped the hosts fix from #173 on exactly the devices that needed it,
  leaving `sudo: unable to resolve host` warnings on re-installs.

## [0.1.32] - 2026-07-13

### Security

- `install.sh` now provisions the runtime with authentication enabled whenever it
  exposes the HTTP API on a non-loopback address. The bundle renderer emits
  `http.auth_enabled: true` alongside the `0.0.0.0` bind, and a new install phase
  generates a per-device 256-bit API token into `/etc/anolis/runtime.env`
  (`0600 root:root`), injected into the service via systemd `EnvironmentFile=`.
  The secret never enters `runtime.yaml` and is never baked into a bundle, which
  is a redistributable artifact. The token is reused across upgrades, so an
  upgrade does not invalidate tokens already issued to clients. Requests from the
  device itself remain exempt (`auth_exempt_loopback`), so on-device access and
  the health probe are unaffected. (#172)

### Fixed

- `install.sh` rewrote the runtime's `bind` to `0.0.0.0` without enabling
  authentication, producing a config the runtime is **guaranteed** to reject at
  startup ("non-loopback but authentication is disabled"). The online `--project`
  path therefore installed a runtime that could never start; it crash-looped while
  the installer reported success. Found on real hardware during the #138
  acceptance run. (#172)
- `install.sh` now preflights the runtime config against the constraint the
  runtime enforces at startup and refuses to install one that cannot start,
  instead of shipping a boot loop. This also covers operator-edited configs
  preserved across upgrades. (#172)
- `install.sh` no longer prints "installation complete" and exits `0` when the
  runtime failed to come up. A failed health check is now a failed install, so
  `curl … | sudo bash` and workbench's `deploy.py` see a non-zero exit. The
  I2C-just-enabled/reboot-pending case is still reported as advisory, not a
  failure. (#172)
- `install.sh` set the device hostname without updating `/etc/hosts`, leaving the
  machine unable to resolve its own name (`sudo: unable to resolve host
  anolis-<serial>`) and undermining the `http://anolis-<serial>.local:8080` access
  URL it prints. (#172)

## [0.1.31] - 2026-07-04

### Added

- `install.sh` gained `--with-telemetry-export`, which provisions the
  telemetry-export service (a venv built from the pinned PyPI package)
  alongside the runtime. It is installed inert and only starts once the
  export/InfluxDB tokens are supplied. (#163)

### Fixed

- `install.sh`: the runtime systemd unit's `ExecStart` now honors `--prefix`
  instead of hardcoding `/opt/anolis`; the post-install health check and
  summary read the HTTP port from the installed `runtime.yaml` rather than
  assuming `8080`; and bundle verification uses `sha256sum -c`, naming any
  file that fails. (#168)

### Changed

- The OpenAPI contract now represents the auth/TLS surface. (#160)
- Added `docs/system-surfaces.md`, a component boundary & ownership map. (#159)

## [0.1.30] - 2026-07-03

### Changed

- machine-profile schema: `runtime_profiles` now allows arbitrary additional
  variant names (`manual` remains required); richer field descriptions ported
  from the anolis-projects fork; consumer list documented in the baseline.
  (#156, #147)
- `install.sh` sets umask 022 and creates the prefix tree with explicit 755
  modes; `--with-telemetry`/`--with-observability` warn that they are not
  implemented yet instead of silently no-oping. (#155)

## [0.1.29] - 2026-07-03

### Fixed

- `install.sh` no longer copies `/etc/skel` into the install prefix when
  creating the `anolis` system user on first install (found by the workbench
  deploy-parity gate). (#152)

## [0.1.28] - 2026-07-03

### Added

- `install.sh --rollback`: restore the previous binaries from
  `<prefix>/.prev` (backed up on every install), restart the runtime
  service, and health-check — the restore verb the workbench rollback
  delegation routes through. (#136)

## [0.1.27] - 2026-07-03

### Added

- `install.sh --stage`: build an offline install bundle from a project config
  (machine-profile + configs + behaviors) on a dev machine, generic over the
  providers the profile declares. (#143)
- `install.sh --project <dir>`: config-driven online install — assemble the
  bundle from the config at install time and install it in one step. (#145)
- The machine-profile schema now declares the optional `components:` section
  (pinned runtime/provider/optional artifact versions) that `install.sh` reads
  and Renovate auto-bumps in project configs, with contract fixtures. (#147)

### Removed

- `install.sh --profile`/`--version` bundle-download mode (replaced by
  `--project`; the bundle a deployment needs is derived from its config, not
  published per example project). (#145)

### Fixed

- `install.sh` derived the bundle download URL from the requested version
  rather than the resolved release tag, breaking `--version` pinning. (#135)

## [0.1.26] - 2026-06-28

### Removed

- The deprecated behaviour-tree mirror fields on `GET /v0/automation/status`
  (`enabled`, `active`, `bt_status`, `last_tick_ms`, `ticks_since_progress`,
  `total_ticks`, `error_count`, `current_tree`) and the deprecated `bt_error`
  SSE alias frame are removed (Phase 4 of the automation-platform epic). The
  forward contract is the neutral set (`execution_status`, `execution_reason`,
  `automation_version`, `last_evaluation_at_epoch_ms`, `engine_diagnostics`,
  `run_id`, `last_error`) and the neutral `automation_fault` SSE event. The sole
  remaining consumer (Workbench) migrated and re-pinned in v0.1.25's one-release
  compatibility window; operator-ui is archived. (anolishq/anolis#117)

## [0.1.25] - 2026-06-28

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

[Unreleased]: https://github.com/anolishq/anolis/compare/v0.1.32...HEAD
[0.1.32]: https://github.com/anolishq/anolis/compare/v0.1.31...v0.1.32
[0.1.31]: https://github.com/anolishq/anolis/compare/v0.1.30...v0.1.31
[0.1.30]: https://github.com/anolishq/anolis/compare/v0.1.29...v0.1.30
[0.1.29]: https://github.com/anolishq/anolis/compare/v0.1.28...v0.1.29
[0.1.28]: https://github.com/anolishq/anolis/compare/v0.1.27...v0.1.28
[0.1.27]: https://github.com/anolishq/anolis/compare/v0.1.26...v0.1.27
[0.1.26]: https://github.com/anolishq/anolis/compare/v0.1.25...v0.1.26
[0.1.25]: https://github.com/anolishq/anolis/compare/v0.1.24...v0.1.25
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
