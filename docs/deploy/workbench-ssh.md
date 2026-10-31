# Deploy a remote machine over SSH

Provision a headless Pi from your workstation. The workstation materialises the
machine config, pushes it with a copy of `install.sh`, and runs the **same
`install.sh`** on the target.

Both surfaces below do exactly that — neither is a separate install mechanism.

> **The target needs internet.** This route runs `install.sh --project` on the
> target, which is its *online* mode: the target downloads the pinned components
> itself. What it saves you is a keyboard and a screen, not a network.
>
> For a genuinely offline target, stage a bundle and carry it — see
> [install-sh.md](install-sh.md#offline-stage-carry-install), or use
> `anolis-provision bundle`.

## Prerequisites

On the **workstation**:

```bash
pipx install anolis-workbench
```

Current Debian, Ubuntu and Homebrew Python refuse a system-wide `pip install`;
`pipx` installs the commands into their own environment. A virtual environment
works too. `paramiko`, which provisioning from the browser UI needs, is a core
dependency, so no extra is required. The target does not need the workbench installed.

The workstation also needs internet: it fetches `install.sh` from the pinned
runtime release.

On the **target**:

- **Passwordless sudo.** Both surfaces run SSH non-interactively — the CLI uses
  `BatchMode=yes`, and the UI opens no PTY — so a sudo password prompt does not
  fail, it *hangs* until the timeout.
- **A known host key.** The UI rejects unknown hosts outright. SSH in from a
  terminal once to accept the key before provisioning from the browser.

## Option A — the browser UI

```bash
anolis-workbench
```

Starts a local commissioning server on `127.0.0.1:3010` and opens a browser.
`anolis-workbench` is a server launcher — it takes `--host`, `--port`,
`--no-browser` and `--version`, nothing else. The work happens in the UI.

It is the better surface for *building* a config, because it validates as you
author rather than at install time. It can also deploy an existing canonical
config directory: import it from the home screen, then deploy it from
Commission. The CLI equivalent is `--system` below.

> Running the workbench **on a Pi** changes one default: it does not open a
> browser. It still binds `127.0.0.1`; reach it from another machine with
> `ssh -L 3010:127.0.0.1:3010 <user>@<pi>`.

## Option B — the CLI

```bash
anolis-provision remote \
  --target pi@192.168.1.50 \
  --system /path/to/bioreactor-v1 \
  --project bioreactor-v1
```

`--system` takes an existing canonical config directory — a real machine is
imported, not seeded. `--template` seeds a *new* project instead; the two are
mutually exclusive.

| flag | effect |
| --- | --- |
| `--target` | `user@host`. Required. |
| `--system` / `--template` | Import an existing config, or seed a new one. |
| `--project` | Project name to create on the target. |
| `--key`, `--port` | SSH private key; non-standard SSH port. |
| `--install-prefix` | Match a non-default prefix on the target. |
| `--no-start` | Install without starting the runtime. |
| `--with-telemetry-export`, `--with-observability`, `--allow-unmet-host` | Passed through to the install. |
| `--variant` | Runtime variant to install, a key of the project's `runtime_profiles` (passed as `install.sh --variant`; default `manual`). |
| `--force` | **Destructive, locally.** Deletes and recreates the workstation's copy of the project under `~/.anolis/systems/`. For a commissioned rig that means losing its configs, behaviours and pre-migration backup. It does not touch the target. |

**`remote` has no `--dry-run`**, unlike `install`, `fleet` and `update`. There is
no rehearsal for this command.

Other subcommands: `install` (this machine), `bundle`, `rollback`, `fleet`,
`workbench-service`, `check-update`, `update`. `rollback` wraps
`install.sh --rollback`, but resolves the latest release to fetch the script,
so unlike the raw script it needs network.

## What lands on the target

Exactly what [install-sh.md](install-sh.md) describes: one
`anolis-runtime.service`, providers as its forked child processes, config under
the install prefix. A machine provisioned this way is indistinguishable from one
provisioned by hand, so every operational instruction in that guide applies.

That includes the hostname. An arm64 target with a device-tree serial, such as
a Pi, is renamed `anolis-<last 8 of serial>`, and `remote` has no
`--keep-hostname`, so after provisioning reach it by that name
(`anolis-<serial>.local`) or by its IP address.

## Verifying from the workstation

```bash
ssh pi@192.168.1.50 'systemctl status anolis-runtime'
ssh pi@192.168.1.50 'journalctl -u anolis-runtime -n 50'
```

The install renders the config for the LAN: a config that sets
`bind: 127.0.0.1` is served on `0.0.0.0` with auth on, so the API answers at
`http://<host>:8080` from any machine on the network, over plain HTTP, to a
client with the token. To keep it off the network, leave `bind` out of the
config before provisioning; see [Security](../security.md#defaults). A loopback
runtime is reached over a tunnel:

```bash
ssh -L 8080:127.0.0.1:8080 pi@192.168.1.50
```

With auth on, a request through the tunnel needs the API token too: it
arrives on the device's loopback, and loopback is not exempt unless
`http.auth_exempt_loopback` is set. Copy the header into a file only you can
read, and have curl read it from there, so the token stays off the command
line:

```bash
(umask 077; ssh pi@192.168.1.50 \
  "sudo sed -n 's/^ANOLIS_API_TOKEN=/Authorization: Bearer /p' /etc/anolis/runtime.env" \
  > ~/.anolis-pi-auth-header)
curl -s -H @"$HOME/.anolis-pi-auth-header" http://127.0.0.1:8080/v0/providers/health | jq
```

## A caution about upgrades

Re-provisioning an existing machine **preserves its `runtime.yaml`**, so a variant
change does not take effect and an operator's edits survive — and the inert check
drops to warn-only. Same behaviour, same consequences, as
[install-sh.md](install-sh.md#choosing-a-runtime-variant).
