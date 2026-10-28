# Concept

The Anolis runtime, `anolis-runtime`, is one process that runs on the machine
it controls. It presents every device on that machine the same way, whatever
the hardware behind it:

- **Signals** are readable state, such as a temperature or a relay's on/off
  state.
- **Functions** are calls that change something, such as setting a motor's
  PWM. The runtime validates each call before it reaches the device.

Operators and automation both work through these signals and functions.

[How the parts fit](https://anolishq.github.io/concepts/) on the docs site
covers the whole system, including the workbench and telemetry.

## The problem

A machine is usually built from devices that differ in hardware, bus and
firmware. Control logic written directly against each one has to change
whenever a device does, and one misbehaving driver can stop the whole
program.

## Providers, runtime and control path

A provider is a separate executable that talks to one kind of hardware. The
runtime starts it as a child process and exchanges protobuf messages with it
over its standard input and output. That contract is ADPP, the Anolis Device
Provider Protocol. See [Providers](providers.md).

The runtime discovers each provider's devices, polls their signals into a
state cache, and sends every device call through one router. It serves an
HTTP API under `/v0` for state, calls, modes, parameters and events. With
automation enabled, a behavior tree runs in `AUTO` mode and calls devices
through the same router. See [Architecture](architecture.md).

## Key properties

- A provider runs in its own process, so a provider crash does not take down
  the runtime. Automatic restart, with backoff and a circuit breaker, is
  opt-in per provider.
- HTTP state reads and the behavior tree read signal values from the state
  cache, not from providers. The exception is `GET /v0/providers/health`,
  which also asks each provider for its own health over ADPP (`GetHealth`).
- Every device call, from an operator, a behavior tree or the safe state,
  goes through the router.
- The runtime has no logic for particular device types. Two opt-in safety
  settings depend on what providers declare: a `safety.stop_chain` device
  with `expect: reports_estop` must publish a polled boolean signal named
  `estop`, and `safety.safe_state.zero_is_safe` drives the required numeric
  arguments of each function declared as actuating to 0. See
  [Safety](safety.md).

## What it is not

- Not real-time. Signals are polled, every 500 ms by default
  (`polling.interval_ms`), and the runtime gives no timing guarantee for a
  read or a call.
- Not a hardware emergency stop. Its software stop latches and drives outputs
  to a declared safe state; see [Safety](safety.md).
- Not an operator interface. The runtime has no UI of its own; the workbench
  and other clients use its HTTP API.
- Not a cloud service. It runs on the machine, and its HTTP server binds to
  `127.0.0.1` by default.
- Not a PLC replacement, a device driver or a firmware framework.

## Providers that exist

- [anolis-provider-bread](https://github.com/anolishq/anolis-provider-bread):
  BREAD RLHT relay and thermocouple boards and DCMT DC motor boards, on a
  Linux I2C bus.
- [anolis-provider-ezo](https://github.com/anolishq/anolis-provider-ezo):
  EZO sensor circuits (pH, ORP, conductivity, dissolved oxygen, temperature,
  humidity), on a Linux I2C bus.
- [anolis-provider-sim](https://github.com/anolishq/anolis-provider-sim):
  simulated devices, for running a machine without hardware. See the
  [simulator quickstart](quickstart-simulator.md).

bread and ezo also have mock buses for development without hardware.
