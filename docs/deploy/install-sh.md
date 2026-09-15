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
only route that requires nothing of the target but a shell.

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
| `--prefix <path>` | Override the install prefix (default `/opt/anolis`). |
| `--rollback` | Restore the previous binaries from `<prefix>/.prev` and restart. |
| `--uninstall` | See the warning below. |

`--rollback` restores **binaries only**; config is left alone. The backup is taken
on each install that actually changes the binaries — an install of a byte-identical
`bin/` deliberately does not overwrite the backup, so `--rollback` still reaches
the last *different* install.

> **`--uninstall` is destructive beyond the binaries.** It stops, disables and
> removes every `anolis-*` unit, then removes the prefix — which includes
> `<prefix>/projects/<profile>`, your installed config and behaviours — and deletes
> the environment files holding the runtime's API token. Recorded data is kept:
> the run journal in `<prefix>/anolis-data` (when it holds anything), observability
> data, packages and the system user. The uninstall prints the purge command for
> what it kept. Back up your config directory first if it is not also held
> elsewhere.

## After installing

```bash
systemctl status anolis-runtime
journalctl -u anolis-runtime -f
curl -s http://127.0.0.1:8080/v0/providers/health | jq
```

There is one unit. Providers are **forked child processes** of the runtime, so
there is no `anolis-provider-*` service to check.

The install's own health check confirms two things: `/v0/runtime/status`
answers, and `/v0/runs` answers 200. The second matters because the run registry
fails soft — if its data directory cannot be created, the runtime logs one
warning at boot and then serves 503 on every run endpoint for as long as it
runs, while status stays green. The unit starts the runtime in `<prefix>` so the
default `anolis-data/` directory resolves somewhere the service user can write;
an install that warns `run registry NOT available` is one where that did not
hold.

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

## Raspberry Pi note

I2C must be enabled and the GPIO-header bus present; the phase is skipped entirely
on x86_64. `install.sh` looks for `/dev/i2c-1` specifically, or an adapter naming
the ARM/BSC controller — a Pi with a display also publishes HDMI DDC adapters,
which are not the bus you want. If the dtparam is missing it appends it and tells
you a reboot is required before providers can open `/dev/i2c-1`.
