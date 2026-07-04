# System Surfaces — Boundary & Ownership Map

Every place an external actor, subsystem, or developer interacts with Anolis:
the file that IS the contract today, who owns it, and how stable it is. This
describes **today**, not target state. It links to existing inventories
([schemas/README.md](../schemas/README.md),
[docs/contracts/README.md](contracts/README.md),
[architecture.md](architecture.md)) rather than duplicating them.

Stability labels: **versioned-contract** (released, consumers pin it) ·
**internal-API** (private between co-shipped components) · **examples**
(copy-and-tweak) · **scaffold** (exists, not yet load-bearing) ·
**absent** (named in older docs but has no code today).

| Surface | Contract (the file that IS it) | Owner | Stability |
| --- | --- | --- | --- |
| Provider protocol (ADPP) | `proto/anolis/deviceprovider/v1/*.proto` + `docs/semantics.md` + `docs/profiles/framed-stdio-v1.md` | `anolis-protocol` (v1.6.0) | versioned-contract |
| Provider SDK | `src/anolis/provider_sdk/` headers (`DeviceAdapter<HandleT>`, `ProviderRuntime`) | `anolis-provider-sdk` (v0.1.2) | versioned-contract (young) |
| Device protocols | CRUMBS: `anolis-provider-bread/src/crumbs/`; EZO: `anolis-provider-ezo/docs/device-contracts-v1.md`; sim devices: `src/devices/` | provider repos (+ external `feastorg` contracts for CRUMBS wire meaning) | versioned-contract per provider |
| Configuration | `schemas/runtime/runtime-config.schema.json`, `schemas/machine/machine-profile.schema.json` | `anolis` (released per version) | versioned-contract |
| HTTP / OpenAPI | `schemas/http/runtime-http.openapi.v0.yaml` (`/v0`) | `anolis` | versioned-contract (auth/TLS gap — see [Divergences](#known-divergences)) |
| Automation | `automation:` config + BT XML; `/v0/automation/*`, `/v0/mode`, `/v0/parameters`; `IAutomationEngine` seam | `anolis` `core/automation/` | versioned-contract (wire), internal-API (engine seam) |
| Persistence / historian | `schemas/telemetry/telemetry-timeseries.schema.v1.json`; run journal JSONL | `anolis`; export service in `anolis-telemetry-export` | versioned-contract (telemetry), internal-API (journal file) |
| CLI / process | `anolis-runtime` flags/signals/exit codes; the [executable profile](https://github.com/anolishq/anolis-protocol/blob/main/docs/profiles/anolis-executable-profile-v1.md) for providers | `anolis` / `anolis-protocol` | versioned-contract |
| Deployment / provisioning | `tools/install.sh` (release asset) + machine-profile `components:` | `anolis` | versioned-contract |
| Workbench / UI | `anolis-workbench/contracts/` (`workbench-api`, `composer-control`, `.anpkg` handoff) | `anolis-workbench` | internal-API (`/api/*`), versioned-contract (`.anpkg`) |
| Observability | stderr logs, `/v0/*/status` health, SSE `/v0/events`, `anolis-<v>-observability.tar.gz` stack | `anolis` | versioned-contract (HTTP), internal-API (log format) |
| Extension / plugin | new provider = SDK + conformance manifest; new automation engine = `IAutomationEngine` | SDK / `anolis` | versioned-contract (provider), scaffold (engine) |

---

## Provider protocol (ADPP)

The wire schema is protobuf (`anolis-protocol/proto/anolis/deviceprovider/v1/`,
10 files; buf module `buf.build/anolishq/anolis-protocol`); normative semantics
in `docs/semantics.md`, transport binding in `docs/profiles/framed-stdio-v1.md`
(length-prefixed `uint32_le` frames over the provider child's stdin/stdout,
1 MiB cap). The wire version is the literal string `"v1"` (handshake); the
**repo** versions by SemVer (v1.6.0 today) with `buf breaking` gating and
semantic tightening shipped as opt-in **conformance levels** (L1 core; L2 adds
pre-Hello `FAILED_PRECONDITION`, `OUT_OF_RANGE` bounds, non-finite
`INVALID_ARGUMENT`, deadlines, OK-read timestamp/quality). All three providers
declare L2 (`config/conformance.toml` each). A Python conformance harness
ships in the release wheel (v1.3.0+, release-asset only — no PyPI); the org 0F
compat matrix (`anolishq/.github`, weekly, report-only) crosses released
harness × provider versions and publishes a durable verdict to the
`adpp-compat-data` branch (`data/adpp-compat/latest.json`).

This repo **consumes** ADPP via the provider SDK chain — it does not
re-publish it. The pin is a release-tarball URL + SHA256 in
`core/CMakeLists.txt` (v1.2.0 today; providers ride v1.6.0 via the SDK — the
runtime's older pin is fine because the wire is `"v1"` either way).

## Provider SDK & extension story

`anolis-provider-sdk` is the **single declarant** of the anolis-protocol pin
for the provider fleet: it FetchContents the protocol (v1.6.0), re-exports
`anolis::adpp_proto`, and provides the spine (`ProviderRuntime` run-loop:
Hello gate, dispatch, exit codes; framed-stdio transport; config/logging) plus
the device seam (`DeviceAdapter<HandleT>` templated over each provider's
session handle). All three providers pin SDK v0.1.2 by tarball+SHA and no
longer declare the protocol directly. A new provider author: implement the
adapter seam, write `config/conformance.toml` (+ mock `conformance.yaml`), and
follow sim's `src/devices/` structure (the de-facto template — its README says
"copy this structure"). Providers are launched **by the runtime**, never run
directly; shared executable conventions (`--config`, `--version`,
`--check-config`) come from the executable profile, which is an org
convention distinct from (and waivable independently of) ADPP conformance.

## Device protocols

- **CRUMBS** (bread, I2C): session/bus behavior terminates in
  `anolis-provider-bread/src/crumbs/` (see its README); the RLHT/DCMT wire
  *meaning* lives in external `feastorg` repos (`bread-crumbs-contracts`
  v0.4.3, `CRUMBS` v0.12.4, `linux-wire`) — a cross-org ownership edge.
- **EZO** (I2C sensors): 6 device families (`sensor.ezo.{ph,orp,ec,do,rtd,hum}`),
  3 shared safe functions per family; contract in
  `anolis-provider-ezo/docs/device-contracts-v1.md`.
- **sim**: `Tempctl, Motorctl, Relayio, Analogsensor` + a chaos fault-injection
  device; simulation engines local/null/remote, with an optional
  **FluxGraph** adapter (gRPC to `anolishq/fluxgraph`, a standalone C++
  physics-simulation library). FluxGraph's only ecosystem integration point is
  sim — the runtime has none.
- **GRAIN**: absent — no code or docs anywhere in the org today.

## Configuration

Two JSON Schemas (Draft-07), released with every anolis version as artifacts:
`schemas/runtime/runtime-config.schema.json` (sections: `runtime, http,
providers[], polling, telemetry, logging, automation`) and
`schemas/machine/machine-profile.schema.json` (the deployment descriptor;
`components:` pins are what `install.sh` installs and Renovate bumps).
Authority note: the schemas are structural; the yaml-cpp loader
(`core/runtime/config.cpp`) is semantically authoritative — unknown keys warn,
deprecated aliases are honored ([schemas/README.md](../schemas/README.md)).
Point-in-time behavior freezes live in [docs/contracts/](contracts/README.md).

**Schema consumers** (a schema change must be released, then propagated —
see `contracts/machine-profile-baseline.md`): workbench (vendored copy
byte-locked to a release artifact, CI-verified), anolis-projects (CI fetches
at its `schema-source.json` pin, sha-verified, Renovate-bumped), docs site
(injects at its pin).

## HTTP / OpenAPI

`schemas/http/runtime-http.openapi.v0.yaml` — OpenAPI 3.1, everything under
`/v0`: runtime/provider/telemetry status, devices + capabilities, state,
`/v0/call`, `/v0/mode`, `/v0/parameters`, automation tree/status, runs (+ run
events), and SSE `/v0/events` (state/quality/availability/mode/parameter/
provider-health/`automation_fault`). Routes register in
`core/http/server.cpp`; human guide in [docs/http/README.md](http/README.md).
**Security is real but code-defined**: Bearer auth (`core/http/auth.cpp`,
global pre-routing 401; loopback-exempt by default; token via config or
`ANOLIS_API_TOKEN`), TLS via `tls_cert_path`/`tls_key_path`
(`httplib::SSLServer`), and startup guards that refuse non-loopback binds
without auth (`allow_insecure_bind` opt-out). None of this appears in the
OpenAPI file yet — operator guidance is [docs/security.md](security.md); see
[Divergences](#known-divergences).

## Automation

Config-driven BehaviorTree: `automation.behavior_tree` (XML path),
`tick_rate_hz`, `manual_gating_policy`, `parameters[]`,
`mode_transition_hooks`. Wire surface: `/v0/automation/tree|status`,
`/v0/mode`, `/v0/parameters`, plus the `automation_fault` SSE event (neutral
since v0.1.26 — no BT vocabulary). Internally the engine seam exists
(`IAutomationEngine`, `core/automation/automation_engine.hpp` — Phase 0 of the
[automation-platform RFC](rfcs/automation-platform.md)); `BTRuntime` is the
only implementation and parts of the HTTP layer still read BT-shaped state, so
treat the seam as **scaffold** until the RFC's later phases land.

## Persistence / historian

The runtime is **store-less by default**. Optional telemetry sink:
`core/telemetry/influx_sink` writes InfluxDB v2 line protocol per the released
contract `schemas/telemetry/telemetry-timeseries.schema.v1.json`
(`anolis_signal` measurement); disabled unless `telemetry.enabled` and a token
(`INFLUXDB_TOKEN`). Run identity persists in an append-only JSONL journal
(`<data_dir>/runs/index.jsonl`, fsync-per-transition, abandoned-run recovery,
one open run at a time) — an internal file format, reached externally only via
`/v0/runs*`. Historian *export* is `anolis-telemetry-export` (separate repo,
v0.1.0): an authenticated HTTP service (default :8091) that queries InfluxDB
and serves guarded CSV/NDJSON/JSON exports; it never touches the runtime API.

## CLI / process

`anolis-runtime` (`core/src/main.cpp`): `--config`, `--check-config`
(validate+exit), `--version` (stdout), `--help`; exit codes 0/1 only; graceful
shutdown on SIGINT/SIGTERM; env vars read: `ANOLIS_API_TOKEN`,
`INFLUXDB_TOKEN`. All logs to stderr as plain text
(`[timestamp] [LEVEL] message`). Providers follow the executable profile
(above) and are spawned by the runtime.

## Deployment / provisioning

**Ownership model** (the #139 resolution — previously undocumented, which is
how the engines diverged):

- The runtime **spawns and supervises providers as child subprocesses**
  (`providers[].command` + `restart_policy`, framed stdio). There is no
  connect-to-a-running-provider mode.
- A deployment is therefore **one systemd unit**, `anolis-runtime.service`
  (`User=anolis`, `SupplementaryGroups=i2c gpio dialout`); providers are never
  independent services. The canonical unit ships inside `install.sh`.
- **`tools/install.sh` is the single provisioning engine** (release asset;
  verbs: `--project` online config-driven install, `--stage`+`--local`
  offline bundle, `--rollback`, `--uninstall`). The machine-profile
  `components:` section is where deployment versions live.
- **Authoring vs deploy are separate concerns**: `~/.anolis/systems` (current
  user, workbench `launcher.py` dev-launch) is the source-of-truth workspace;
  `/opt/anolis` is a derived artifact. Workbench *delegates* deployment to
  install.sh (`anolis_workbench/core/deploy.py`, local or over SSH).
- Enforcement: the workbench **deploy-parity gate** byte-compares a workbench
  deploy against `--stage`+`--local` of the same config on x86_64 and arm64,
  weekly and on deploy-surface PRs.
- `anolis-projects` holds example deployment descriptors only (stability:
  **examples**); the observability stack (InfluxDB+Grafana compose) ships as
  `anolis-<v>-observability.tar.gz`. `--with-telemetry-export` installs the
  telemetry-export service (venv + systemd unit, inert until secrets; #137);
  `--with-observability` still warns as not-implemented (#162).

## Workbench / UI

`anolis-workbench` (v0.11.7): the authoring/commissioning app. Its `/api/*`
HTTP surface (default :3010; compose/commission/operate/provision routes) is
**internal-API** — a private localhost contract between the bundled frontend,
the Tauri shell, and the sidecar (documented in
`contracts/workbench-api.openapi.v1.yaml`, but not a published API). It also
reverse-proxies the runtime's public `/v0/*` (stability owned by this repo's
OpenAPI). The **`.anpkg` handoff package** is a versioned contract
(`package_format_version: 1`): deterministic ZIP of machine-profile + rendered
configs + behaviors + provenance + checksums, secret-redaction enforced
(`contracts/README.md`, `docs/contracts/handoff-package-*.md` in workbench).
Upstream anolis contracts are consumed via byte-locks
(`contracts/upstream/anolis/*.lock.json`). Desktop distribution: Tauri 2 app
(msi/deb/appimage/dmg + PyPI wheel), sidecar frozen via PyInstaller.
`anolis-operator-ui` is **retired** (archived; superseded by Workbench
Operate).

## Observability

Logs: plain-text stderr only (internal format). Health: `/v0/runtime/status`
(+ per-subsystem `/v0/providers/health`, `/v0/telemetry/status`,
`/v0/automation/status`) — there is no bare `/health` and **no metrics/
Prometheus endpoint** (the `metrics.json` release asset is repo engineering
metrics from CI, not runtime telemetry). Live events: SSE `/v0/events` with
quality states `OK/STALE/UNAVAILABLE/FAULT`. Operator dashboards: the
released observability stack (Grafana + InfluxDB compose).

---

## Known divergences

Verified 2026-07-03; each should shrink over time.

| Divergence | Where | Tracking |
| --- | --- | --- |
| Bearer auth + TLS enforced in code but absent from the OpenAPI contract (no `securitySchemes`, no 401s) | `core/http/auth.cpp` vs `schemas/http/runtime-http.openapi.v0.yaml` | anolishq/anolis#158 |
| Workbench renders a telemetry-export config the service cannot load (`runtime:`+`influxdb:` vs required `server:`+`influxdb:`+`limits:`) | `anolis-workbench/anolis_workbench/core/telemetry_config.py` vs `anolis-telemetry-export/export_core/config.py` | anolishq/anolis-workbench#175 |
| Upstream anolis contract pins advance independently (workbench: machine-profile v0.1.30 / runtime-config v0.1.5 / runtime-http v0.1.25; docs site 0.1.26; telemetry-export v0.1.24) | each consumer's lock/pin | anolishq/anolis-workbench#176 (workbench locks) |
| SDK docs lag its shipped surface (README/CHANGELOG say "scaffold 0.1.0"; v0.1.2 spine+adapter is consumed by all providers) | `anolis-provider-sdk` | — (docs fix) |
| Wire version literal `"v1"` duplicated (proto + SDK default) with no shared constant | `handshake.proto` / SDK `runtime.hpp` | — (accepted; changes with ADPP v2 only) |
| Tauri desktop health-probe hard-depends on `/api/status` body containing `"workbench"`/`"composer"` — pinned nowhere | workbench `desktop/src-tauri/src/main.rs` | — (note here is the pin) |
