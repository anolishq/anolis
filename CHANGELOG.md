# Changelog

All notable changes to the `anolis` runtime are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

## [0.9.2] - 2026-10-31

0.9.2 is a patch release: Grafana dashboard display names, nothing in the
runtime. An install with `--with-observability` refreshes the dashboards
from the release asset.

### Fixed

- **Grafana panels name what they show** (#712). Stat tiles on Device
  Health and System Overview read "_value" above their number (the Flux
  column's name); they now show the value alone, as the title names it.
  I/O & Watchdog Health named each device's series and tiles
  `bread0/dcmt0 {device_id="dcmt0", provider_id="bread0", runtime_name=…}`;
  they now read `bread0/dcmt0`, and Provider Degraded / Failed Devices
  reads "bread0 degraded" and "bread0 failed devices". Its status table and
  Signal History's String table head their columns Provider, Device,
  I/O OK, Trips and so on, not `provider_id`, `io_ok`, `watchdog_trip_count`.

## [0.9.1] - 2026-10-31

0.9.1 is a patch release: the Grafana dashboards' queries, nothing in the
runtime. An install with `--with-observability` refreshes the dashboards
from the release asset.

### Fixed

- **Grafana's dashboards no longer time out on a Raspberry Pi** (#708).
  Twelve panels on Signal History, Device Health and System Overview seed
  each series with its last value from the 30 days before the range, and
  filtered that read by the live runtimes before `last()`. That filter can't
  be pushed down to storage, so InfluxDB read the whole 30 days into Flux on
  every refresh: 22-36 s per query on the reference rig, past Grafana's 30 s
  timeout. The filter now follows `last()`, and the same queries take
  0.13-1.1 s there with the same results (Quality Distribution Over Time,
  6.1 s, is slow for another reason).

## [0.9.0] - 2026-10-31

0.9.0 is a minor release because an existing profile's operator panel
behaves differently (`docs/dependencies.md`: before 1.0, a change that makes
an existing install behave differently bumps MINOR). The operator block's
`experiment` is now `run`, and `run.start.enable` now lists the groups' on
switches, which Start no longer turns on. Runs take a `label` in place of
`experiment_label`.

New in 0.9.0, all additive: `require.when` on Start conditions (#701),
`operator.actions` and the `RequestedPulseWindow` node with `max_queued`
(#697), and `estop.uncovered_functions` and `fault_uncovered_functions` in
the runtime status (#698). Each is under Added.

Upgrade notes for operators:

- **`operator.experiment` is now `operator.run`** (#696). A profile that
  still says `experiment` validates through 0.9.x, as a deprecated alias; one
  with both is refused. Rename the key to `run` before 0.10. `run` may also
  carry a `label`, the heading of the panel's run section.
- **Start leaves the On switches as they are** (#701). `run.start.enable` now
  lists the groups' on switches. With a workbench that reads 0.9.0's
  schema, Start run runs only the groups an operator ticked On, and refuses
  while none is; it used to set every `start.enable` parameter true. Stop
  still sets `stop.disable` false. A profile that relied on Start to switch
  everything on now needs the groups ticked first.
- **Runs take a `label`** (#696). `experiment_label` still works through
  0.9.x and is returned with the same value; a request that sends both with
  different values is refused with 400. Send `label`.

### Added

- **Start conditions that apply only when a group is on** (#701). A
  `run.start.require` entry (and one under the deprecated `experiment`)
  takes an optional `when`: the name of a bool parameter, normally one of
  `start.enable`. The condition then applies only while that parameter reads
  true at Start. Without `when` it applies always, as before, so 0.8.0
  profiles stay valid. The runtime does not read the operator block; the
  workbench evaluates `when`.
- **The runtime status names what a stop or a fault leaves running** (#698).
  `estop.uncovered_functions` lists the functions `uncovered_actuating_functions`
  counts, as `provider/device/function`: what a software stop leaves as it was.
  With automation enabled, `fault_uncovered_functions` lists the actuating
  functions no `AUTO -> FAULT` hook drives, which a fault in AUTO leaves as
  they were; it was only the startup preflight's warning before. Both are
  additive; the count stays.
- **One pulse per operator request** (#697). The `RequestedPulseWindow`
  behaviour-tree node runs one pulse, `delay_s` after a request and `pulse_s`
  long, each time its `request` counter parameter increases. Requests that
  arrive while one runs wait their turn (`queued`), up to `max_queued`
  outstanding, the running one included (default 1); presses beyond it add
  nothing. The counter's value when the tree starts is the baseline, so a
  start runs nothing, and a counter that drops only moves the baseline. A tree
  cannot write parameters, so a counter, not a bool, is what an operator's
  request can be. The machine profile's `operator.actions` declares buttons
  that increase such a counter, each with a `label` ("Sample now"); the
  workbench shows them. Neither names a device.

### Changed

- **`run.start.enable` lists the groups' on switches** (#701). A consumer's
  Start runs only if at least one is on, and no longer sets them true, so the
  operator's On switches decide what a run drives; `stop.disable` still sets
  its parameters false. The shape is unchanged: only the schema's description
  and the contract doc say so.
- **The operator block's `experiment` is now `run`** (#696). A filament
  extruder or a centrifuge does a run, not an experiment.
  `operator.run` has the same shape as `operator.experiment` (`start` with
  `enable`, `mode`, `require`; `stop` with `disable`, `mode`) and an optional
  `label`, the heading the workbench gives the section (default "Run").
  `operator.experiment` is accepted as a deprecated alias through 0.9.x; a
  profile with both is refused.
- **Runs take a `label`** (#696). `POST /v0/runs` accepts `label`, and every
  run record carries it. `experiment_label` is its deprecated alias through
  0.9.x: a request may still send it, and records and responses carry it with
  the same value as `label`. A request that sends both with different values is
  refused with `INVALID_ARGUMENT` (400). Journals written before this read the
  old `experiment_label` as the label.

### Docs

- The install-route pages say what the install does to the runtime's bind
  and auth (it rewrites a loopback bind to `0.0.0.0` with auth on), show the
  health check with its token, and say how to apply a safe-state fix after a
  failed install (#692).
- The HTTP API reference documents the runs and telemetry endpoints, the
  provider pages point at the SDK and its conformance harness, and the
  safety anchors work on the docs site as well as on GitHub (#693).

### CI

- The install walk runs against runtime 0.8.0 (#691).

## [0.8.0] - 2026-10-30

0.8.0 is a minor release because a running machine behaves differently:
automation whose device calls keep failing now enters FAULT
(`docs/dependencies.md`: before 1.0, a change that makes an existing install
behave differently bumps MINOR). The machine-profile schema gains
`operator.experiment.start.require`, and the runtime-http contract gains an
event and a status field.

Upgrade notes for operators:

- **A repeatedly failing automation call faults the runtime** (#687, Fixes
  #678). When the same device call automation makes is refused or fails three
  times in a row, the runtime enters FAULT, which runs the `AUTO -> FAULT`
  hooks, and the fault names the call and its last error. Before, the tree was
  ticked again for as long as AUTO lasted, and nothing said the calls were
  failing: on the reference rig, dosing ran 60 s in AUTO with every pulse
  refused. A success resets the count, and so does entering AUTO. Calls the
  device-loss latch refuses count too: a tree that keeps calling a device the
  latch holds now enters FAULT after three refusals, where it used to run on
  until MANUAL -> AUTO re-armed the device. Set
  `automation.max_consecutive_call_failures` (default 3, at least 1) higher to
  allow more failures in a row.

### Added

- **`operator.experiment.start.require` in the machine-profile schema** (#689,
  Fixes #686): optional conditions on signals that must hold before an
  operator panel's Start runs, each `{signal, equals, message}`, where
  `equals` is a bool, number or string and `message` is the fix the operator
  sees. Profiles without it, 0.7.0's included, stay valid.
- **`automation.max_consecutive_call_failures`** (#687), default 3, at least 1.
- **`automation_call_failed` SSE event** (#687), one per refused or failed
  automation device call: `device_handle`, `function`, `error`,
  `consecutive_failures`.
- **`call_failures` in `GET /v0/automation/status`** (#687): each device call
  that failed this run, with its failures in a row, last error and when.

### Changed

- **A repeatedly failing automation call faults the runtime** (#687): see the
  upgrade note.

### CI

- The install walk runs against runtime 0.7.0 (#685).
- The runtime-http OpenAPI check fails when the `/v0/events` description
  leaves out an event the SSE serializer sends (#687).
- pytest moves to `>=9.2.0,<9.3.0` for the Python checks (#688).

## [0.7.0] - 2026-10-30

0.7.0 is a minor release because existing installs behave differently: run
records and events are synced to storage before the API answers, a write
InfluxDB rejects is dropped instead of retried, and providers no longer
inherit each other's pipes (`docs/dependencies.md`: before 1.0, a change that
makes an existing install behave differently bumps MINOR). The machine-profile
schema gains an optional `operator` block.

Upgrade notes for operators:

- **Each run-journal append is synced to storage before it returns** (#673,
  Fixes #551). `POST /v0/runs`, `POST /v0/runs/{id}/close` and run events used
  to report success once the bytes reached the page cache, so a power cut soon
  after could lose a confirmed record. Each append to `runs/index.jsonl` or a
  run's events file is now fsynced, and `runs/` is synced after a new file is
  created. A failed sync is the call's error: open and close report no success,
  and an event append returns `ok == false`. Events are appended off the tick
  thread.
- **A write InfluxDB rejects is dropped, not retried** (#671, Fixes #572). A
  4xx other than 429 (400, 401, 403, 404, 413, 422) used to go back into the
  retry buffer and be sent ahead of every new batch; once the buffer filled,
  each new batch was dropped instead, until a restart. Now the rejected request
  is dropped, counted in `total_failed`, and logged as an ERROR with the status,
  the server's error and the rows dropped, for example
  `[InfluxSink] Write rejected (HTTP 400): …; dropped 2 rows, not retried (2 rejected in all)`.
  401 and 403 add "(check the InfluxDB token and bucket)": the token is read
  only at start, so fix it and restart. 429, 5xx and connection errors are
  retried as before.
- **Provider pipes are created close-on-exec** (#676, Fixes #570). On Linux,
  each provider inherited the runtime's ends of every pipe made before it,
  including earlier providers' stdin. Stopping a provider closes its stdin and
  waits for EOF, which never came while a later provider held a copy, so the
  stop waited out `shutdown_timeout_ms` and killed it, and the SDK's
  `on_shutdown` never ran. Providers now hold only their own fds 0, 1 and 2,
  shut down when asked, and run `on_shutdown`. Windows was unaffected.
- **A provider that closes its stdin no longer ends the runtime** (#675, Fixes
  #549). The runtime now ignores `SIGPIPE` from the start, so such a write fails
  with "Broken pipe (provider terminated)" and that provider is marked
  unavailable, where the runtime used to exit 141 with no log. That was
  possible during provider startup and whenever `http.enabled` was false; the
  installed service was already covered by systemd's `IgnoreSIGPIPE=yes`.
  Providers start with the disposition the runtime inherited, as before.
- **`value_double` is written so it reads back exactly** (#674, Fixes #573).
  It used to be rounded to 6 significant digits on its way to InfluxDB:
  `123456.789` was stored as `123457`. New points carry the full value; points
  already written keep their rounding.

### Added

- **An optional `operator` block in the machine-profile schema** (#682, Fixes
  #680): the readings an operator watches, the experiment's settings in
  groups, and what Start and Stop set. Stop's `mode` takes `IDLE` or `MANUAL`
  only. `docs/contracts/machine-profile-baseline.md` describes it, and the
  release's machine-profile schema bundle carries the schema and its new
  fixtures.
- **`InfluxSink::total_rejected()`** (#671), and the OpenAPI `total_failed`
  description says rejected writes count in it.

### Changed

- **Run-journal appends are synced to storage** (#673), as the upgrade notes
  describe; the header, `docs/system-surfaces.md` and the automation-platform
  RFC now say so.
- **A 4xx other than 429 from InfluxDB is dropped and logged** (#671).

### Fixed

- **Providers shut down cleanly when there is more than one** (#676).
- **A write to a closed provider fails that call instead of ending the
  runtime** (#675).
- **`value_double` round-trips exactly** (#674).
- **A torn last journal line no longer swallows the next record** (#672, Fixes
  #639): an append starts a new line when the file doesn't end in one, so an
  abandoned run's recovery record, or a newly opened run, is kept.
- **install.sh's observability step retries a failed `apt-get update`** (#670,
  Fixes #669), up to 3 times over about a minute, and names apt's first `E:`
  line when it gives up.

### CI

- The install walk runs against runtime 0.6.0 (#667), and its workbench
  scenario keeps install.sh's output in the artifact (#668).
- The docs check runs when `doc-schemas.json` or a schema changes (#683).

## [0.6.0] - 2026-10-29

0.6.0 is a minor release because existing installs behave differently: when
an observed stop enters FAULT, which provider-declared function policies are
enforced, and how a behaviour tree's terminal result is logged
(`docs/dependencies.md`: before 1.0, a change that makes an existing install
behave differently bumps MINOR). The runtime-config schema gains
`automation.on_terminal`, and the runtime-http capabilities gain three
function fields.

Upgrade notes for operators:

- **An observed stop enters FAULT even when the latch is already held** (#662,
  Fixes #547). With `safety.stop_chain` on an automation machine, a status
  going `in_force` or `unknown` used to latch and enter FAULT only if the latch
  was not already engaged: after a software stop and FAULT -> MANUAL, or with a
  latch restored from file, the runtime stayed in MANUAL or IDLE. It now enters
  FAULT either way, leaving an existing latch's time and file as they are, and
  logs `E-stop latch already held: <reason>`. A stop status that holds at
  startup also ends in FAULT once automation starts, where the runtime used to
  come up IDLE and latched.
- **With `http.enabled: false`, a software stop on an automation machine enters
  FAULT** (#662). The safe-state controller got its mode manager only when the
  HTTP server was on, so with it off a stop latched and drove the ladder but
  stayed out of FAULT.
- **A provider's `FunctionPolicy` `allowed_modes` and `min_interval_ms` are
  enforced** (#663, Fixes #566), as the protocol requires of the runtime.
  - A call in a mode outside a non-empty `allowed_modes` is refused with
    `FAILED_PRECONDITION`, as in
    `calibrate is allowed only in MANUAL; the runtime is in AUTO`, whether it is
    manual, from the behaviour tree or from a mode-transition hook. A runtime
    without automation judges calls as MANUAL.
  - A call sooner than `min_interval_ms` after the last one dispatched to that
    function is refused with the time remaining, not deferred.
  - Software-stop and startup safe-state calls are exempt from both.
  - `requires_lease` is not enforced: discovery logs a WARN naming each
    function that sets it.
  - `GET /v0/devices/{provider}/{device}/capabilities` carries all three
    fields.

  No shipped provider sets these fields yet, so nothing changes until one
  does.
- **`automation.on_terminal: repeat | stop`** (#665, Fixes #596). `repeat`, the
  default, keeps today's behaviour: a root that returned `SUCCESS` or
  `FAILURE` is ticked again from the root. `stop` ends the run instead: no more
  ticks or calls, and `execution_status` reports `completed` or `failed` until
  AUTO is entered again or a definition is loaded. Under either, the terminal
  result is logged once per change, where a tree returning `SUCCESS` every tick
  logged "BT completed successfully" on every tick.

### Added

- **`automation.on_terminal`** (#665, Fixes #596), as the upgrade notes
  describe, in the runtime-config schema, the OpenAPI `execution_status` text
  and D9 of `docs/rfcs/automation-platform.md`.
- **The `allowed_modes`, `min_interval_ms` and `requires_lease` function
  fields** in the capabilities JSON and the OpenAPI `FunctionSpec` (#663).

### Changed

- **`allowed_modes` and `min_interval_ms` are enforced in `CallRouter`** (#663,
  Fixes #566), as the upgrade notes describe; `docs/automation.md` has a
  section on them.
- **A terminal behaviour-tree result is logged once per change** (#665).

### Fixed

- **An observed stop enters FAULT when the latch is held, and at startup**
  (#662, Fixes #547), and `hold()`'s doc comment says so.
- **A software stop enters FAULT with the HTTP server off** (#662).

### CI

- The install walk runs against runtime 0.5.0 (#664).

## [0.5.0] - 2026-10-29

0.5.0 is a minor release because existing installs behave differently: what a
software stop logs, and how Grafana and its Signal History dashboard behave
(`docs/dependencies.md`: before 1.0, a change that makes an existing install
behave differently bumps MINOR). It is the first release since 0.2.0 whose
runtime code changed.

Upgrade notes for operators:

- **A software stop on a variant with FAULT-entry hooks logs one INFO line
  where it logged ERRORs** (#657, Fixes #648). Under a safe-state rung the
  e-stop latch refuses a FAULT entry's actuating hook calls, by design. The
  runtime no longer makes those calls, and logs per transition:

  ```text
  [Runtime] 4 before-transition hook call(s) (MANUAL -> FAULT) not made: the e-stop latch holds actuation (POST /v0/estop/clear releases it)
  ```

  Before, each call logged a `[CallRouter]` WARN and a `[Runtime]` ERROR
  ("Before-transition hook failed ... Actuation latched by e-stop"). Nothing
  actuates differently: those calls never reached a provider. READ-only hook
  calls still run, and under rung `none` the hooks run as before. An alert or
  log filter keyed on those ERRORs stops seeing them.
- **Grafana no longer updates its preinstalled plugins at startup** (#660,
  Fixes #659). Its `preinstall_auto_update` setting defaults to on, and while the InfluxDB
  datasource is replaced, dashboard queries fail ("Data source became
  unavailable", then "Plugin not registered"). `install.sh --with-observability`
  writes `GF_PLUGINS_PREINSTALL_AUTO_UPDATE=false` into Grafana's systemd
  drop-in, and the Docker Compose stack sets it too. An existing host picks it
  up when install.sh runs again with `--with-observability`, which rewrites
  the drop-in and restarts Grafana. Plugin versions then change only with the
  `grafana` package or the Grafana image.
- **Signal History refreshes every 30 s by default, not every 5 s** (#658,
  Fixes #647). On a Raspberry Pi its Double panel never drew at 5 s: each
  refresh cancelled the load before it. A viewer can still pick a shorter
  refresh in Grafana.

### Changed

- **FAULT-entry hook calls the e-stop latch would refuse are not made** (#657,
  Fixes #648), as the upgrade notes describe. `CallRouter::latch_refuses`
  answers the latch check without dispatching.
- **Signal History's default refresh is 30 s** (#658, Fixes #647).

### Fixed

- **The observability stack keeps the plugins its Grafana shipped with**
  (#660, Fixes #659), so the dashboard query suite no longer fails when a
  plugin update lands mid-run.

### CI

- The install walk runs against runtime 0.4.0 (#656).

## [0.4.0] - 2026-10-29

0.4.0 is a minor release because `install.sh --uninstall` behaves differently
on an existing host (`docs/dependencies.md`: before 1.0, a change that makes an
existing install behave differently bumps MINOR).

Upgrade notes for operators:

- **`--uninstall` refuses a prefix its units don't run from, and changes
  nothing** (#654, Fixes #544). It used to stop and delete every `anolis-*`
  unit and the secrets in `/etc/anolis` whatever the prefix. So
  `--prefix /opt/anolsi`, a typo for a live `/opt/anolis`, took that install's
  units and API token and reported success. It now reads which prefix
  `anolis-runtime.service` and `anolis-telemetry-export.service` run from, by
  their `WorkingDirectory` or the binary in `ExecStart`, and exits 1 when that
  isn't the prefix given, naming it:

  ```text
  uninstall: anolis-runtime.service runs the install at /opt/anolis, not /opt/anolsi; nothing was changed (to remove that install: --uninstall --prefix /opt/anolis)
  ```

  A prefix that no longer exists but that its units run from is still
  uninstalled.
- **It also refuses when it can't tell which prefix one of those units runs
  from**, again with nothing changed.
- **It removes only the units install.sh writes.** Another `anolis-*.service`,
  such as the workbench's `anolis-workbench.service`, is kept, and the
  uninstall prints the commands that remove it:

  ```text
  systemctl disable --now anolis-workbench.service && rm /etc/systemd/system/anolis-workbench.service && systemctl daemon-reload
  ```

  Earlier releases deleted it with the runtime's units.

### Changed

- **`--uninstall` acts only on the install at its prefix** (#654, Fixes #544),
  as the upgrade notes describe.

### Docs

- **`docs/safety.md` gives a runtime without automation its own startup
  sequence** (#653, Fixes #569). With `automation.enabled: false`, the default,
  there is no IDLE gate: `/v0/mode` answers 503 and a manual `POST /v0/call` is
  dispatched from the first request. The sequence says to engage the software
  stop if nothing may move until an operator is ready. A `CallRouter` test pins
  that behaviour.

## [0.3.0] - 2026-10-28

0.3.0 is a minor release because one kind of kept config resolves its data
directory somewhere else (`docs/dependencies.md`: before 1.0, a change that
makes an existing config behave differently bumps MINOR).

Upgrade notes for operators:

- **`install.sh` resolves a relative `runtime.data_dir` against the config's
  directory**, as the runtime does (#649). It used to write `<prefix>/<x>`.
  The case that changes: an upgrade keeps a `runtime.yaml` naming a relative
  `data_dir`, and neither place holds a latch file (`estop/*.json`) or a
  non-empty `runs/index.jsonl`, or both do. That config now gets
  `<prefix>/config/<x>`, where it used to get `<prefix>/<x>`. A kept config
  whose latch or run journal is only at `<prefix>/<x>` keeps it there. A variant
  installed fresh or with `--replace-config` also resolves against the config's
  directory, so `data_dir: ../anolis-data` gives `<prefix>/anolis-data`; before
  this release no anolis-projects variant named a relative `data_dir`. An absent
  key still becomes `<prefix>/anolis-data`, and an absolute one is left alone.
- **A failed install's banner reads "Anolis installed, but NOT ready — see the
  end of this summary"** (#651), where it read "Anolis installation complete".
  A script that greps install output for "Anolis installation complete" now
  matches only installs that exit 0. The exit code and the last line, which
  says why the install did NOT succeed, are unchanged.

### Changed

- **A relative `runtime.data_dir` keeps its place through a hand copy of a
  variant** (#649, part of #643). A project variant can name
  `data_dir: ../anolis-data`, which is `<prefix>/anolis-data` installed and
  stays so when the variant is copied over `runtime.yaml`, as bioreactor-v1's
  quickstart does to activate automation.
- **An install that exits 1 says it is not ready** (#651, Fixes #616), above
  the summary, which keeps the prefix, profile, port and token path.

### Docs

- Command-line and telemetry references, `docs/cli.md` and
  `docs/observability.md` (#630).
- The concept page, three RFCs and the observability README rewritten to the
  docs style guide (#638). `docs/configuration-schema.md` now says any stop-chain
  reading whose quality is not `OK` is `unknown`, as it has been since #604.
- Stale workbench version notes dropped, and the simulator quickstart says to
  start the runtime from inside `anolis-try` (#644).
- The pins row in `docs/system-surfaces.md` names no version numbers (#642).
- The YAML and JSON examples in the docs are validated against their schemas
  (#645).

### CI

- The install walk runs against runtime 0.2.0, upgrading from 0.1.57, and
  covers an install whose safe state fails at run time and one that cannot be
  planned (#641).
- The install walk's `handcopy` scenario copies a variant over `runtime.yaml`,
  restarts, and requires the same data directory and a latched e-stop that
  still holds (#649).
- anolishq/.github's shared workflows move to v2.6 (#650).

## [0.2.0] - 2026-10-27

0.2.0 is a minor release because existing configs and installs behave
differently: the CORS default, where a default data directory lives, and an
install over a safe state that cannot be planned.

Upgrade notes for operators:

- **Browser origins are refused unless listed** (#627). A browser UI that
  calls the runtime directly from another origin must list that origin in
  `http.cors_allowed_origins`; with no list, or `[]`, no browser origin may
  call it, where it used to mean `*`. Configs that set an explicit list, like
  bioreactor-v1's `http://localhost:3000` entries, keep working. A `POST` with
  a non-JSON body, such as `curl -d` without `-H 'Content-Type:
  application/json'`, now gets 415, and with auth off, a request whose `Host`
  names something other than this machine gets 403.
- **A default or relative `runtime.data_dir` is beside the config file**
  (#631). A config run by hand or from a script without `data_dir` now keeps
  its e-stop latch and run journal beside the config file, not in the working
  directory; data a previous run left in `<cwd>/anolis-data` stays there, so
  set `runtime.data_dir` to that path to keep using it. Installed hosts need
  nothing: the upgrade names the directory they already use.
- **An install over a config whose safe state cannot be planned now fails;
  fix the config, then re-run** (#634). "Cannot be planned" means the runtime
  refuses the call before it reaches the devices: the zero rung cannot build a
  call's arguments, the call names a device or function that does not exist,
  or an argument is out of range. It would fail the same way on every e-stop.
  A safe state that ran and did not take stays a warning, and against a
  runtime before 0.2.0, which gives no cause, every failure stays a warning.
- **`EstopAction` gains `cause`** (#633), on each failed call in
  `startup_safe_state.failed_actions[]` and the `POST /v0/estop` `actions[]`.
  The OpenAPI object declares `additionalProperties: false`, so a client
  validating responses against an older copy of the contract rejects the new
  field.
- **Providers run in their own process group** (a job object on Windows,
  #632). Stopping or restarting a provider also ends any process it started,
  such as a wrapper script's worker. A provider no longer receives terminal
  signals such as Ctrl+C from the runtime's terminal; the runtime catches them
  and stops each provider by closing its stdin, as before.

### Changed

- **A browser page's requests are refused before routing** (#627, Fixes
  #540): a `POST` to `/v0/` from an origin not in `cors_allowed_origins`
  (403), a `POST` with a body that is not JSON (415), and, with auth off, a
  foreign `Host` (403). `cors_allowed_origins` defaults to none.
- **A default or relative `runtime.data_dir` resolves against the config
  file's directory** (#631, Fixes #562), and the resolved path is logged at
  startup as `[Config] Data dir:`. `install.sh` writes `<prefix>/anolis-data`
  into the installed config, and adds it on upgrade to a kept config that has
  none.
- **An install whose safe state cannot be planned exits 1** (#634, part of
  #616), naming `safety.safe_state` and the first refused call.
- **A provider restart runs beside the main loop** (#628, Fixes #557). The
  stop chain, the other providers' supervision and a shutdown signal are
  handled while a provider comes back up, which can take a minute at the
  default timeouts. Restarts still run one at a time.

### Added

- **Every call a provider receives is logged at INFO** (#625, Fixes #623),
  with its arguments, its source (`HTTP`, `behavior tree`, `hook` or
  `safe-state`) and its result. A behaviour tree's repeated call is logged
  only when it changes.
- **A failed safe-state call says whether the config was the cause** (#633,
  part of #616): `cause` is `config` or `run`.

### Fixed

- **A stopped provider takes everything it started with it** (#632, Fixes
  #563), and a restart does not start the replacement while any of the old
  one is still running: that attempt fails, and supervision retries.
- **A data race on a provider's pid** between a provider being stopped and
  another thread asking whether it was running (HTTP, polling), which #628
  made reachable from the main loop too.

### Docs

- A quickstart that runs the released runtime with the simulator (#629).
- `docs/safety.md` says a supervised restart of anolis-provider-bread 0.10.0
  turns its outputs off unless its config sets
  `provider.outputs_on_startup: leave` (#635).
- The automation RFC names its review without linking it (#626).

### CI

- The install walk runs against runtime 0.1.57, upgrading from 0.1.56 (#624).

## [0.1.57] - 2026-10-27

Upgrade notes for operators:

- **The bundled Grafana dashboards are fixed.** `install.sh
  --with-observability` provisions them from the release asset of the
  installed runtime's version on every run, so the next install of 0.1.57
  with observability replaces the broken copies. A host that stays on 0.1.55
  or 0.1.56 keeps them. `install.sh`, the runtime and the schemas are
  unchanged since 0.1.56.

### Fixed

- **Signal History's value panels and Device Health's Quality Distribution
  Over Time no longer fail with "range found duplicate table with key"**
  (#620, #621). Each unions a series' last value before the range with its
  changes inside it, then called `range()` before grouping by series. A
  series with rows on both sides of the range start had one table in each
  stream, differing only in `_start` and `_stop`; `range()` gave both the
  same bounds, and Flux refused the result. That is every series once
  telemetry has run longer than the range, so the panels drew nothing. The
  union is now grouped by series before `range()`, and the held value
  (step-and-hold) is unchanged. The prototype process dashboard generator
  (`tools/process-dashboard/`) builds the same query and is fixed the same
  way.

### CI

- The install walk runs against runtime 0.1.56, upgrading from 0.1.55
  (#619).

## [0.1.56] - 2026-10-27

Upgrade notes for operators:

- **The stop chain's status signal is judged only with `QUALITY_OK`.** A
  status reading with any other quality, including one whose provider never
  sets quality, now reads `unknown`, and `unknown` holds the stop, where a
  fresh `false` used to read `clear`. A provider that leaves quality unset on
  its stop-chain status signal now keeps the stop held. provider-sim's and
  bread's signal helpers set `QUALITY_OK` by default.
- **`install.sh` no longer writes `config/providers/`.** Provider configs ship
  once, under `projects/<profile>/config/`, where the rendered runtime
  configs point. Nothing on a machine is deleted: an existing
  `<prefix>/config/providers/` stays, and a `runtime.yaml` kept from before
  #144 that still points into it keeps working.
- **`--replace-config` replaces an existing `runtime.yaml`.** It is moved to
  `runtime.yaml.replaced-<UTC time>` beside it first, and the variant must
  still be inert. Without the flag, an existing `runtime.yaml` is preserved,
  as before.
- **A bundle's `install.sh` is verified against the release's `SHA256SUMS`.**
  A mismatch stops the stage, as a mismatched tarball does.

### Added

- **`install.sh --replace-config`** (#470, #610). It is the supported way to
  put a project's config onto a machine that already has one, which before
  meant deleting `runtime.yaml` and re-running. The log names the file and
  where the previous one went.
- **A startup warning for a device watchdog under twice the poll
  interval** (#599, #602). Polling is what feeds each device's firmware
  watchdog once the runtime runs, so a watchdog shorter than two poll
  intervals can trip on one late poll and leave every SET refused. The
  runtime now logs, per device, `preflight: <device> reports
  watchdog_timeout_ms=<n>, under twice polling.interval_ms=<m>, …` with
  the values that would fix it. It stays a warning: a trip is the safe
  direction.

### Fixed

- **A stripped host's InfluxDB tokens are minted by `install.sh` itself**
  (#514, #517). On a host whose credentials an uninstall before 0.1.54
  deleted, while it kept the InfluxDB instance, `--with-observability`
  refused and printed root commands to run. It now mints the write and read
  tokens with the influx CLI config the uninstall keeps, records them in
  `observability.env` (0600 root), and logs `observability: minted new
  write and read tokens`. Without that config it refuses and names it.
- **The bundled `install.sh` is verified against `SHA256SUMS`** (#508,
  #510). Stage checked only the tarballs, so the `install.sh` a bundle
  carries for a later `--local` was used unverified. A mismatch is now
  fatal, and so is a `SHA256SUMS` that can't be fetched for a reason other
  than a 404. A release before 0.1.50, whose `SHA256SUMS` has no
  `install.sh` line, warns and keeps it.
- **Every FAULT before-hook call is issued when one fails** (#542, #601). A
  failed call with `fail_on_error` (the default) stopped that hook and every
  later matching hook, though FAULT commits whatever the hooks return. On a
  transition into FAULT, a failed call is now logged at error level and the
  rest are still issued. Other target modes keep the veto.
- **A FAULT request always ends in FAULT** (#528, #603). A transition that
  committed while FAULT's before-callbacks ran made the request fail with
  "Concurrent mode change detected", so the runtime could stay in MANUAL or
  AUTO with the FAULT hooks, event and journal entry skipped. FAULT now
  commits from whatever mode it finds.
- **The stop chain reads `unknown` unless the reading is `QUALITY_OK`**
  (#526, #604). Its status signal was judged by age alone, so a fresh
  `false` with unset or unrecognised quality read `clear`, the only state
  that lets `POST /v0/estop/clear` through. Now any other quality reads
  `unknown`, and the reason names it. A `reports_estop` device's `estop` is
  trusted the same way. A `power_loss` device still counts as answering
  with any fresh reading, whatever its quality.
- **`install.sh`'s output and leftovers** (#515, #613):
  - it no longer writes `config/providers/`, which nothing read;
  - apt runs with `DEBIAN_FRONTEND=noninteractive`, so
    `--with-observability` no longer prints debconf's `unable to initialize
    frontend`;
  - a service user kept from an earlier install is named with its groups,
    which `install.sh` neither grants nor revokes, with the command to drop
    one.
- **The Grafana password note after a failed reset** reads
  `(admin password: unknown, its reset failed; see the warnings above)`,
  without nested parentheses (#611).

### Changed

- The observability dev stack's Grafana image is 13.3.0 (#609).
- Docs: statements the source contradicted are corrected across the
  site-published docs and the README (#600); references to non-public
  material are removed (#598); the HTTP doc links the OpenAPI spec and
  example manifest by URL (#615); the e-stop latch's comment says it
  refuses CONFIG calls too (#608).

### CI

- **The install walk** (#516, #614): `tests/e2e/install-walk.sh` runs this
  checkout's `install.sh` as root on fresh x86_64 and arm64 runners against
  a simulator project, for a fresh install with a variant switch, restart,
  uninstall and reinstall; recovery after v0.1.53's uninstall; an upgrade
  from the previous release; and the workbench's `anolis-provision
  install`. It runs on pull requests that touch `install.sh` or the walk,
  and on release tags.
- The release lanes' runtime builds are uploaded as artifacts (#607); the
  flaky stop-chain expectation tests are fixed (#605, #606); routine
  dependency maintenance.

## [0.1.55] - 2026-10-26

Upgrade notes for operators:

- **`safety.stop_chain.devices` entries can declare what a device does when
  the stop is in force.** An entry is a bare handle, as before, or
  `{handle, expect}` with `expect` `power_loss` (the stop cuts power to the
  device's controller, so it goes silent) or `reports_estop` (it stays on the
  bus and reports its `estop` signal true). Existing configs load unchanged.
- **A handle listed twice in `safety.stop_chain.devices` now fails config
  load**, in either form, naming both positions. Before, it loaded, and with
  two mappings the last `expect` won silently.
- **`GET /v0/runtime/status`'s `stop_chain` gains two required fields,
  `expectations` and `mismatches`, and `/v0/events` a `stop_chain_mismatch`
  event.** An older workbench keeps working: it reads `stop_chain` loosely
  and does not validate runtime responses against the OpenAPI file. Only a
  client that does needs the new file.

The config schema accepts the new `devices` entry form. `install.sh` is
unchanged since 0.1.54.

### Added

- **Per-device stop expectations and the "Stop not reaching *device*"
  alarm** (#484, #503), increment 4 of
  [rfcs/stop-chain.md](docs/rfcs/stop-chain.md). Once the stop has been in
  force for the status signal's stale bound, max(1000 ms, 3 x
  `polling.interval_ms`), a `power_loss` device that still has a fresh
  reading, or a `reports_estop` device whose `estop` reads a fresh `false`,
  raises it:
  - a WARN, `Stop not reaching <device> (expect <expect>): <reason>`;
  - an entry in `stop_chain.mismatches` (`device`, `expect`, `reason`,
    `since_epoch_ms`);
  - a `stop_chain_mismatch` event with `raised: true`.

  It is resolved, with an event with `raised: false`, when the device
  behaves as expected or the stop leaves `in_force`. The hold is the same
  either way. A `reports_estop` device with no fresh `estop` reading does
  not raise it, and a device with no `expect` never does.
  `stop_chain.expectations` lists each declared `expect` by handle. At
  startup the runtime warns about a `reports_estop` device with no polled
  bool `estop` signal, or a `power_loss` device with no polled signal.

### Changed

- CI (#501): the TSan lane's vcpkg packages get their own cache key, the
  compatibility lane downloads the pinned anolis-provider-sim release and
  checks it against its `SHA256SUMS` instead of building it, and changes
  only to `tools/`, `tests/shell/`, `observability/` or
  `tests/observability/` skip the build lanes. The provider-sim pin must
  name a release tag (`docs/dependencies.md`).
- `docs/configuration-schema.md` names a handle listed twice among the
  `safety.stop_chain` config-load failures (#505).

## [0.1.54] - 2026-10-26

Upgrade notes for operators:

- **`--uninstall` now keeps the InfluxDB credentials while observability is
  provisioned.** `/etc/anolis/observability.env` and
  `/etc/anolis/observability-grafana.env` stay on disk with the InfluxDB
  instance they belong to. The uninstall reports them as kept and adds them
  to the `rm -f` purge line it prints. Without observability they are
  removed as before. A later `--with-observability` install reuses them.
- **A host uninstalled by an earlier `install.sh` is refused with a recovery
  step.** If InfluxDB is already set up but neither file is there,
  observability is not provisioned, and the install prints the
  `sudo -H influx` commands that mint the two tokens and where to write
  them.
- **The latest-state dashboard panels show only a runtime that reports
  health in the range.** Signal tables, quality counts and the quality pie,
  and the seeds of the history charts, keep a runtime's signals only if it
  wrote an `anolis_provider_health` row in the range. They are empty with
  `telemetry.health_interval_ms: 0` or before 0.1.37, and can be for a range
  shorter than the health interval (15 s by default).

No config schema, provider protocol or HTTP API change. `install.sh`
changed, so `SHA256SUMS` lists a new hash for it.

### Added

- **`runtime_name` on `mode_change` and `parameter_change` telemetry rows**
  (#478, #493), so the dashboards' annotations filter by runtime. These
  measurements are outside the locked telemetry contract. Rows written by
  older runtimes have no `runtime_name`, and the annotations show them under
  every runtime.
- **A query suite for the bundled Grafana dashboards** (#478, #493). It runs
  every panel, annotation and variable query through Grafana against
  InfluxDB loaded with fixture telemetry: `just test-observability` (needs
  Docker), and CI's `observability` job.

### Fixed

- **The bundled Grafana dashboards** (#478, #493). Each read the
  datasource's bucket and filters by runtime. Variable filters are anchored,
  so selecting signal `int` no longer also matches `uint`. Device Health
  counts the contract's qualities (OK, STALE, UNAVAILABLE, FAULT), its
  staleness gauge reads `staleness_ms`, and both quality-distribution panels
  run instead of failing. Latest-state panels look back 30 days for a
  signal's last change. History panels plot the last value per window, held
  between changes. System Overview counts providers, devices and signals seen
  in range by full identity. Signal History shows unsigned and string values.
  The mode and parameter annotations show their fields. IO Health counts a
  missing or excluded device per runtime.
- **A reinstall after `--uninstall` left Grafana and the runtime without an
  InfluxDB token** (#469, #495). The uninstall deleted the credentials of
  the instance it kept, so the reinstall found InfluxDB set up and could not
  mint new ones. It now reuses the kept tokens: the Grafana env file is
  restored, and a missing or blank `INFLUXDB_TOKEN` in `runtime.env` gets
  the write token. A token the operator set is left alone. A Grafana
  password the recovery path generates is written to `observability.env`,
  after that file is set to `0600 root:root`.

`tools/process-dashboard/` (#477, #496), a prototype dashboard generator, is
not in any release.

## [0.1.53] - 2026-10-25

Upgrade notes for operators:

- **The software stop's latch survives a restart.** A latch engaged by
  `POST /v0/estop` is now written to `<data_dir>/estop/` and restored at
  startup, so a redeploy or reboot comes back latched instead of accepting
  actuating calls. Clear it with `POST /v0/estop/clear` once the machine is
  safe. `GET /v0/runtime/status` says why a runtime started latched in
  `estop.restored_reason`. `data_dir` must be on persistent storage; installs
  already keep it under the install prefix.
- **AUTO is refused while the latch is engaged.** After a software stop,
  `FAULT -> MANUAL` still works, but `MANUAL -> AUTO` is refused with the
  reason until the latch is cleared.
- **A string argument outside a provider's `allowed_values` is refused.**
  `POST /v0/call`, safe-state hooks, mode-transition hooks and behaviour-tree
  calls get `INVALID_ARGUMENT`. Startup preflight reports a declared call
  that would be refused. Nothing changes until a provider sets the field;
  anolis-provider-bread 0.9.1 sets it for `set_mode`.

The config schema gains `safety.stop_chain`, which is optional. The OpenAPI
changes are additive except one: `POST /v0/estop/clear` can now answer `409`,
but only when `safety.stop_chain` is configured. anolis-workbench vendors this
OpenAPI file and needs a repin to pick them up.

### Added

- **`safety.stop_chain`: observing the emergency stop** (#283, #284, #483,
  #490), specified in
  [rfcs/stop-chain.md](docs/rfcs/stop-chain.md) (#480). It names a bool
  signal any provider publishes, `provider/device/signal`, such as a
  contactor's mirror contact, and the devices the stop removes power from.
  The runtime only observes it.
  - The status reads `in_force`, `clear` or `unknown`. A reading that is
    stale, missing or not a bool is `unknown`, never `clear`.
  - On `in_force` or `unknown` the latch is held without running the ladder,
    and an automation machine enters `FAULT`. No safe-state calls are sent.
  - The clear is refused, with `409` and the reason, until the stop reads
    `clear`. Releasing the stop never clears the latch.
  - A device in scope that goes unreachable while the stop is in force is
    reported as the stop, not as a device fault.
  - `safety.on_startup: safe_state` with the stop in force at boot skips the
    devices in scope, reported as `stop chain in force`, and still drives the
    rest.
  - `GET /v0/runtime/status` carries `stop_chain`, and a move is a
    `stop_chain_change` event on `/v0/events`.
  - Startup warns when the status device is in its own scope, is not
    registered, has actuating functions, or its signal has no poll hint and
    is never polled.
- **The software stop's latch is kept across restarts** (#482, #489), one
  file per machine under `data_dir`, keyed by the config file's directory, so
  a project's variants share it and separate projects do not. A present but
  unreadable or corrupt file starts the runtime latched.
- **`allowed_values` on string function arguments** (#467, #479). The
  capabilities response carries a provider's `ArgSpec.allowed_values`, the
  OpenAPI `ArgSpec` describes it, and a call outside it is refused with
  `INVALID_ARGUMENT` before it reaches the provider.

### Fixed

- **AUTO could be entered with the software stop still latched** (#481,
  #488). It is refused, with the reason, both before the transition's hooks
  run and again as AUTO commits, so a stop pressed mid-transition is caught.
- **The refuse-hookless gate could be passed by a provider restart during
  MANUAL -> AUTO** (#239, #486). The gate is checked again under the mode lock
  as AUTO commits, so a hookless actuating inventory committed while the
  transition's hooks run cannot land the runtime in AUTO.

## [0.1.52] - 2026-10-25

No runtime, config schema, provider protocol or HTTP API change. This
release changes `tools/install.sh` and the docs.

### Added

- **The install health check reports the startup safe state** (#471,
  #473). Every install restarts the runtime, and the restart runs
  `safety.on_startup` again, but `phase_health` printed only the
  version, the run registry and provider liveness. A deploy whose
  startup safe state failed on every call still read as healthy.
  `phase_health` now reads `startup_safe_state` from the runtime status
  it already fetches, and prints one line after the run-registry line:

  - `✓ health: startup safe state achieved (<software_safe_state>)`,
    for example `(hooks)`;
  - `⚠ health: startup safe state NOT achieved`, then one indented line
    per failed call (`<device> <function>: <error>`) and per skipped
    call (`<device> <function>: skipped, <reason>`);
  - `→ health: startup safe state not run (on_startup: leave)`, or
    `(no software safe state declared)`;
  - nothing, for a runtime before 0.1.45, which does not report it.

  Not achieved is a warning, not a failed install. The line is parsed
  with jq, or python3 when jq is absent, and left out when neither is
  installed.

### Changed

- `docs/safety.md` puts the advice on when to set `leave` back with
  the `on_startup` options, and says a run where every call was
  skipped for an absent device logs a WARN, not an ERROR (#464).

## [0.1.51] - 2026-10-23

No config schema or provider protocol change. `GET /v0/runtime/status`
gains an optional field, `startup_safe_state.skipped_actions` (#461).

### Fixed

- **The runtime starts with a provider that lists no devices** (#461,
  #462). A provider that answered and listed no devices failed
  discovery with `No devices could be registered from provider <id>`,
  and the runtime exited. Under systemd it restarted about every 8 s,
  for as long as the bus stayed unpowered. That provider is now up with
  no devices registered, and logs a WARN. A listing where no device
  describes is treated the same way. Discovery fails only when
  `ListDevices` itself fails, and a supervised restart that lists
  nothing replaces the old inventory with none.
- **The startup safe state skips a device that is absent, rather than
  failing it** (#461, #462). A call is skipped when its provider
  reports the device `UNREACHABLE` and the device is not registered.
  It is listed in the new `startup_safe_state.skipped_actions`
  (`device_handle`, `function`, `reason`) and logged at WARN, and
  preflight warns `absent at startup` for it. Any other call that
  cannot be resolved still fails, at ERROR. `safe_state_achieved` is
  `true` only when at least one call ran and every call that ran
  succeeded, so a boot with every device absent reports `false` with no
  failed actions, logged at WARN. A device that appears later is not
  driven.

### Changed

- `docs/http-api.md` describes bearer authentication, and says the
  device-loss latch engages on a device the provider reports
  unreachable, not on a provider that hangs, crashes or restarts (#457,
  #459).
- The device-loss integration check waits 1 s after the first
  re-assert before counting them, so a late second one fails it. With
  a provider-sim too old to inject a device loss, it now says that it
  needs v0.2.8, the pinned version. `.ci/compatibility-matrix.yml`,
  which nothing read, is removed; `.ci/dependency-pins.yml` is the one
  record of the pins (#433, #458).

## [0.1.50] - 2026-10-22

No config schema or provider protocol change. The HTTP API changes in
description text only (#453).

### Security

- **The release's `SHA256SUMS` covers `install.sh` and the manifest
  JSON files** (#446, #449). Until now it listed only the `.tar.gz`
  assets, so a downloaded `install.sh`, which runs as root, had nothing
  to be checked against. The release job writes `SHA256SUMS` with
  `tools/release-sha256sums.sh` and fails unless the published file has
  a line for `install.sh` and for every other asset it covers.
  `docs/deploy/install-sh.md` shows how to check `install.sh` before
  running it:
  `sha256sum -c --ignore-missing SHA256SUMS && sudo bash install.sh`.
  anolis-workbench's next release checks it before running it
  (anolishq/anolis-workbench#530).
- **`install.sh` refuses a release tarball with no `SHA256SUMS` entry**
  (#408, #445). A release with no `SHA256SUMS`, or no line for the
  tarball, used to warn and install it unverified. It now stops, as it
  already did on a mismatch. The observability phase skips in those
  cases and keeps the previous dashboards. Every release the current
  pins name publishes complete sums.
- **`install.sh` brings pip and certifi current in the telemetry-export
  venv** (#451). The venv is created once and kept on later installs,
  so it kept the certifi it was created with. certifi before 2026.10.19
  trusts emSign roots whose intermediate CA keys are compromised (the
  eMudhra incident), and pip before 26.2.2 always loads its own copy of
  that bundle. Each install now runs
  `pip install --upgrade 'pip>=26.2.2' 'certifi>=2026.10.19'` in the
  venv, and warns rather than failing when that can't run, offline for
  instance. An existing install picks the fix up on its next
  `install.sh` run.

### Fixed

- **A device that returns after a provider restart dropped it gets its
  safe-state re-assert** (#424, #447). A device latched by the
  device-loss latch, then left out of a provider's smaller inventory on
  restart, then registered again under the same id came back without a
  return edge, and kept running whatever it was last commanded. The
  state cache now keeps the device's loss record across that rebuild,
  so its first good read is a return and the re-assert runs. The latch
  entry is kept meanwhile: the device stays blocked to automation until
  MANUAL → AUTO, as any device latched after a loss is.
- **`/v0/providers/health` no longer waits out a stuck provider call**
  (#411, #455). One request is in flight per provider session, and
  health queued behind a stuck one for up to the provider's
  `timeout_ms` (5 s by default). It now waits at most 250 ms for the
  session. When another call still holds it, that provider's `reported`
  is `null`, and its state and device rows still come from availability
  and the state cache. The InfluxDB health rows take the same path. A
  provider that hangs while its session is idle still holds health for
  up to `timeout_ms` (#454).
- **The OpenAPI descriptions of a parameter's `source` and of
  `execution_status` say what the runtime does** (#387, #453). `source`
  reads `operator` from the first change on, even after a later POST
  sets the value back to its default. A tick exception reports
  `failed` (`execution_reason` `terminal_failure`), moves the runtime
  to FAULT and emits `automation_fault`.

### Changed

- CI, the extended checks and the soak run on ubuntu-26.04 (#444). The
  coverage job's `lcov` calls continue past lcov 2.4's `inconsistent`
  records (#448, #452). Release builds stay on ubuntu-24.04, so the
  binaries need the same glibc as before.
- certifi 2026.10.19 in `uv.lock` (#450). It serves the Python test and
  tooling environment; the runtime doesn't use it.

## [0.1.49] - 2026-10-19

No config schema, HTTP API or provider protocol change.

### Fixed

- **`install.sh` warns when the `runtime.yaml` it keeps points a
  provider at another project's config** (#439, #440). Reinstalling
  with a different project keeps the existing `runtime.yaml`, so its
  providers went on reading `--config` files from the old project's
  directory under `projects/`. That directory stays on disk, so they
  started without an error and on the old configs. For each such
  provider the install now prints a `⚠` line naming the provider, the
  file it reads, its project, and the path in the new project. The
  kept file is not changed. anolis-workbench's `anolis-provision`
  shows these lines in its install summary
  (anolishq/anolis-workbench#521).

## [0.1.48] - 2026-10-18

Upgrade notes for operators:

- **With `http.auth_enabled: true`, requests from the device itself now
  need the API token.** `http.auth_exempt_loopback` defaults to `false`.
  After upgrading, a local script or service that calls the API without
  the token gets `401`. Send `Authorization: Bearer <token>` (the token
  `install.sh` generated is in `/etc/anolis/runtime.env`), or set
  `http.auth_exempt_loopback: true` knowingly: it trusts every local
  process and every local service that forwards requests from the
  network (see `docs/security.md`). A config that already sets it to
  `true` behaves as before, and the runtime now logs a warning at
  startup for it. With auth off, nothing changes.
- **Upgrade anolis-workbench first, to anolis-workbench 0.18.0 or
  later.** On a device with auth on, a workbench that does not send
  the token gets `401` from this runtime: its Operate proxy on the
  device, and a workbench session that reaches the runtime over an ssh
  tunnel. anolis-workbench 0.18.0 or later sends the token, and works
  against this runtime and earlier ones.

### Security

- `http.auth_exempt_loopback` defaults to `false`. With it on, any
  request from the runtime's host skipped the token, including one a
  local proxy forwarded for a remote client, so a service on the same
  host that proxies to the runtime exposed the whole API to its own
  clients without the token. The exemption is now opt-in, and the
  runtime logs a WARN at startup when it is on with auth enabled.
  `install.sh`'s health checks send the token from `runtime.env` (or
  `http.auth_token`), passed to curl on stdin rather than in its
  arguments. `docs/security.md` states what the exemption trusts.

### Fixed

- `install.sh` reads `runtime.env` the way systemd does when it looks for
  the token: the last `ANOLIS_API_TOKEN=` line wins, and surrounding
  double quotes and a trailing CR are dropped.
- `install.sh --uninstall` also removes `/etc/anolis/workbench-runtime-token`,
  the copy of the API token anolis-workbench 0.18.0's service keeps, so
  `/etc/anolis` no longer outlives the uninstall because of it.

### Changed

- The integration suite and the compatibility lane pin anolis-provider-sim
  v0.2.8, up from v0.1.6. v0.2.8 adds the `inject_device_unreachable` chaos
  function, which a Runtime-level test of the device-loss latch needs (#429).

## [0.1.47] - 2026-10-17

Upgrade notes for operators:

- **A call to a device that was latched after a loss no longer hangs
  the runtime.** On 0.1.45 and 0.1.46, when a call's post-call read was
  a latched device's first good read since a transport loss, the
  safe-state re-assert ran on the calling thread and blocked on the
  provider lock that thread held. That call never returned, and every
  later call to the provider blocked behind it, e-stop ladder calls
  included (#427, #428). Manual `POST /v0/call`, the e-stop ladder,
  mode-transition hooks and non-actuating tree calls could all trigger
  it. The latch is always on, so this applies to every machine.
- **A transport loss that reaches the latch late now latches the
  device and re-asserts its safe state** (#403, #426). Before, a loss
  edge overtaken by the device's return could latch a device the cache
  said had answered, with no re-assert to follow. Actuating tree calls
  to that device stay refused until MANUAL -> AUTO, as for any latched
  device.

No config schema or provider protocol change. `GET /v0/events` gains
`device_removed`; the OpenAPI change is to a description string only.

### Added

- **`device_removed` events** (#402, #421). When a supervised restart
  republishes a provider's inventory without a device, the state cache
  emits `{provider_id, device_id, timestamp_ms}` for it. The
  `provider_id` and `device_id` filters on `GET /v0/events` apply.

### Fixed

- The safe-state re-assert after a device returns runs on its own
  worker thread and never while a provider lock is held (#427, #428).
  See the upgrade notes.
- Reachability edges carry a sequence number, and the device-loss latch
  applies them in the order the state cache decided them (#403, #426).
  A stale return is dropped; a stale transport loss still latches.
- A mode change made on the automation tick thread is journalled after
  the fault that caused it, and the tick thread no longer writes the
  run journal itself (#282, #422).
- `install.sh --uninstall` removes a run journal that holds only empty
  directories with the prefix, and keeps it if the emptiness check
  fails. The printed full purge names `influxdata-archive-keyring` and
  `/var/log/grafana` (#328, #423).

### Removed

- `provider_health_change` from the `GET /v0/events` documentation and
  the event types (#400, #419). Nothing ever emitted it, so the stream
  is unchanged. Provider health is read from `GET /v0/providers/health`.

## [0.1.46] - 2026-10-17

Upgrade notes for operators:

- **An e-stop with no software safe state now runs the `-> FAULT`
  hooks.** On a machine with no `safety.safe_state`, `POST /v0/estop`
  used to leave every output as it was, because the latch refused the
  FAULT-entry hooks. Those hooks now run on the e-stop's own entry into
  FAULT, so outputs they drive will move when the stop is pressed (#251,
  #401). With any safe-state rung declared, nothing changes.
- **`install.sh` checks release tarballs against the release's
  `SHA256SUMS`** (#408). A hash mismatch stops the install. A failure to
  fetch `SHA256SUMS` other than a 404 (a dropped connection, DNS, a
  timeout, a 5xx) also stops it, where it used to continue. A release
  with no `SHA256SUMS`, or with no line for the tarball, still installs
  with a warning.

No OpenAPI, config schema or provider protocol change.

### Added

- **`install.sh --keep-hostname`** (#327, #396) skips the host rename to
  `anolis-<serial>` and leaves `/etc/hosts` alone. The default is
  unchanged.
- **`device_availability` events** (#300, #399). `GET /v0/events`
  documented the event but nothing emitted it. The state cache now emits
  `available: false` when a read fails or a provider goes unavailable,
  and `available: true` on the first successful read after a loss.

### Changed

- **`POST /v0/estop` with rung `none` runs the FAULT-entry hooks**
  (#251, #401). See the upgrade notes. `safe_state_achieved` stays
  `false` and `actions` stays `[]`, since the ladder itself ran nothing.
  In IDLE, `before_transition` hooks are still refused by the IDLE gate
  and `after_transition` hooks run.
- **`install.sh` verifies downloaded tarballs** (#408): the runtime and
  provider tarballs (#415) and the observability tarball (#416) against
  the release's `SHA256SUMS`, failing closed on any fetch failure but a
  404 (#417). A failed observability check leaves the installed
  dashboards as they were and the install goes on.
- `install.sh --arch` accepts `aarch64` as `arm64` and rejects anything
  but `arm64` and `x86_64` before downloading (#407, #413).

### Fixed

- `GET /v0/providers/health` reads a provider's availability and the
  clock after `get_health()` returns, so a provider lost while the call
  waited is reported `UNAVAILABLE`, not `AVAILABLE` with its devices
  `OK` (part of #411, #414). The endpoint can still wait behind a stuck
  provider call.
- A tick exception halts the behaviour tree, so FAULT -> MANUAL -> AUTO
  no longer rethrows at the child that threw, and a new run starts with
  a clean stall count and last error (part of #387, #405).
- `install.sh --local` on a bundle with no `manifest.json` names the
  path and exits 1, instead of exiting 2 with no message (#385, #397).
- The health phase skips providers with an empty id, and tells an
  endpoint that did not answer from one that answered with an unreadable
  body (#388, #398).
- HTTP unit tests bind port 0, so `ctest -j` no longer collides on ports
  (#393, #404).

### Docs and CI

- What a provider release must provide for `install.sh` (part of #242,
  #406); the pinned clang-tidy 18.1.8 in CONTRIBUTING (#410, #412).
- The docs check gates `ok` (#390, #394); every enabled clang-tidy
  check is an error (#372, #409).

## [0.1.45] - 2026-10-16

Upgrade notes for operators:

- **Grafana and InfluxDB now bind to loopback.** After an upgrade with
  `--with-observability`, Grafana (:3000) is no longer reachable from the
  LAN. Pass `--with-grafana-network` on every observability run to keep LAN
  access, or reach it through the ssh tunnel the install summary prints.
  InfluxDB (:8086) always binds `127.0.0.1`.
- **The runtime drives its safe state at boot.** `safety.on_startup`
  defaults to `safe_state`: on every runtime start, the declared
  `safety.safe_state` runs once before automation and the HTTP server
  start, so an output left on (a heater in open loop) is stopped. Set
  `safety.on_startup: leave` where a restart must not interrupt a running
  process.
- `install.sh` now refuses to install an older runtime over a newer one;
  pass `--allow-downgrade` to do it on purpose.
- Grafana is restarted on every `--with-observability` run (#340), so its
  settings take effect without a manual restart.

The OpenAPI contract changes are additive (new optional properties).
anolis-workbench vendors this OpenAPI file and needs a repin to pick them
up.

### Added

- **Breaking: `safety.on_startup` (`safe_state` | `leave`, default
  `safe_state`)** (#362, #392). The runtime started in IDLE, which refuses
  new control calls but does not stop an output that is already on.
  `safe_state` runs the `POST /v0/estop` ladder once, after provider
  discovery and the first state poll and before automation and the HTTP
  server start, without engaging the latch or driving FAULT. The rung, call
  count and `safe_state_achieved` are logged, at ERROR (with one line per
  failed call) when the safe state is not achieved; the runtime still
  starts. `leave` changes nothing and logs a WARN. `GET /v0/runtime/status`
  gains an additive `startup_safe_state` object (`on_startup`, `ran`,
  `software_safe_state`, `safe_state_achieved`, `failed_actions`). Any
  other value fails config load. A supervised provider restart runs
  nothing, as before; `docs/safety.md` now says so.
- **`POST /v0/estop` reports `safe_state_achieved`** (#256, #368): `true`
  only when a rung ran, issued at least one call, and every call
  succeeded. The e-stop summary log line is at WARN when it is `false`, and
  the per-device re-assert log lines carry it too. It covers only the calls
  the rung issued, and a successful call means the provider accepted the
  command.
- **`health.excluded_signals`** (#325, #377) leaves listed signals of a
  device (`device_handle: provider_id/device_id`, `signals: [...]`) out of
  that device's health rollup, so an unused output or an empty input
  channel no longer marks it STALE or FAULT. Excluded signals are still
  polled, served and sent to telemetry. Malformed entries fail the load;
  a handle or signal id that matches nothing is logged as a WARN after
  discovery.
- **Signal `unit` and `description` in capabilities** (#352, #365).
  `GET /v0/devices/{provider_id}/{device_id}/capabilities` now carries the
  unit and description a provider declares on each signal, omitting either
  when empty. The OpenAPI `SignalSpec` gains both as optional properties.
  InfluxDB tags are unchanged.
- **`install.sh` reports provider liveness** (#344, #355). After the
  runtime answers, the health phase polls `GET /v0/providers/health` until
  every provider is RUNNING or RECOVERING, or the health timeout elapses,
  and logs `providers: N of M running` with one warning per provider that
  is not. A shortfall is a warning, not an install failure. The response is
  parsed as JSON with `jq`, or `python3` when `jq` is absent; with neither,
  the check warns that providers were not checked (#363, #383).
- **`install.sh --with-grafana-network`** (#351, #359) opens Grafana, and
  only Grafana, to the network. See Changed.
- **`install.sh --allow-downgrade`** (#366, #382). See Changed.
- `GET /v0/parameters` carries a `source` field, `default` until a POST
  changes the value and `operator` after, and startup logs each parameter
  at its config default (#273, #374). Operator changes are still not
  persisted across a restart.
- Startup warnings, all report-only:
  - a device whose signal `poll_hint_hz` is more than 2x away from
    `polling.interval_ms` (#269, #375); the hint stays advisory;
  - each ACTUATE function no dispatchable `AUTO -> FAULT` hook call drives
    (#246, #370); the refuse-hookless gate's verdict is unchanged;
  - each device with ACTUATE functions whose provider reports
    `watchdog_armed=false` (#297, #371). An absent key is silent, so no
    warning is not evidence of an armed watchdog.

### Changed

- **Breaking: `install.sh --with-observability` binds Grafana and InfluxDB
  to `127.0.0.1`** (#351, #359). Both used to listen on every interface,
  and Grafana serves dashboards to anonymous viewers. InfluxDB always binds
  `127.0.0.1:8086` through a new systemd drop-in and is restarted on every
  observability run. Grafana binds `127.0.0.1` unless
  `--with-grafana-network` is given; the drop-in is rewritten on each run,
  so a re-run without the flag puts it back on loopback. The summary prints
  the ssh tunnel by default, or the network address with the flag.
  `docs/security.md` gains an Observability section. The separate-host
  compose stack is unchanged.
- **Breaking: `install.sh` refuses to downgrade the runtime** (#366,
  #382). When the bundle's runtime is older than the installed
  `bin/anolis-runtime --version` (compared as X.Y.Z, so `0.1.44-rc1` equals
  `0.1.44`), the install stops with nothing changed and points to
  `--rollback` and `--allow-downgrade`. If either version cannot be read,
  it warns and carries on. `--dry-run --local` reports the comparison. Only
  this release's `install.sh` and later carry the check; an older one
  still installs over a newer runtime.
- **Breaking: the zero rung never sends a guessed boolean** (#255, #367).
  With `zero_is_safe: true`, an ACTUATE function with a required boolean
  argument was called with `false`, which can release a brake. It is now
  skipped and reported as not driven. Numeric arguments are zeroed as
  before. The e-stop now logs one WARN per action it did not drive.
- `estop.uncovered_actuating_functions` in `GET /v0/runtime/status` counts
  what the active rung can actually drive (#253, #369): a function is
  covered only by a call of that rung that targets it and passes the
  dispatch dry run. A machine whose hooks drive every output no longer
  reports a gap, and an undispatchable call no longer counts as coverage.
  What the e-stop runs is unchanged.
- Automation `error_count`, in the health view and engine diagnostics, is
  per run: it is cleared on a definition load and on entering AUTO (#378,
  #386).
- `just test` and `just test-py` take an optional anolis-provider-sim path
  and fail before pytest starts, with download and build instructions,
  when none resolves (#346, #356).
- Docs: the build is C++20 with GCC 13+, Clang 17+ or Visual Studio 2026
  (CMake 4.2+ on Windows) (#338, #341); the vcpkg baseline is the lock
  (#361); core, docs and contracts describe providers in protocol and SDK
  terms (#348, #358); `AGENTS.md` states the logging rule (#347, #357).

- The separate-host observability compose stack pins `grafana/grafana`
  13.2.4 (#308).

### Fixed

- **An open provider circuit is probed every 60 s** (#326, #376). Once the
  restart budget was spent, nothing closed the circuit and the provider
  stayed down until the runtime restarted. A successful probe closes it; a
  provider whose restart policy is disabled is never probed. A restart now
  stops the old process before starting the new one, so a wedged provider
  no longer runs beside its replacement, and `restart_policy.timeout_ms`
  is counted from when the old process has stopped, so it bounds the new
  process coming up (#379, #391).
- `GET /v0/automation/status` reports `execution_status` `idle` outside
  AUTO instead of `running` (#270, #373), and `failed` no longer outlives
  its run: it is cleared by loading a definition or entering AUTO again
  (#378, #386).
- A clean stop no longer logs `[ERROR] [StateCache] ReadSignals failed`
  for each provider systemd signalled with the runtime (#350, #381). Once
  shutdown is requested, a failed read logs at DEBUG and reports no device
  loss.
- **`install.sh --rollback` restores `manifest.json`** (#345, #354) from
  `.prev/meta/`, so it no longer describes the newer install after a
  rollback. A backup records the manifest only when its version matches
  the binary being saved.
- `install.sh --with-observability` restarts `grafana-server` on every
  run (#340, #343), so a changed drop-in, env file or datasource takes
  effect without a manual restart.
- Grafana's InfluxDB datasource debug lines no longer flood the journal
  while a dashboard is open (#349, #360). The Grafana drop-in sets
  `LogFilterPatterns=~.@level.:.debug.`; on systemd older than 253, which
  ignores the key, `install.sh` warns instead.
- `install.sh --uninstall` refuses an empty, relative, top-level, `$HOME`
  or symlinked `--prefix` before anything is stopped (#339, #342).

### Removed

- `core/automation/bt_nodes_sanity.cpp` and its executable target, which
  no test or CI job ran; its port checks moved into
  `BTNodesTest.NodesRegisterAndDeclareTheirPorts` (#347, #357).

## [0.1.44] - 2026-10-13

### Added

- **`install.sh --rollback` names the version it restores** (#315). The
  backup step records the replaced runtime's `--version` and a copy of
  `manifest.json` under `<prefix>/.prev/meta/`. `--rollback` and
  `--rollback --dry-run` print `rollback target: <version>` and the path of
  that manifest before restoring. A backup taken by an older `install.sh` has
  no record, and the target is reported as unknown. Rollback still restores
  binaries only.

### Fixed

- **`install.sh` refuses an unsafe `--prefix` on install and rollback**
  (#335). An empty, relative or symlinked prefix, `/` or a top-level
  directory, or a path that is or holds `$HOME`, is refused before anything
  runs, and so is such a prefix recorded in a staged bundle. Before, an empty
  prefix or `/` would have had the install chown every file in `/bin` to the
  service user. `--uninstall` keeps its stricter guard.
- **A behaviour tree that fails to take the service context now fails its
  load** (#281). A tree that remapped a port onto the runtime's
  service-context key built, then made `load()` throw after it had replaced
  the active definition. The context is now written to the candidate before
  it is published, so the load fails and the previous definition stays
  active.
- **`--with-observability` sets Grafana's log level to `warn`** (#248).
  Grafana logged every request at `info`, so an open dashboard's auto-refresh
  flooded the journal and evicted runtime logs. On a host where Grafana is
  already running, re-running `install.sh --with-observability` writes the
  setting but does not restart Grafana; run
  `systemctl restart grafana-server` once.

### Changed

- `docs/http-api.md` defines `mode` in `GET /v0/runtime/status` when
  automation is disabled: always `MANUAL`, with `GET /v0/mode` returning 503
  as the way to tell (#250).
- `AGENTS.md` describes CI's clang-tidy gate as diff-based (#317).

## [0.1.43] - 2026-10-09

The runtime and `install.sh` stop knowing about one transport and one platform
(#318, #319). Ships together with anolis-provider-bread 0.5.0 and
anolis-provider-ezo 0.4.0, which publish claims and answer `--check-host`.

### Changed

- **Ownership is checked over opaque claims.** A device's `anolis.claim` tag
  lists the resources it needs exclusively, space-separated; the runtime
  requires every key to have one owner, at startup and on provider restart, as
  before. It compares exact strings and parses nothing (providers spell an I2C
  key with the provider SDK's `i2c::claim_key`). The `hw.bus_path` /
  `hw.i2c_address` tags are no longer read: there is no fallback. A provider
  that still publishes only those is logged once at startup as unchecked; with
  an older bread or ezo, the ownership check covers nothing until they are
  upgraded. The startup line is now `Ownership validation passed`.
- **`install.sh` no longer sets up hardware.** Removed: the I2C phase (the
  Pi `dtparam=i2c_arm=on` edit and its reboot notice), the `i2c-tools`
  install, adding `anolis` to `i2c,gpio,dialout`, and the unit's
  `SupplementaryGroups=`. The unit's user keeps its own groups, so access the
  host already granted to `anolis` is unchanged. Enabling buses and granting
  device access belong to the project's host prep for its platform, run before
  `install.sh`.

### Added

- **Host preflight in `install.sh`.** Before starting anything it runs, as
  `anolis` from the prefix, every binary's `--version` (a binary the host's
  glibc/libstdc++ cannot load fails with the loader's message) and each
  provider's `--check-host <its config>`, printing each unmet requirement with
  the provider's fix. Unmet requirements stop the install unless
  `--allow-unmet-host`; a provider that gives no answer is reported and
  skipped. See `docs/deploy/install-sh.md`, Host requirements.
- **Provider host-check readiness keys are logged.** When a provider's
  WaitReady reports `host_check=unmet`, the runtime logs its `host_unmet` text
  (executable profile v1 §3).

## [0.1.42] - 2026-10-05

### Fixed

- **Releasing an emergency stop no longer lets automation restart a device by
  itself** (#285, #299). Reproduced on hardware on 2026-08-20: with the machine
  in AUTO, a Category 0 stop and its release restarted the impeller about 1.4 s
  later with no human action. The runtime survives the power cut, the boards
  reboot to motors-off, and the behaviour tree's keepalive re-issued its
  command on the next tick.

  A device whose read fails with `UNAVAILABLE` or `DEADLINE_EXCEEDED` is now
  latched: behaviour-tree calls that actuate it are refused with
  `FAILED_PRECONDITION` until the next `MANUAL -> AUTO`. Mode-transition hooks
  and safe-state calls are not blocked, so the latch cannot veto the transition
  that clears it. `INTERNAL` (a reply that arrived but did not decode) does not
  latch, and neither does a provider restart: provider loss is not device loss.
  When a latched device answers again, its declared `safety.safe_state` calls
  are re-issued in any mode, since a device that came back without rebooting is
  still running its last command. Latched handles are listed as
  `device_loss_latched` on `GET /v0/runtime/status`.

  Not covered: whether sustained device loss should stop the whole machine
  (#301), and the `device_availability` SSE event, which is still never emitted
  (#300).

- **The run registry comes up on installed machines** (#302, #303). The
  runtime unit set `User=anolis` but no `WorkingDirectory`, so the default
  relative data directory resolved to `/anolis-data`, creating it failed, and
  every `/v0/runs` endpoint answered 503 for the life of the process while
  `/v0/runtime/status` stayed green and the install reported success. The unit
  now sets `WorkingDirectory=<prefix>` and the install creates
  `<prefix>/anolis-data`. The install's health phase probes `GET /v0/runs` and
  warns when it does not answer 200. The unit is re-rendered on every install,
  so existing machines pick this up at their next upgrade.

- **`install.sh --uninstall` keeps the run journal** (#304, #305). With the
  run registry working (#303), recorded runs live in `<prefix>/anolis-data`,
  and `--uninstall` removed them with the rest of the prefix. A non-empty
  journal is now kept, the way observability data already is, and the
  uninstall prints the `rm -rf` that would purge it. Before anything is
  stopped, the prefix must be an absolute path to an anolis install
  (`bin/anolis-runtime`), not a symlink, not `/` or a top-level directory,
  not `$HOME` or a directory holding it, and with nothing mounted at or below
  it; a refusal changes nothing. The prefix is then removed as one canonical
  path with `rm -rf --one-file-system`.

## [0.1.41] - 2026-09-10

### Added

- **Startup preflight for declared hook and safe-state calls** (#252). Once
  providers have reported their capabilities, every call declared in
  `safety.safe_state` and `automation.mode_transition_hooks` is dry-run against
  the live registry, and the ones that cannot resolve or type-check are logged.
  These calls otherwise only execute during a mode transition or an e-stop, so a
  broken one stayed invisible until the moment it was needed. `--check-config`
  cannot cover this: it exits before any provider starts, so no `ArgSpec` exists.
  The log names which ladder rung `POST /v0/estop` would actually run, warns when
  a machine declares no software safe state at all, and re-runs after a provider
  restart republishes its inventory. It reports; it never refuses to start.
  Passing means *dispatchable*, not *will run* — runtime gating (IDLE/AUTO mode,
  the actuation latch) lives in `execute_call` and is deliberately not simulated.

- **The startup preflight now names the specific hazard behind #251.** A machine
  can declare its safe state twice over, for two different triggers:
  `safety.safe_state` is what `POST /v0/estop` runs, while a `-> FAULT` entry in
  `automation.mode_transition_hooks` is what an autonomous fault runs. The
  refuse-hookless gate only ever asks for the second, so following its refusal
  message produces a machine whose e-stop reads nothing.

  Pressing the e-stop there is worse than not pressing it: the latch engages
  before FAULT is entered, so it also **suppresses** the `-> FAULT` hooks that
  reaching FAULT by any other route would have run. The preflight previously said
  only "no software safe-state is declared"; on this shape it now says that the
  hooks exist, that the e-stop will not run them, and that it will prevent them
  from running. `has_fault_safe_state_hook` is shared with the gate rather than
  reimplemented, so the two cannot disagree about what a fault safe-state hook is.

  Diagnostic only — no behaviour change. Two attempts to fix this in the runtime
  were closed after review (#259, #260): the runtime cannot know whether a
  machine's FAULT hooks constitute a safe state for that machine in its current
  state, and on DCMT hardware sending them can clear a watchdog trip and release
  a brake (#261).

- **Device identity is no longer discarded on arrival** (anolishq/anolis-provider-bread#126).
  The runtime read exactly two descriptor tags — `hw.bus_path` and
  `hw.i2c_address`, for I2C ownership validation — and dropped the rest of
  `Device.tags` along with `Device.type_version`. A provider that published its
  device's firmware revision therefore had it thrown away at the registry
  boundary, and a bench result could not be attributed to a firmware build at
  all: on the reference machine the DCMT/RLHT revision existed only in a
  provider log line, lost on journal rotation.

  `GET /v0/providers/health` now carries `type_version` and the full
  `descriptor_tags` map per device, and `anolis_device_health` carries
  `type_version` as a field. Both are passed through verbatim — the runtime
  interprets no key, so this holds no provider-specific knowledge and works for
  any provider that declares identity. Registry-derived, so it is present even
  when the provider is unavailable and its live health cannot be fetched, which
  is when knowing what is installed matters most. Bounded on ingest at 32 tags
  and 128 **bytes** per string — truncated on a UTF-8 character boundary, since
  cutting mid-codepoint emits invalid UTF-8 and would fail the whole response —
  because the only limit upstream is the 1 MiB provider frame.

  Note what `type_version` is: the device **type schema** version the protocol
  defines, which identifies the type contract and is not required to track the
  firmware build. It is not a firmware revision, and should not be read as one.

- **Allowlisted device identity strings reach the timeseries** (anolishq/anolis-provider-bread#126).
  The health line-protocol writer had integer and boolean allowlists and **no
  string category at all**, so a provider reporting its firmware through health
  metrics had it reach the HTTP surface and never the timeseries.
  `startup_firmware`, `startup_product_code` and `firmware_version` are now
  written as fields (truncated to 128 bytes, omitted when empty). Note these
  keys are not in the SDK's reserved vocabulary yet — the first two are what
  ezo already emits, the third is what bread#126 proposes for itself.

  Fields rather than tags throughout: as tags they would fork the series on
  every firmware change, orphaning the history they exist to connect. The
  descriptor tag map is still not written to line protocol, whose keys must
  stay bounded.

  **What this does and does not buy today.** ezo's firmware now lands in the
  timeseries with no provider change. bread's does not: it publishes
  `module_version` and `crumbs_version` as descriptor *tags*, so they reach
  `GET /v0/providers/health` but not InfluxDB, and its `type_version` is the
  module major alone — a constant `"1"`. Making a bread device attributable in
  the timeseries needs a change in bread. And a device whose reported version
  never moves when its behaviour does (feastorg/Slice_DCMT#13) stays
  unattributable regardless.

### Changed

- **`POST /v0/estop` is documented as what it is: a software stop, not an
  emergency stop.** Most confusion about this system's safety behaviour came
  from using one word for three unrelated mechanisms — the physical button and
  contactor, the `safety.safe_state` ladder, and the firmware command watchdog —
  and then comparing them as alternatives.

  `docs/safety.md` now opens with the stop categories from IEC 60204-1:2005
  §9.2.2, and states plainly that a bus command to a powered device is at best a
  Category 2 stop: it removes no power, has no determined performance level, is
  not necessarily a *controlled* stop in the standard's sense, and does not
  execute at all if the runtime, a provider, or the bus has failed. ISO
  13850:2015 §4.1.3 permits only Category 0 or 1 as an emergency stop, so no
  amount of runtime work makes this endpoint one. The endpoint's OpenAPI
  description says the same. The path keeps its name for wire compatibility.

  `Emergency Response` previously listed the software stop **first** and the
  hardware e-stop **third**, which teaches an operator to reach for a control
  that cannot remove power while a person is at risk. Hardware is now first for
  any physical hazard, with an explicit rule for when the operator cannot tell
  which case applies.

  Also records two consequences that keep surprising people: under a Category 0
  stop every device disappears at once and that is the *success* signal rather
  than a fault, and under Category 0 a motor coasts by construction because an
  unpowered H-bridge cannot brake.

  Documentation and one endpoint description only — no behaviour change.

- **The refuse-hookless gate no longer steers operators into a machine with no
  e-stop** (#258). A machine declares its safe state twice over, for two
  different triggers: `safety.safe_state` is what `POST /v0/estop` runs, and a
  `-> FAULT` entry in `automation.mode_transition_hooks` is what an autonomous
  fault runs. The gate only ever asked for the second, so an operator who
  followed its refusal message to the letter got AUTO permitted *and* a machine
  whose e-stop drove nothing — and whose latch then suppressed the very hooks
  they had just added (#251). That is not hypothetical: it is the state
  `bioreactor-v1` shipped in until anolishq/anolis-projects#58, and why its
  E-stop was latch-only on the reference machine for weeks.

  When the e-stop ladder would drive nothing, the refusal now says that the hook
  it is asking for is not what the e-stop runs, and names `safety.safe_state`.
  When a working ladder exists the message is unchanged — an operator who
  already did this does not need telling.

  "Would drive nothing" is `control::planned_safe_state_kind`, extracted from
  `SafeStateController` so the gate and the controller cannot disagree. Asking
  the config directly would have been wrong: the setpoints rung counts only when
  it covers *every* actuating output, so setpoints that cover nothing declare a
  safe state that plans `None` — and the gate would have stayed quiet for
  exactly the machine it exists to warn about.

  `docs/getting-started.md`, `docs/automation.md` and `docs/safety.md` also
  described the gate without mentioning `safety.safe_state`. An operator who
  follows those adds the hook proactively and never sees the refusal at all, so
  the runtime message alone would not have reached them.

  **No behaviour change.** `safety.safe_state` still does not satisfy the gate,
  and correctly so: it fires only on operator request, never on an autonomous
  transition, so it is not a fault-safe path. Requiring it would refuse configs
  that are legal today, which on a running culture is its own incident. This is
  the companion to #262, which named the same hazard at startup.

### Fixed

- **A behaviour-tree tick exception no longer leaves the runtime actuating**
  (anolishq/anolis#279). A tick that threw was caught, logged, counted, and
  emitted as an `AutomationFaultEvent` — and then the loop ticked again. The
  runtime stayed in AUTO and every actuator held whatever the tree last
  commanded, indefinitely.

  Nothing else caught it. The DCMT firmware command watchdog is a *bus*-liveness
  watchdog, not a control-liveness one: it is fed by every reply builder,
  including the routine state poll, and the poller runs on a separate thread that
  a tick exception does not touch. So the watchdog stayed fed while the
  controller was dead, the `*->FAULT` hooks never ran because nothing drove
  FAULT, and `safety.safe_state` has exactly one trigger in the whole runtime
  (`POST /v0/estop`).

  The engine now drives FAULT from the exception handler, which stops further
  ticking via the loop's own AUTO gate and fires the declared `*->FAULT` hooks.
  This path is more reachable than it looks: BT nodes guard their inputs but not
  their outputs, and a precondition script referencing an undefined blackboard
  entry throws on the first tick of a tree that loaded cleanly — so an edited
  tree can fault the engine on reload.

  `GET /v0/automation/status` now reports `execution_reason: terminal_failure`
  rather than `mode_gate` for this case, so the cause is not hidden behind its
  own effect.

- **A `uint64` provider parameter could not be driven from any config-declared
  hook** (#252). Call arguments in `safety.safe_state` and
  `automation.mode_transition_hooks` are parsed into `ParameterValue`, which has
  no unsigned alternative — every YAML integer became `int64` and `CallRouter`
  rejected it as a type mismatch. On the reference bioreactor this left the
  heater (`set_open_duty_pct`, uint64 args) with **no expressible software safe
  state**: the e-stop ladder ran and that call failed. `CallRouter` now
  reconciles argument types against the target's declared `ArgSpec` before
  validating — `int64 <-> uint64`, and integer to `double` (so `setpoint_c: 0`
  works alongside `duty_pct: 0` on the same device). Widening only, and never
  silent: a negative into a `uint64`, a value above `INT64_MAX` into an `int64`,
  or an integer past 2^53 into a `double` is **refused, not wrapped or rounded**.
  Declared min/max bounds still apply to the converted value, and `double` is
  never narrowed to an integer. Applies to every caller, so `POST /v0/call` also
  now accepts an in-range `int64` for an unsigned parameter where it previously
  returned 400.

- **A provider-supplied string could forge an InfluxDB row.** `escape_field_string`
  escaped only `"` and `\`, but line protocol is newline-delimited and a field
  string has no encoding for a literal newline. A raw `\n` in a provider value
  therefore did not corrupt one row — it ended it and started another, so a
  value crafted to close the row and open a well-formed second one injected a
  whole measurement with attacker-chosen `runtime_name` / `provider_id` /
  `device_id` tags. The likely outcome was worse than the forgery: the
  surrounding malformed lines get the entire batch rejected, silently and
  repeatedly, losing all telemetry.

  Newline, carriage return and tab are now rewritten to their two-character
  escapes and any other C0/DEL byte is dropped. This affected every
  provider-controlled string field, `anolis_signal`'s `value_string` included,
  so it predates the health-identity fields that prompted the audit.

- **`install.sh` never enabled I2C on a Pi with a display** (#249). `phase_i2c`
  decided the bus was already up by globbing `/dev/i2c-*`, which matches the
  HDMI DDC adapters (`i2c-20`, `i2c-21` — `fef04500.i2c`) that the VC4 driver
  publishes regardless of `dtparam=i2c_arm=on`. The guard was therefore
  effectively always true and the enablement below it unreachable on exactly the
  hardware it was written for: the install reported `i2c: already enabled`, never
  appended the dtparam, never set `REBOOT_NEEDED`, and then failed 30 s later
  with `health: runtime not responding` while the real cause —
  `failed to open I2C bus '/dev/i2c-1': No such file or directory` — appeared
  only in the journal.

  Detection now looks for the GPIO-header bus specifically: `/dev/i2c-1`, or an
  adapter whose name identifies the ARM/BSC controller if the number differs.
  The "configured but not live" warning now says a reboot is required *before
  providers can open /dev/i2c-1*, rather than referring to a glob.

  The bug survived because nothing could exercise it without real hardware, so
  the two filesystem roots are overridable and four bats cases pin the
  behaviour — including a DDC-only adapter set, which is what a desktop Pi with
  no dtparam actually looks like.

- **The uninstall's data-purge instruction named a directory that does not
  exist** (#247). It told the operator their recorded telemetry lived in
  `/var/lib/influxdb2`; the Debian `influxdb2` package stores under
  `/var/lib/influxdb`. Following the printed "Full purge (DESTROYS recorded
  data)" block verbatim therefore left **every byte on disk** while reporting
  success — a data-handling instruction failing in the direction that
  under-deletes, so the mistake stays invisible until someone goes looking for
  data they believed was gone, or hands on a device assuming it was wiped.

  The guidance is now derived from what is actually present on the host rather
  than hardcoded, so it cannot drift from the package layout again. The purge
  block also now covers `influxdata.list.ucf-dist` (left by dpkg when the vendor
  list was modified, and enough to resurrect the repo on a later reinstall) and
  the non-empty `/etc/influxdb` and `/etc/grafana` that dpkg declines to remove.

- **The staged bundle's `.sha256` sidecar could not be checked with
  `sha256sum -c`** (#268). `--stage` piped the digest through `awk '{print $1}'`,
  so the sidecar held a bare 64-character hash with no filename. The air-gap
  operator's obvious command — the single most important thing they do after
  carrying a tarball across a trust boundary on removable media — failed with
  `no properly formatted checksum lines found`, which reads like a corrupt
  download rather than a misformatted checksum file. The digest itself was
  always correct.

  The sidecar is now standard `sha256sum` output, generated from within the
  staging directory so the recorded filename is bare; an absolute path would
  name a file that does not exist on the target machine after the handoff. This
  also aligns `--stage` with the release workflow's `SHA256SUMS`, which already
  used the two-field convention. The bundle-internal `checksums.sha256` was
  already correct and is unchanged.

  `--stage` now also prints the verify command above the install hint, so the
  order shown is the order the operator should follow.

## [0.1.40] - 2026-08-02

### Changed

- The refuse-hookless `AUTO` gate now requires a safe-state hook that actually
  covers the `AUTO -> FAULT` transition, not merely any declared hook. A config
  declaring only a non-FAULT hook (e.g. `AUTO -> MANUAL`) previously satisfied
  the gate while having no autonomous safe-state path on a fault, so a
  behavior-tree fault could leave actuators at their last-commanded values. The
  predicate now mirrors the runtime's own two-filter hook dispatch: `from` must
  match `AUTO` (`AUTO`/`"*"`/omitted) **and** `to` must match `FAULT`
  (`FAULT`/`"*"`/omitted), in either the before- or after-transition list. No
  behavior change for a config whose hook already covers `AUTO -> FAULT`,
  including the canonical `bioreactor-v1` automation and full variants (#232).

### Fixed

- The refuse-hookless gate is re-checked when a provider restarts while the
  runtime is already in `AUTO`. A restart publishes a replacement inventory, so
  a provider returning with a **new** actuating function and no `AUTO -> FAULT`
  safe-state hook could be driven by the behavior tree with no autonomous
  safe-state path until the next mode change — a narrow but real fail-open. The
  gate now runs after `restart_provider`; when it refuses against the live
  registry it logs an error and forces `FAULT`, halting autonomous actuation.
  The restart itself still succeeds (#233).
- `install.sh --local` refuses a differing explicit `--prefix`. A staged bundle
  bakes the prefix into its systemd unit and rendered config paths, so
  installing it under another prefix silently produced a broken unit. The
  staged prefix is now recorded in `manifest.json`, adopted when `--prefix` is
  omitted, and a conflicting explicit value fails with re-stage guidance (#222).
- Staging cross-validates configured providers against pinned ones. A config
  referencing an unpinned provider previously produced a bundle that passed its
  checksums and then crash-looped on the target. Config rendering now fails
  staging if any provider command in any runtime-config variant does not
  resolve to a pinned, bundled provider binary; a pinned but unreferenced
  provider warns (#226).
- `components.optional.telemetry_export` is consumed instead of ignored. The
  pin is baked into `manifest.json` so the offline `--local` path (which has no
  profile) still receives it, with precedence profile pin > environment >
  fallback, warning when a pin overrides an explicit environment value (#223).

## [0.1.39] - 2026-07-30

### Added

- Device-health liveness staleness is now **cadence-derived** instead of a fixed
  2s/5s wall clock (D7 phase 1). The `WARNING`/`STALE` thresholds scale with the
  poll interval and the provider's device count
  (`warn = max(3 x poll_interval_ms x device_count, 2000)`,
  `stale = max(8 x ..., 5000)`), so a healthy but *serialized* bus (e.g. an EZO
  chain whose per-cycle dwell exceeds 2s) no longer false-flaps `STALE`. At the
  default 500ms interval with a single device the bounds are exactly the historic
  2000/5000 — no change for fast buses. A new optional `health.staleness`
  config section (`warn_after_ms` / `stale_after_ms`, `0` = derive) provides
  absolute overrides for buses the heuristic does not fit. Pure liveness change:
  the `/v0/providers/health` and `anolis_device_health` health strings and
  fields are unchanged (anolishq/anolis#220).

- Device health now folds in **per-signal quality and freshness** (D7 phase 2,
  closes anolishq/anolis#220). A device serving a cached `QUALITY_FAULT` sample
  reads the new health state **`FAULT`**, and a signal of degraded quality
  (`QUALITY_STALE`/`QUALITY_UNKNOWN`) reads `STALE`, even when the poll is fresh
  (the false-green the old liveness-only check missed). The per-signal *age*
  check uses `max(declared stale_after_ms, <liveness stale bound>)`, so it never
  fires before the cadence-derived liveness bound (cannot re-introduce the
  false-STALE storm) and only flags a genuinely stuck/old cached sample. New
  per-device fields `stale_signal_count` / `fault_signal_count` on
  `/v0/providers/health` and `anolis_device_health` explain the result;
  per-device precedence is `UNAVAILABLE > FAULT > STALE > WARNING > OK`.
  Provider-declared `stale_after_ms` is now carried through registry ingestion.
  Additive wire change: the device `health` enum gains `FAULT` and the two count
  fields (required on the HTTP surface).

- Refuse-hookless safety gate: the runtime now refuses the transition into
  `AUTO` when automation is enabled with actuating outputs but no safe-state
  `mode_transition_hooks` declared. Enforced at `set_mode` (the single choke
  point into AUTO) and re-evaluated against the live device inventory on each
  `MANUAL -> AUTO` attempt. Manual control and `POST /v0/estop` stay available,
  and `FAULT` is never blocked. Actuating outputs are enumerated fail-closed
  (any function not explicitly tagged read-only counts), and a startup ERROR log
  names them. Automation configs with actuating outputs must now declare
  `mode_transition_hooks` (e.g. a `*->FAULT` hook that drives actuators to a safe
  state) to run autonomously; a config whose functions are all read-only is
  unaffected. Interim: the gate checks that hooks are *declared*, not yet that a
  hook targets `FAULT` specifically (anolishq/anolis-workbench#237).

- `install.sh --variant <key>` (alias `--runtime-profile`) selects the active
  runtime config by a machine-profile `runtime_profiles` map **key** instead of
  the previous `.manual.` filename grep, so a mislabeled config can no longer be
  boot-selected as inert. install.sh now **verifies the selected variant is
  actually inert** (`automation.enabled=false` and no `mode_transition_hooks` —
  a variant declaring hooks is one flag-flip from actuation, so it is not a
  clean inert baseline) and refuses a non-inert config on a fresh
  install; a deployment must declare a `runtime_profiles` map (fail-closed).
  An operator-activated config preserved across an upgrade is kept with a
  warning rather than blocking the upgrade. Automation is activated from Operate
  after install, never at install time (anolishq/anolis-workbench#225).
- Software safe-state e-stop: `POST /v0/estop` engages a latching safe-state
  that works regardless of `automation.enabled`, and `POST /v0/estop/clear`
  releases it. The safe-state ladder runs the profile's declared safe-state
  hooks; else drives per-actuator safe setpoints (only when they cover every
  actuating output); else zeroes all actuating outputs, but only when the
  profile asserts `safety.safe_state.zero_is_safe`; else honestly reports no
  software safe-state rather than a false zero. While latched, actuating
  `POST /v0/call` requests are refused with `409` (fail-closed on unclassified
  functions); read-only calls are unaffected. On an automation machine an
  e-stop also drives `FAULT`. `GET /v0/runtime/status` gains an `estop` block
  (`latched`, `software_safe_state`, `latched_at_epoch_ms`,
  `uncovered_actuating_functions`). A software stop is not a substitute for the
  hardware backplane cut (#219).
- New top-level `safety.safe_state` runtime-config section (`hooks`,
  `setpoints`, `zero_is_safe`), parsed independently of `automation` so a
  pure-manual machine can declare a software safe-state (#219).
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

[Unreleased]: https://github.com/anolishq/anolis/compare/v0.1.39...HEAD
[0.1.39]: https://github.com/anolishq/anolis/compare/v0.1.38...v0.1.39
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
