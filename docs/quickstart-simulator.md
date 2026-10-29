# Try Anolis with the simulator

Run the runtime and the simulation provider on a Linux computer, without any
hardware, and control a simulated device over the HTTP API. This takes about
five minutes and installs nothing system-wide.

You need a Linux x86_64 or arm64 machine with `curl`, `tar` and
`sha256sum`. `python3` is used below only to pretty-print JSON.

## 1. Download the runtime and the simulation provider

Set the two versions to the latest releases of
[anolis](https://github.com/anolishq/anolis/releases/latest) and
[anolis-provider-sim](https://github.com/anolishq/anolis-provider-sim/releases/latest),
and the architecture to `x86_64` or `arm64`:

```bash
ANOLIS=0.5.0
SIM=0.2.8
ARCH=x86_64

mkdir anolis-try && cd anolis-try
curl -fsSLO https://github.com/anolishq/anolis/releases/download/v$ANOLIS/anolis-$ANOLIS-linux-$ARCH.tar.gz
curl -fsSL -o anolis.SHA256SUMS https://github.com/anolishq/anolis/releases/download/v$ANOLIS/SHA256SUMS
curl -fsSLO https://github.com/anolishq/anolis-provider-sim/releases/download/v$SIM/anolis-provider-sim-$SIM-linux-$ARCH.tar.gz
curl -fsSL -o sim.SHA256SUMS https://github.com/anolishq/anolis-provider-sim/releases/download/v$SIM/SHA256SUMS
sha256sum -c --ignore-missing anolis.SHA256SUMS
sha256sum -c --ignore-missing sim.SHA256SUMS
tar -xzf anolis-$ANOLIS-linux-$ARCH.tar.gz
tar -xzf anolis-provider-sim-$SIM-linux-$ARCH.tar.gz
```

Both checks print `OK`, and `bin/` now holds `anolis-runtime` and
`anolis-provider-sim`.

## 2. Configure

The provider's config declares the simulated devices. Save it as
`provider-sim.yaml`:

<!-- validate: sim-config -->

```yaml
devices:
  - id: tempctl0
    type: tempctl
    initial_temp: 20.0
  - id: motorctl0
    type: motorctl

simulation:
  mode: non_interacting
  tick_rate_hz: 10.0
```

The runtime's config starts that provider and serves the HTTP API on this
machine only. Save it as `anolis-runtime.yaml`:

<!-- validate: runtime-config -->

```yaml
http:
  enabled: true
  bind: 127.0.0.1
  port: 8080

providers:
  - id: sim0
    command: ./bin/anolis-provider-sim
    args: ["--config", "provider-sim.yaml"]
```

Check both:

```bash
./bin/anolis-provider-sim --check-config provider-sim.yaml
./bin/anolis-runtime --check-config anolis-runtime.yaml
```

## 3. Run

```bash
./bin/anolis-runtime --config anolis-runtime.yaml
```

Run it from inside `anolis-try`: the provider's `command` and `--config` paths
are relative to the directory you start the runtime in. It is ready when it
logs `Runtime Ready`; a few `WARN` lines about poll hints and an undeclared
safe state are expected with this config. Leave it running and use a second
terminal for the next steps. Stop it with Ctrl+C.

## 4. Read and control a device

List the devices. The simulator adds a `chaos_control` device for fault
injection alongside the two you configured:

```bash
curl -s http://127.0.0.1:8080/v0/devices | python3 -m json.tool
```

Read the temperature controller's signals:

```bash
curl -s http://127.0.0.1:8080/v0/state/sim0/tempctl0 | python3 -m json.tool
```

List the functions it accepts, with their ids and arguments:

```bash
curl -s http://127.0.0.1:8080/v0/devices/sim0/tempctl0/capabilities | python3 -m json.tool
```

Turn relay 1 on with `set_relay`, function 3:

```bash
curl -s -X POST http://127.0.0.1:8080/v0/call \
  -H 'Content-Type: application/json' \
  -d '{"provider_id": "sim0", "device_id": "tempctl0", "function_id": 3,
       "args": {"relay_index": {"type": "int64", "int64": 1},
                "state": {"type": "bool", "bool": true}}}'
```

The reply carries `"code": "OK"`. Read the state again and `relay1_state` is
`true`.

This runtime has automation off, so it is in `MANUAL` mode and accepts calls
straight away. With automation on, it starts in `IDLE` and refuses calls until
you switch modes; see [Automation](automation.md).

## Next

- [HTTP API](http-api.md): every endpoint and the value encoding
- [Configuration](configuration.md): the runtime config file
- [Simulated devices](https://github.com/anolishq/anolis-provider-sim/blob/main/docs/configuration.md):
  provider-sim's devices and simulation modes
- [Deployment](deploy/README.md): installing on a real machine as a service
- [Safety](safety.md): read before you connect anything that moves
