# Deploy with `install.sh`

The canonical path. The script lives at **`tools/install.sh`** in this repository
and is published as a release asset — `install.sh` is not at the repo root, and a
staged bundle carries its own copy inside it.

`install.sh` needs root, except `--stage`, which builds a bundle and installs
nothing.

## Online: install from a config

```bash
sudo tools/install.sh --project /path/to/bioreactor-v1
```

The config directory holds `machine-profile.yaml` with `config/` and `behaviors/`
beside it. `install.sh` reads the component pins from the profile, downloads those
versions, assembles a bundle, and installs it.
[bioreactor-v1](https://github.com/anolishq/anolis-projects/tree/main/projects/bioreactor-v1)
is a complete example, and the workbench writes projects in the same layout.
`install.sh` relies on that layout:

- The profile must have a `components:` block naming the runtime and provider
  versions.
- Only `config/anolis-runtime.*.yaml` and `config/provider-*.yaml` are installed.
- The directory's name is the installed project's name. Provider config paths
  in the runtime config name it too (`../anolis-projects/projects/<name>/config/...`),
  so a copy under a new name must update those paths.

**This machine needs internet**, plus `curl`, `tar`, `sha256sum`, and `python3`
with `pyyaml` (`sudo apt install python3-yaml`) to resolve the profile. Set
`GITHUB_TOKEN` if you hit release rate-limiting — that is the usual cause of a
failed download.

### Using `install.sh` from a release

Without a checkout, take `install.sh` from the release whose runtime you
install, and check it against that release's `SHA256SUMS` before running it as
root:

```bash
tag=v<version>   # the runtime version machine-profile.yaml pins
curl -fsSLO "https://github.com/anolishq/anolis/releases/download/${tag}/install.sh"
curl -fsSLO "https://github.com/anolishq/anolis/releases/download/${tag}/SHA256SUMS"
sha256sum -c --ignore-missing SHA256SUMS && sudo bash install.sh --project /path/to/bioreactor-v1
```

`sha256sum -c` prints `install.sh: OK`. `--ignore-missing` skips the lines for
assets you did not download. A `SHA256SUMS` with no line for `install.sh`
verifies nothing, and `sha256sum` then reports `no file was verified` and exits
1, so the `&&` stops before `sudo`. Releases up to v0.1.49 have no such line.

## Offline: stage, carry, install

Build the bundle somewhere with internet. No root needed:

```bash
tools/install.sh --stage ./out --arch arm64 --project /path/to/bioreactor-v1
```

That writes `anolis-<profile>-<version>-<arch>.tar.gz` and a `.sha256` beside it,
and prints the two commands to run next. Carry both to the target, then:

```bash
sha256sum -c anolis-bioreactor-v1-0.1.41-arm64.tar.gz.sha256
tar xzf anolis-bioreactor-v1-0.1.41-arm64.tar.gz
sudo anolis-bioreactor-v1-0.1.41-arm64/install.sh --local anolis-bioreactor-v1-0.1.41-arm64.tar.gz
```

Verify before installing — the sidecar is standard `sha256sum` output, so `-c`
works directly.

**The bundle carries its own `install.sh`.** You do not need this repository on
the target: unpack the tarball and use the copy inside it, as above. This route
needs no internet and no `python3` on the target. `--stage` checks that
copy against the runtime release's `SHA256SUMS`, as it does each tarball, and
stops on a mismatch. A release before 0.1.50, whose `SHA256SUMS` has no line for
`install.sh`, is staged with a warning that names it.

**A staged bundle bakes its install prefix.** `--local` refuses an *explicit*
`--prefix` that differs from the one it was staged with; pass none and the baked
prefix is adopted.

## Choosing a runtime variant

`--variant <key>` selects a `runtime_profiles` entry from the machine profile
(default: `manual`). `--runtime-profile` is an accepted alias.

On a **fresh** install the variant must be **inert** — automation off, no
mode-transition hooks — and a non-inert one is refused outright, so a first
install cannot bring a machine up already actuating.

> **On upgrade, an existing `runtime.yaml` is preserved and `--variant` is not
> applied.** This protects a config an operator has edited or activated, but it
> has two consequences worth knowing:
>
> - Passing `--variant full` to an upgrade does nothing except log a warning.
> - The inert check drops to **warn-only** on upgrade. If automation was activated
>   from Operate, upgrading restarts the runtime with automation still enabled,
>   announced by one yellow line in a long log. It comes back in IDLE and runs no
>   behaviours until an operator selects AUTO again.
>
> To switch variants, re-run with **`--replace-config`**, or edit
> **`/opt/anolis/config/runtime.yaml`** in place. (Under `--prefix`, that is
> `<prefix>/config/runtime.yaml`.) `--replace-config` moves the existing file to
> `runtime.yaml.replaced-<UTC time>` beside it, then installs the bundle's. The
> variant must still be inert, and the check runs before anything moves. Both
> paths are logged, and `--dry-run --replace-config` says what it would
> replace. Provider configs need no flag: they live in
> `<prefix>/projects/<project>/config/`, which every install replaces, and the
> runtime config points there.

## Upgrading a running machine

**Leave the runtime running.** The install replaces files under the running
runtime and restarts it once, near the end (`systemctl restart`), so the boards
go unserviced only for that restart. If you stop it first, every board with a
hardware watchdog goes unserviced for the whole install. That is longer than a 5 s watchdog, so
each board trips. Its trip holds it safe, but the startup safe state then fails:
every hook call is refused with `WATCHDOG_TRIP`, and the install warns `health:
startup safe state NOT achieved`. On the reference rig on 2026-10-31, an install
after a stop tripped all three boards; the installs before and after it, with
the runtime left running, tripped none (#711).

If the boards did trip, clear each one with its provider's clear-trip function,
then release any brake the trip set. For the bread provider that is
`clear_watchdog_trip` on each board, then `set_brake` with both channels false on
each DCMT. Restart the runtime (`sudo systemctl restart anolis-runtime`) to
re-apply the startup safe state. Until then, the status still reports the
failed attempt from the last start.

## Optional components

| flag | effect |
| --- | --- |
| `--with-telemetry-export` | Installs `anolis-telemetry-export.service`, inert until secrets are provided. Online only. |
| `--with-observability` | Installs InfluxDB 2.x and Grafana as native apt packages with their own units, 30-day bucket retention and scoped tokens. Online only. |
| `--with-grafana-network` | With `--with-observability`, opens Grafana (:3000) to the network. Without it Grafana binds `127.0.0.1` and the summary prints the ssh tunnel to reach it; InfluxDB always binds `127.0.0.1`. See [Observability](../security.md#observability) in the security notes. |

Two things about observability that surprise people. Its services **start
immediately** — `--no-start` does not gate them. And if the vendor apt repos
are unreachable it **warns and skips** rather than failing: the runtime install
still succeeds, and you can re-run with the flag later once online. The same
holds when a repo is reachable but its index is mid-publish ("Mirror sync in
progress?"): `apt-get update` is tried 3 times, 15 s and then 45 s apart, and
the warning then names the failing step and apt's first error.

Raise retention with `OBSERVABILITY_RETENTION`, but read `observability/README.md`
("Data Retention") on the trade-offs first.

## Other flags

| flag | effect |
| --- | --- |
| `--no-start` | Install without starting the runtime. Does not gate observability. |
| `--dry-run` | Print what would happen, change nothing. Ignored by `--stage`, which builds the bundle anyway. |
| `--allow-unmet-host` | Install even when a provider reports unmet host requirements. See [Host requirements](#host-requirements). |
| `--allow-downgrade` | Install even when the bundle's runtime is older than the installed one, which `bin/anolis-runtime --version` reports. Without it the install is refused before anything changes; `--rollback` is the way back to the previous install. `--dry-run --local` reports the comparison. |
| `--replace-config` | Replace an existing runtime config instead of preserving it. It moves to `runtime.yaml.replaced-<UTC time>` beside it first, and the variant must be inert. See the upgrade note above. |
| `--keep-hostname` | Keep the host's name. By default an arm64 host with a device-tree serial is renamed `anolis-<last 8 of serial>`, and `127.0.1.1` in `/etc/hosts` is pointed at the new name. With this flag neither happens, and the printed access URL and the mDNS name (`<host>.local`) use the existing name. |
| `--prefix <path>` | Override the install prefix (default `/opt/anolis`). Install and rollback refuse, before changing anything, a prefix that is empty, relative or a symlink, resolves to `/` or a top-level directory, or is or holds `$HOME`; so does an install that adopts a staged bundle's prefix. |
| `--rollback` | Restore the previous binaries from `<prefix>/.prev` and restart. |
| `--uninstall` | See the warning below. |

`--rollback` restores **binaries only**, plus the `manifest.json` saved with
them so the manifest describes what is installed; config is left alone. A
backup taken by an older `install.sh` saved no manifest, and rollback warns
that `manifest.json` still describes the newer install. The backup is taken
on each install that actually changes the binaries — an install of a byte-identical
`bin/` deliberately does not overwrite the backup, so `--rollback` still reaches
the last *different* install.

> **`--uninstall` is destructive beyond the binaries.** It stops, disables and
> removes the units install.sh writes, `anolis-runtime` and
> `anolis-telemetry-export`, then removes the prefix — which includes
> `<prefix>/projects/<profile>`, your installed config and behaviours — and deletes
> the environment files holding the runtime's API token, including the
> workbench service's copy at `/etc/anolis/workbench-runtime-token`. Recorded data is kept:
> the run journal in `<prefix>/anolis-data` (when it holds anything), observability
> data, packages and the system user. With observability installed, the InfluxDB
> credentials in `/etc/anolis/observability.env` and
> `/etc/anolis/observability-grafana.env` are kept with the instance they
> unlock, and a later `--with-observability` install reuses them. On a host an
> uninstall before 0.1.54 stripped of them, that install mints new tokens with
> the admin token in root's influx CLI config (`/root/.influxdbv2/configs`,
> also kept) and records them; with no CLI config it stops and names it. A
> Grafana admin password it makes then is also set in the kept `grafana.db`. The
> uninstall prints the purge command for what it kept. Back up your config directory first if it is not also held
> elsewhere.
>
> Before it stops anything, the uninstall checks the prefix and refuses one that
> is not an absolute path, is a symlink, resolves to `/` or a top-level directory,
> or is or holds `$HOME`. A prefix that exists must also be an anolis install (it
> must hold `bin/anolis-runtime`) and have nothing mounted at or below it. The
> uninstall also refuses when one of those units runs from another prefix, and
> names it: `--prefix /opt/anolsi`, a typo for a live `/opt/anolis`, stops
> nothing and deletes nothing. A prefix that does not exist but that its units
> run from is still uninstalled, units and environment files included. A refusal
> changes nothing. If the run journal is on its own mount, unmount it first.
> Another `anolis-*` unit, such as the workbench's `anolis-workbench.service`,
> is kept, and the uninstall prints the commands that remove it.

## After installing

```bash
systemctl status anolis-runtime
journalctl -u anolis-runtime -f
```

A config that sets `bind: 127.0.0.1` is installed with auth on (see
[Security](../security.md#defaults)). With auth on, an HTTP request needs the
API token, even on the device itself. This reads it from the root-only
`runtime.env` and passes it to curl on stdin, so it does not appear on the
command line:

```bash
sudo sed -n 's/^ANOLIS_API_TOKEN=/Authorization: Bearer /p' /etc/anolis/runtime.env \
  | curl -s -H @- http://127.0.0.1:8080/v0/providers/health | jq
```

The commands here pipe through `jq`; install it with `sudo apt install jq`, or
drop the pipe.

There is one unit. Providers are **forked child processes** of the runtime, so
there is no `anolis-provider-*` service to check.

The install's own health check sends the token the same way. It confirms that `/v0/runtime/status` answers and
that `/v0/runs` answers 200, then reports provider liveness. The second matters because the run registry
fails soft — if its data directory cannot be created, the runtime logs one
warning at boot and then serves 503 on every run endpoint for as long as it
runs, while status stays green. The installed config sets
`runtime.data_dir: <prefix>/anolis-data`, somewhere the service user can write;
an install that warns `run registry NOT available` is one where that did not
hold.

For providers it reads `/v0/providers/health` until every provider is
`RUNNING` or `RECOVERING` or the health timeout elapses, then logs
`providers: N of M running` and one warning per provider that is not running,
with its id and lifecycle state. Like the run registry, a shortfall is a
warning and does not fail the install. It counts providers, not devices.

It also reports what `safety.on_startup` did at the runtime's restart. A safe
state that ran and did not take, such as on a board that came up tripped, is a
warning: `health: startup safe state NOT achieved`, then each failed and
skipped call. A safe state the config cannot express on this machine's devices
fails the install, with exit 1: the runtime marks such a call `cause: config`
(the zero rung cannot build its arguments, or the call names a device or
function that does not exist, or an argument out of range), and it fails the
same way on every restart and every e-stop. The install logs `health:
safety.safe_state cannot run on this machine's devices: <first refused call>`,
and the summary says the install did not succeed. The install has already
written `<prefix>/config/runtime.yaml`, and a re-run keeps it, so fix
`safety.safe_state` there and run the install again; or fix the project's
config and re-run with `--replace-config` (#616). A runtime before 0.2.0 marks
no call, so against one every failure stays a warning.

> **A provider's stdout is the protocol pipe, not the log.** The runtime `dup2`s
> each provider's stdout onto the ADPP framing channel; only **stderr** is
> inherited and reaches the journal. Provider diagnostics appear in
> `journalctl -u anolis-runtime` because they are written to stderr — anything a
> provider prints to stdout corrupts protocol framing.

### The startup preflight

Since v0.1.41 the runtime dry-runs declared safe-state calls against the live
registry once providers report capabilities, and logs the ones that cannot resolve
or type-check. It reports; it never refuses to start. Read it — but know its
limits:

- It checks `automation.mode_transition_hooks` **only when `automation.enabled` is
  true**. A fresh install is always inert, so on a freshly installed machine that half
  does not run. **Re-check the log after activating automation from Operate** —
  that is the first moment those hooks are examined.
- It runs at the end of provider initialisation, so **a provider that fails to
  start means no preflight at all.** The machine most likely to have a broken safe
  state is the one that reports nothing.
- Passing means *dispatchable*, not *will run*. Runtime gating — IDLE/AUTO mode,
  the actuation latch — is deliberately not simulated.
- The derived `zero` rung has no declared calls, so a machine relying on it gets a
  clean preflight having checked nothing.

## Host requirements

`install.sh` deploys anolis; it does not set up the host's hardware. Enabling a bus
(on a Raspberry Pi, the `config.txt` lines), loading modules, and giving the
`anolis` user access to device nodes belong to the project's host prep for its
platform, run first: host prep, then any reboot it asks for, then `install.sh`.
For a Raspberry Pi, bioreactor-v1's
[`prep-raspberrypi-os.sh`](https://github.com/anolishq/anolis-projects/blob/main/projects/bioreactor-v1/host/prep-raspberrypi-os.sh)
is a worked example to start from. The
`anolis` system user is a contract between the two: host prep may create it, and
`install.sh` accepts an existing one. `install.sh` never grants or revokes its
groups, so a user kept by `--uninstall` keeps the ones an earlier install or host
prep gave it, such as `dialout` and `gpio` from before #318. An install over an
existing user names its groups, and how to drop one:
`sudo gpasswd -d anolis <group>`.

Whatever the platform, the tools around host prep rely on it to:

- **Create the user as `install.sh` would, if it creates it:**
  `useradd --system --shell /usr/sbin/nologin --home-dir <prefix> anolis`, where
  `<prefix>` is the install prefix (`/opt/anolis` by default). No
  `--create-home`, which would copy `/etc/skel` into the install tree.
- **Signal a pending reboot with `/run/reboot-required`**, the Debian convention.
  The workbench reports it after a deploy, so an operator knows to reboot before
  relying on hardware. `install.sh` does not read it. `/run` is tmpfs, so the
  reboot clears it.
- **Be safe to run again.** A second run changes nothing, so a guide can always
  say "run host prep" without asking whether it already ran.
- **Optionally, offer a check mode** that changes nothing and exits non-zero when
  something is missing, so the host can be confirmed before installing.

Before anything starts, `install.sh` checks the result. As `anolis`, from the
install prefix, it runs:

- every binary's `--version`. A binary the host cannot load (its glibc or
  libstdc++ is too old) fails here, with the loader's message naming the missing
  version.
- each provider's `--check-host <its config>` (executable profile v1 §6), and
  prints what the provider reports: each unmet requirement with its fix, and any
  the provider could not determine.

Unmet requirements stop the install. `--allow-unmet-host` installs anyway, for
example before the host-prep reboot; those providers then start up not ready and
report why in `/v0/providers/health`. A provider checks the host only at startup,
so once the host is fixed, restart the service (or reboot, which host prep may
ask for anyway). A binary that cannot
run always stops the install. A provider that gives no answer (one that predates
`--check-host`) is reported and skipped.

## What a provider release must provide

`install.sh` holds no list of providers. For each entry under
`components.providers` in `machine-profile.yaml` it downloads a release asset
and the release's `SHA256SUMS` from the `repo` that entry names, and the
requirements below are the whole contract. A provider from any organisation installs the same way as one from
anolishq: `repo` may be any `owner/repo` on GitHub.

For an entry with key `<kind>`, `repo` and `version`:

| requirement | detail |
| --- | --- |
| a GitHub release tagged `v<version>` | `version` is written without the `v`, e.g. `version: 0.4.1` downloads tag `v0.4.1`. |
| an asset named `anolis-provider-<kind>-<version>-linux-<arch>.tar.gz` | `<arch>` is `arm64` or `x86_64`: the names `install.sh` derives from the host (it maps `aarch64` to `arm64`) and the values `--arch` documents. Name the asset `arm64`, not `aarch64` or `amd64`. Publish one asset per architecture you support. |
| a `SHA256SUMS` asset listing every tarball | standard `sha256sum` output with bare file names, one line per `.tar.gz` asset, every architecture included. `install.sh` checks each downloaded tarball against it, and stops when the hash does not match, when the release has no `SHA256SUMS` (HTTP 404), when it has no line for the tarball, or when `SHA256SUMS` cannot be fetched (a refused connection, a 5xx). No downloaded tarball is used unverified. |
| the archive holds `bin/anolis-provider-<kind>` | at the archive root, with no top-level directory. It is the only file `install.sh` copies out. |
| `<kind>` uses `a-z`, `0-9` and `-` only | the runtime configs refer to the binary as `<prefix>/bin/anolis-provider-<kind>`, and staging refuses a config whose provider command does not resolve to a pinned kind. |
| `--version` exits `0` | the host preflight runs it as the service user, and a binary that cannot run stops the install. |
| `--check-host <config>` | recommended. Its result is printed and unmet requirements stop the install (see [Host requirements](#host-requirements)). A provider that gives no answer is reported and skipped. |

The flags are those of the
[executable profile](https://github.com/anolishq/anolis-protocol/blob/main/docs/profiles/anolis-executable-profile-v1.md),
which is the contract for the binary itself.

What is **not** required:

- Nothing else in the archive. Other files are ignored.
- No manifest file. `install.sh` does not read the release's
  `manifest.json`; it computes the bundle's own `checksums.sha256` after
  downloading.
- No naming by the runtime. The runtime starts whatever `command` a runtime
  config names; the asset pattern is `install.sh`'s alone.
- No config-schema asset. `install.sh` never fetches one. A composer that
  validates provider config does: the convention is a raw JSON asset
  `anolis-provider-<kind>-<version>-config-schema.json` holding the
  `--config-schema` envelope (executable profile §2), and a
  `anolis-provider-<kind>-<version>-config-schema-manifest.json` sidecar
  recording its sha256. Publish both if your provider's config is to be
  edited in a composer.

## The install walk

The shell suite (`tests/shell/`) stubs `systemctl`, `apt`, `curl` and the
filesystem, so it can't show that a real install works. The **Install walk**
workflow (`.github/workflows/install-walk.yml`) does that. It runs
`tests/e2e/install-walk.sh` as root on fresh GitHub runners, x86_64 and arm64,
with `tests/e2e/e2e-sim`: the simulator provider in place of hardware, pinned
to released runtime and provider binaries. It runs on pull requests that touch
`tools/install.sh` or the walk, and on release tags.

Each scenario asserts on `install.sh`'s exit code, its output and the runtime's
API:

| scenario | what it walks |
| --- | --- |
| `fresh` | `--with-observability`, then a variant switch with `--replace-config`, a restart, and `--uninstall` and a reinstall, with telemetry reaching InfluxDB after each |
| `recover` | an `--uninstall` by v0.1.53, which deleted the InfluxDB credentials, then a current install, which mints new ones |
| `upgrade` | the previous release's `install.sh` and pins, then the current ones |
| `workbench` | `anolis-provision install` from the released workbench, and what its summary shows |
| `safestate` | a safe state the sim fails at run time, which warns and exits 0, then one the runtime cannot plan on the sim, which exits 1 naming the refused call (#616) |

Each scenario ends by breaking what its last check depends on, the runtime's
InfluxDB token or grafana-server, and requires that check to fail; `safestate`
instead installs a plannable safe state and requires exit 0, so its exit 1 is
the safe state's. A check that can't fail fails the walk.

It's the gate the bench follows: a change to `install.sh` walks here before it
walks on a rig. To run one scenario on a disposable host:
`sudo tests/e2e/install-walk.sh fresh`.
