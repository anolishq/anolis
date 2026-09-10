# Deploying a development build

For working *on* the runtime or a provider. Building is covered in
[getting-started.md](../getting-started.md); this is about getting what you built
onto a rig and iterating on it.

## Two ways to run a dev build

**Run it in place.** The runtime takes a config path, so a built binary and a
config directory are enough — no install, no systemd, no root:

```bash
./build/dev-release/core/anolis-runtime --config /path/to/runtime.yaml
```

Provider paths in that config are resolved relative to the working directory, so
a dev config can point straight at sibling build trees. This is the fastest loop
and the one to use while changing code.

**Install it like a real deployment.** When you need to test the deployed shape —
systemd, the `anolis` user, the install prefix, log routing — use
[install.sh](install-sh.md) rather than approximating it by hand. Approximations
are where "works on my box" comes from.

## Check a config without hardware

```bash
./build/dev-release/core/anolis-runtime --check-config /path/to/runtime.yaml
```

Know what this does and does not catch. It validates structure and exits **before
any provider starts**, so no `ArgSpec` exists yet and it **cannot check the
arguments of a declared hook**. A safe-state call with a wrong argument name
passes `--check-config` cleanly and fails at the moment it is needed.

The runtime's **startup preflight** is what covers that gap: it dry-runs every
declared `safety.safe_state` and `automation.mode_transition_hooks` call against
the live registry once providers report capabilities. Start the runtime against
the real inventory and read that output — it is the only check that exercises
those calls before an emergency does.

## Iterating against a rig

Providers are child processes of the runtime, so restarting the runtime restarts
them. There is no per-provider unit to bounce:

```bash
sudo systemctl restart anolis-runtime
journalctl -u anolis-runtime -f
```

If you installed with `install.sh`, `--rollback` restores the previous binaries
from `<prefix>/.prev` and restarts. It restores **binaries only** — config is left
as it is.

## Building a bundle for a target you do not have

`--stage` needs no root and no target, and cross-stages by architecture:

```bash
./install.sh --stage ./out --arch arm64 --project /path/to/bioreactor-v1
```

Useful for checking that a config assembles and that its pinned components
resolve, without touching a Pi.

## Before you point a dev build at real actuators

Read [safety.md](../safety.md). Two things developers get wrong most often:

**A machine can satisfy the AUTO gate and still have a software stop that drives
nothing.** The gate asks for a `-> FAULT` mode-transition hook; `POST /v0/estop`
runs `safety.safe_state`. They are separate declarations, and declaring only the
first leaves the stop inert — while its latch suppresses the hook you did declare.

**The software stop is not an emergency stop.** It commands devices that stay
powered, and it cannot run if the runtime, a provider or the bus has failed —
which is most of the ways a dev build fails. Keep a hardware stop within reach.
