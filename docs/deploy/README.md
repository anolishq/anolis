# Deployment

## One engine

Every supported route to a running machine ends in the same place: **`install.sh`**,
the single provisioning engine. It takes a *config directory* — a
`machine-profile.yaml` with a `config/` and `behaviors/` beside it — resolves the
component versions that profile pins, assembles a bundle, and installs it.

The surfaces above it are conveniences, not alternatives:

| route | what it is | guide |
| --- | --- | --- |
| `install.sh` directly | Headless. Online from a config, or stage a bundle elsewhere and install it with nothing but a shell on the target. | [install-sh.md](install-sh.md) |
| `anolis-provision` | A CLI wrapper that manages projects and adds SSH provisioning of a remote machine. Delegates the install itself. | [workbench-ssh.md](workbench-ssh.md) |
| Workbench UI | A browser tool that authors the config and drives the same path. | [workbench-ssh.md](workbench-ssh.md) |
| Build from source | For working on the runtime or a provider. | [developer.md](developer.md) |

Because there is one engine, a machine provisioned through the UI is
indistinguishable from one provisioned by hand. There is no "workbench install"
to migrate away from, and nothing the UI can do that the script cannot.

## What a deployment is

A deployment is **a config, not a version**. `machine-profile.yaml` pins the
runtime and each provider; `install.sh` reads those pins. Upgrading a machine
means editing the pins and re-running, not passing a different flag.

## What gets installed

One systemd unit — **`anolis-runtime.service`**. Providers are **forked child
processes** of the runtime, not separate units, so their diagnostics reach
`journalctl -u anolis-runtime` — via **stderr**, since a provider's stdout is the
protocol pipe. Optional extras add their own units:
`anolis-telemetry-export.service`, and InfluxDB and Grafana if you ask for
observability.

## Before you deploy anything

Read [safety.md](../safety.md), in particular the stop categories. The short
version: **`POST /v0/estop` is a software stop, not an emergency stop.** It
commands devices that stay powered and it cannot run if the runtime, a provider,
or the bus has failed. Any machine that can hurt someone or destroy a batch needs
an emergency stop in hardware, and that is outside everything documented here.
