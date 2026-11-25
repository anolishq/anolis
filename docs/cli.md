# Command line

```text
anolis-runtime [--config PATH] [--check-config PATH] [--version] [--help]
```

| Option | Effect |
| --- | --- |
| `--config PATH`, `--config=PATH` | Run with this config file. Default: `anolis-runtime.yaml` in the current directory. |
| `--check-config PATH` | Load and validate the config file, and with automation enabled its behaviour tree, then exit without starting anything. |
| `--version`, `-V` | Print `anolis-runtime <version>` and exit. |
| `--help`, `-h` | Print the options and exit. |

Any other argument is an error: the runtime prints `Unknown argument: <arg>`
and exits with code 1.

A relative path in the config, such as a provider's `command`, is resolved
against the current directory, not the config file's directory. Two are
resolved against the config file's directory instead: `runtime.data_dir`, and
`automation.behavior_tree`. A behaviour tree found only relative to the current
directory still loads, with a deprecation warning. One that exists in both
places loads from beside the config file, with a warning naming both.

## Checking a config

```bash
anolis-runtime --check-config anolis-runtime.yaml
```

On success it prints a summary line, starting `Config valid:`, and exits 0.
On failure it prints `ERROR:` with the reason and exits 1.

With `automation.enabled: true` it also loads `automation.behavior_tree` with
the node set the runtime registers, so a missing file, a file that is not XML,
an unregistered node or malformed `CallDevice` args fail the check with
BehaviorTree.CPP's reason, and a node carrying an underscore attribute the
runtime does not run fails it naming the node and the attribute.

The loaded tree is then checked against the config. A `GetParameter`,
`GetParameterBool` or `GetParameterInt64` whose `param` names a parameter
`automation.parameters` does not declare, or one of a type the node does not
read, fails the check, as does a `CallDevice`, `ReadSignal` or
`CheckQuality` whose `device_handle` names a provider not in `providers[]`.
A port that takes its value from the blackboard (`{key}`) is not checked,
and the output says how many were not. The runtime refuses to start on the
same findings.

A runtime built without automation prints `Behavior tree not checked: this
runtime is built without automation`.

It does not start providers, so it cannot catch a provider's own config
errors; check those with the provider's `--check-config`. Nor does it check
that the devices and functions a tree names exist: those come from the
providers at start.

## Environment variables

| Variable | Used when |
| --- | --- |
| `ANOLIS_API_TOKEN` | `http.auth_enabled` is true and the config sets no `http.auth_token` |
| `INFLUXDB_TOKEN` | `telemetry.enabled` is true and the config sets no InfluxDB token |

`install.sh` writes `ANOLIS_API_TOKEN` into `/etc/anolis/runtime.env` for the
systemd unit, and with `--with-observability` also `INFLUXDB_TOKEN`.

## Stopping

`SIGINT` (Ctrl+C) and `SIGTERM` stop the runtime: it stops the HTTP server and
shuts down each provider, waiting up to `runtime.shutdown_timeout_ms`.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Normal shutdown, or `--check-config`, `--version` or `--help` succeeded |
| 1 | Unknown argument, missing or invalid config, or a startup failure |

The signal handlers are installed once startup finishes, so `SIGINT` or
`SIGTERM` while providers are still starting ends the process by the signal
instead ([#559](https://github.com/anolishq/anolis/issues/559)).
