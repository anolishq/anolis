# Deploying a development build

For working *on* the runtime or a provider. Building is covered in
[getting-started.md](../getting-started.md); this is about getting what you built
onto a rig and iterating on it.

## Two ways to run a dev build

**Run it in place.** The runtime takes a config path, so a built binary and a
config directory are enough — no install, no systemd, no root:

```bash
cmake --preset dev-release && cmake --build --preset dev-release
./build/dev-release/core/anolis-runtime --config /path/to/runtime.yaml
```

(`just` builds `ci-linux-release` by default, so ask for the `dev-release` preset
explicitly if that is the path you want.)

Provider paths in that config are resolved relative to the working directory, so
a dev config can point straight at sibling build trees. This is the fastest loop
and the one to use while changing code.

**That trick is dev-only.** The runtime `execv`s a provider — there is no `PATH`
lookup — and the installed unit sets `WorkingDirectory=<prefix>`, so a relative
provider path resolves against the install prefix. A sibling-build-tree path
copied from a dev config into an installed `runtime.yaml` points at nothing; use
absolute `<prefix>/bin/` paths. `install.sh --project` rewrites the standard
`../anolis-provider-<name>/build/<preset>/anolis-provider-<name>` path to
`<prefix>/bin/anolis-provider-<name>` when it assembles the config.

**Install it like a real deployment.** When you need to test the deployed shape —
systemd, the `anolis` user, the install prefix, log routing — use
[install.sh](install-sh.md) rather than approximating it by hand. Approximations
are where "works on my box" comes from.

## Check a config without hardware

```bash
./build/dev-release/core/anolis-runtime --check-config /path/to/runtime.yaml
```

Know what this does and does not catch. It validates structure — including
rejecting a non-scalar or out-of-range argument value — but exits **before any
provider starts**, so no `ArgSpec` exists and it **cannot check argument names or
types against the provider's declaration**. A safe-state call with a misspelled
argument name passes `--check-config` cleanly and fails at the moment it is needed.

The runtime's **startup preflight** covers part of that gap: it dry-runs declared
safe-state calls against the live registry once providers report capabilities. Its
limits are described in [install-sh.md](install-sh.md#the-startup-preflight), and
two of them matter here — the `mode_transition_hooks` half runs only when
`automation.enabled` is true, and the whole preflight is skipped if a provider
fails to start. It also validates *dispatchability*, not that a call will run:
runtime gating such as the actuation latch is deliberately not simulated, which is
exactly why a declared `-> FAULT` hook can pass preflight and still be refused
during a software stop whose ladder runs a rung.

## Iterating against a rig

Providers are child processes of the runtime, so restarting the runtime restarts
them. There is no per-provider unit to bounce:

```bash
sudo systemctl restart anolis-runtime
journalctl -u anolis-runtime -f
```

If you installed with `install.sh`, `--rollback` restores the previous binaries
from `<prefix>/.prev` and restarts. It restores **binaries only**, plus the
`manifest.json` saved with them — config is left as it is. It names the
runtime version it restores before restoring it (with `--dry-run`, without
restoring), from `<prefix>/.prev/meta/`, which the backup step fills. A backup
taken by an older `install.sh` has no record there, and the version is
reported as unknown.

Note that a deploy driven through the workbench uses the `install.sh` from the
*pinned runtime release*, not your working copy. Editing `tools/install.sh`
locally does not change what a remote provision runs.

## Building a bundle for a target you do not have

`--stage` needs no root and no target, and cross-stages by architecture:

```bash
tools/install.sh --stage ./out --arch arm64 --project /path/to/bioreactor-v1
```

Useful for checking that a config assembles and that its pinned components
resolve, without touching a Pi.

## Before you point a dev build at real actuators

Read [safety.md](../safety.md). Two things developers get wrong most often:

**A machine can satisfy the AUTO gate and still have a software stop with no
safe state of its own.** The gate asks for a `-> FAULT` mode-transition hook;
`POST /v0/estop` runs `safety.safe_state`. They are separate declarations, and
declaring only the first leaves the stop's ladder driving nothing: it runs the
hook you did declare, when it matches the mode the stop was pressed in (in
IDLE, only an `after_transition` hook runs), and reports
`safe_state_achieved: false`.

**The software stop is not an emergency stop.** It commands devices that stay
powered, and it cannot run if the runtime, a provider or the bus has failed —
which is most of the ways a dev build fails. Keep a hardware stop within reach.
