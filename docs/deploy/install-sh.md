# Deploy with `install.sh`

The canonical path. No Python on the target, no UI, and no network on the target
at all if you stage the bundle first.

`install.sh` needs root, except `--stage`, which builds a bundle and installs
nothing.

## Online: install from a config

```bash
sudo ./install.sh --project /path/to/bioreactor-v1
```

The config directory holds `machine-profile.yaml` with `config/` and `behaviors/`
beside it. `install.sh` reads the component pins from the profile, downloads those
versions, assembles a bundle, and installs it.

This machine needs internet and `python3` with `pyyaml` to resolve the profile.

## Offline: stage, carry, install

Build the bundle somewhere with internet — no root needed:

```bash
./install.sh --stage ./out --arch arm64 --project /path/to/bioreactor-v1
```

That writes `anolis-<profile>-<version>-<arch>.tar.gz` and a `.sha256` beside it,
and prints the two commands to run next. On the target:

```bash
sha256sum -c anolis-bioreactor-v1-0.1.40-arm64.tar.gz.sha256
sudo ./install.sh --local anolis-bioreactor-v1-0.1.40-arm64.tar.gz
```

Verify before installing. The sidecar is standard `sha256sum` output, so `-c`
works directly.

**The bundle carries its own `install.sh`.** You do not need this repository on
the target — unpack the tarball and use the copy inside it.

**A staged bundle bakes its install prefix.** `--local` refuses a `--prefix` that
differs from the one it was staged with; re-stage instead.

## Choosing a runtime variant

`--variant <key>` selects a `runtime_profiles` entry from the machine profile
(default: `manual`).

The variant must be **inert** — automation off, no mode-transition hooks. A
non-inert variant is refused, so an install cannot bring a machine up already
actuating. Turn automation on afterwards, from Operate.

> **On upgrade, an existing `runtime.yaml` is preserved and `--variant` is not
> applied.** This is deliberate: it protects a config an operator has edited or
> activated. But it means passing `--variant full` to an upgrade does nothing
> except log a warning. To switch variants, remove `/opt/anolis/runtime.yaml` and
> re-run, or edit it in place.

## Optional components

| flag | effect |
| --- | --- |
| `--with-telemetry-export` | Installs `anolis-telemetry-export.service`, inert until secrets are provided. Online only. |
| `--with-observability` | Installs InfluxDB 2.x and Grafana as native apt packages with their own units, a 30-day bucket retention and scoped tokens. Online only. |

Two things about observability that surprise people. Its services **start
immediately** — `--no-start` gates only the runtime. And if the vendor apt repos
are unreachable it **warns and skips** rather than failing: the runtime install
still succeeds, and you can re-run with the flag later once online.

Raise retention with `OBSERVABILITY_RETENTION`, but read
`observability/README.md` on the trade-offs first.

## Other flags

| flag | effect |
| --- | --- |
| `--no-start` | Install without starting the runtime. |
| `--dry-run` | Print what would happen, change nothing. |
| `--prefix <path>` | Override the install prefix (default `/opt/anolis`). |
| `--rollback` | Restore the previous binaries from `<prefix>/.prev` and restart. |
| `--uninstall` | Remove the installation. |

`install.sh` backs up the prior binaries on every install, so `--rollback` returns
you to the last one. It restores **binaries**, not config.

## After installing

```bash
systemctl status anolis-runtime
journalctl -u anolis-runtime -f
curl -s http://127.0.0.1:8080/v0/providers/health | jq
```

There is one unit. Providers are child processes of the runtime, so there is no
`anolis-provider-*` service to check — their output is in the runtime's journal.

**Watch the startup preflight.** Since v0.1.41 the runtime dry-runs every call
declared in `safety.safe_state` and `automation.mode_transition_hooks` against the
live registry once providers report their capabilities, and logs the ones that
cannot resolve or type-check. These calls otherwise run only during a mode
transition or a software stop, so a broken one would stay invisible until the
moment it was needed. The preflight reports; it never refuses to start. Read it.

## Raspberry Pi note

I2C must be enabled and the header bus present. `install.sh` detects the
GPIO-header bus specifically, and if the dtparam is missing it appends it and
tells you a reboot is required **before providers can open `/dev/i2c-1`**. A Pi
with a display publishes HDMI DDC adapters as `/dev/i2c-20` and `/dev/i2c-21`;
those are not the bus you want and the installer does not mistake them for it.
