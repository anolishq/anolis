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

**This machine needs internet**, plus `curl`, `tar`, `sha256sum`, and `python3`
with `pyyaml` to resolve the profile. Set `GITHUB_TOKEN` if you hit release
rate-limiting — that is the usual cause of a failed download.

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
the target: unpack the tarball and use the copy inside it, as above. This is the
only route that requires nothing of the target but a shell. `--stage` checks that
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
>   from Operate, upgrading restarts the machine still actuating, announced by one
>   yellow line in a long log.
>
> To switch variants, remove **`/opt/anolis/config/runtime.yaml`** and re-run, or
> edit it in place. (Under `--prefix`, that is `<prefix>/config/runtime.yaml`.)

## Optional components

| flag | effect |
| --- | --- |
| `--with-telemetry-export` | Installs `anolis-telemetry-export.service`, inert until secrets are provided. Online only. |
| `--with-observability` | Installs InfluxDB 2.x and Grafana as native apt packages with their own units, 30-day bucket retention and scoped tokens. Online only. |
| `--with-grafana-network` | With `--with-observability`, opens Grafana (:3000) to the network. Without it Grafana binds `127.0.0.1` and the summary prints the ssh tunnel to reach it; InfluxDB always binds `127.0.0.1`. See [Observability](../security.md#observability) in the security notes. |

Two things about observability that surprise people. Its services **start
immediately** — `--no-start` gates only the runtime. And if the vendor apt repos
are unreachable it **warns and skips** rather than failing: the runtime install
still succeeds, and you can re-run with the flag later once online.

Raise retention with `OBSERVABILITY_RETENTION`, but read the observability README
at the repo root on the trade-offs first.

## Other flags

| flag | effect |
| --- | --- |
| `--no-start` | Install without starting the runtime. Does not gate observability. |
| `--dry-run` | Print what would happen, change nothing. |
| `--allow-unmet-host` | Install even when a provider reports unmet host requirements. See [Host requirements](#host-requirements). |
| `--allow-downgrade` | Install even when the bundle's runtime is older than the installed one, which `bin/anolis-runtime --version` reports. Without it the install is refused before anything changes; `--rollback` is the way back to the previous install. `--dry-run --local` reports the comparison. |
| `--keep-hostname` | Keep the host's name. By default an arm64 host with a device-tree serial is renamed `anolis-<last 8 of serial>`, and `127.0.1.1` in `/etc/hosts` is pointed at the new name. With this flag neither happens, and the printed access URL, the mDNS name (`<host>.local`) and the telemetry host tags use the existing name. |
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
> removes every `anolis-*` unit, then removes the prefix — which includes
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
> is not an absolute path to an anolis install (it must hold `bin/anolis-runtime`),
> is a symlink, resolves to `/` or a top-level directory, is or holds `$HOME`, or
> has anything mounted at or below it. A refusal changes nothing. If the run
> journal is on its own mount, unmount it first.

## After installing

```bash
systemctl status anolis-runtime
journalctl -u anolis-runtime -f
curl -s http://127.0.0.1:8080/v0/providers/health | jq
```

With `http.auth_enabled: true` that request needs the API token too, even on
the device itself. This reads it from the root-only `runtime.env` and passes
it to curl on stdin, so it does not appear on the command line:

```bash
sudo sed -n 's/^ANOLIS_API_TOKEN=/Authorization: Bearer /p' /etc/anolis/runtime.env \
  | curl -s -H @- http://127.0.0.1:8080/v0/providers/health | jq
```

There is one unit. Providers are **forked child processes** of the runtime, so
there is no `anolis-provider-*` service to check.

The install's own health check sends the token the same way. It confirms that `/v0/runtime/status` answers and
that `/v0/runs` answers 200, then reports provider liveness. The second matters because the run registry
fails soft — if its data directory cannot be created, the runtime logs one
warning at boot and then serves 503 on every run endpoint for as long as it
runs, while status stays green. The unit starts the runtime in `<prefix>` so the
default `anolis-data/` directory resolves somewhere the service user can write;
an install that warns `run registry NOT available` is one where that did not
hold.

For providers it reads `/v0/providers/health` until every provider is
`RUNNING` or `RECOVERING` or the health timeout elapses, then logs
`providers: N of M running` and one warning per provider that is not running,
with its id and lifecycle state. Like the run registry, a shortfall is a
warning and does not fail the install. It counts providers, not devices.

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
  true**. An install is always inert, so on a freshly installed machine that half
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
platform, run first: host prep, then any reboot it asks for, then `install.sh`. The
`anolis` system user is a contract between the two: host prep may create it, and
`install.sh` accepts an existing one.

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
| a `SHA256SUMS` asset listing every tarball | standard `sha256sum` output with bare file names, one line per `.tar.gz` asset, every architecture included. `install.sh` checks each downloaded tarball against it, and stops when the hash does not match, when the release has no `SHA256SUMS` (HTTP 404), when it has no line for the tarball, or when `SHA256SUMS` cannot be fetched (a refused connection, a 5xx). Nothing is installed unverified. |
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
