# Deploy a remote machine over SSH

The route for a Pi with no keyboard and no internet: your workstation downloads
the components and pushes them over SSH, then runs the **same `install.sh`** on
the target.

Both surfaces below do exactly that. Neither is a separate install mechanism —
`core/deploy.py` hands the config directory to `install.sh` and lets it work.

## Install the workbench on your workstation

```bash
pip install anolis-workbench
```

Not on the Pi. The point of this path is that the target needs nothing but SSH.

## Option A — the browser UI

```bash
anolis-workbench
```

That starts a local commissioning server on `127.0.0.1:3010` and opens a browser.
`anolis-workbench` is a server launcher and takes only `--host`, `--port` and
`--no-browser`; the work happens in the UI.

Use it to author or import a machine config, then provision the target from the
same place. It is the better surface for building a config, because it validates
as you author rather than at install time.

## Option B — the CLI

```bash
anolis-provision remote \
  --target pi@192.168.1.50 \
  --system /path/to/bioreactor-v1 \
  --project bioreactor-v1
```

`--system` takes an existing canonical config directory — a real machine is
imported, not seeded from a template. `--template` seeds a *new* project instead,
and the two are mutually exclusive.

Also available: `--key` for an SSH private key, `--port` for a non-standard SSH
port, `--install-prefix` to match a non-default prefix on the target, and
`--force` to overwrite an existing local workspace project.

`anolis-provision` has further subcommands — `install` (this machine), `bundle`,
`rollback`, `fleet`, `check-update` and `update`. `rollback` is a thin wrapper
over `install.sh --rollback`.

## What lands on the target

Exactly what [install-sh.md](install-sh.md) describes: one
`anolis-runtime.service`, providers as its child processes, config under the
install prefix. A machine provisioned this way is indistinguishable from one
provisioned by hand, so every operational instruction in that guide applies here
too.

## Verifying from the workstation

```bash
ssh pi@192.168.1.50 'systemctl status anolis-runtime'
ssh pi@192.168.1.50 'journalctl -u anolis-runtime -n 50'
```

The runtime binds `127.0.0.1` by default, so reach its HTTP surface over an SSH
tunnel rather than exposing it:

```bash
ssh -L 8080:127.0.0.1:8080 pi@192.168.1.50
curl -s http://127.0.0.1:8080/v0/providers/health | jq
```

## A caution about upgrades

Re-provisioning an existing machine **preserves its `runtime.yaml`**, which means
a variant change does not take effect and an operator's edits survive. That is
deliberate, and it is the same behaviour described in
[install-sh.md](install-sh.md#choosing-a-runtime-variant) — worth knowing before
you assume a redeploy reset anything.
