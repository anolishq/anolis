# Providers

## What a Provider Is

A provider is an external process that:

1. Talks to specific hardware.
2. Exposes devices via ADPP.
3. Communicates with runtime over framed stdio.

The runtime remains hardware-agnostic; hardware specifics live in providers.

## Protocol Boundary (ADPP)

Transport:

1. `stdin/stdout`
2. `uint32_le` length-prefixed protobuf messages

Baseline operations expected by runtime integration:

1. `Hello`
2. `ListDevices`
3. `DescribeDevice`
4. `ReadSignals`
5. `Call`

Recommended operations:

1. `GetHealth`
2. `WaitReady`

Schema source: `proto/anolis/deviceprovider/v1/envelope.proto` in the anolis-protocol repository, fetched at configure time with CMake FetchContent (release v1.2.0, pinned in `core/CMakeLists.txt`; set `FETCHCONTENT_SOURCE_DIR_ANOLIS_PROTOCOL` to use a local clone).

## Runtime/Provider Contract

1. Provider owns hardware IO and protocol state.
2. Runtime owns shared machine state (`StateCache`) and call validation (`CallRouter`).
3. External layers (HTTP/UI/automation) must go through runtime APIs, never directly to provider processes.

Practical consequences:

1. Provider crashes isolate to that provider.
2. Runtime can keep operating other providers.
3. Recovery behavior is driven by restart policy and supervision state.
4. Each provider runs in its own process group (a job object on Windows).
   Stopping a provider ends anything it started too, such as a wrapper
   script's worker, and a restart does not start the replacement while any
   of the old one is still running: that attempt fails, and supervision
   retries (#563). A provider gets no terminal signals such as Ctrl+C; the
   runtime catches them and stops each provider by closing its stdin.

## Provider Supervision

Configure per provider:

```yaml
providers:
  - id: hardware0
    command: ./my-provider
    restart_policy:
      enabled: true
      max_attempts: 3
      backoff_ms: [200, 500, 1000]
      timeout_ms: 30000
```

Behavior:

1. Crash/unresponsive provider is marked unavailable.
2. Runtime applies backoff then attempts restart. Each restart stops the old
   process before it starts the new one. The stop first waits for any request
   already in flight to the old process to finish, which can take up to twice
   that request's timeout (one for the write, one for the response: the
   provider's `timeout_ms`, `hello_timeout_ms` or `ready_timeout_ms`). It then closes
   stdin, waits up to `runtime.shutdown_timeout_ms`, force-kills the provider's
   PID and waits up to 500 ms. The new process starts after that whether or not
   the old one has exited, so a process stuck in uninterruptible I/O, or a
   child the provider spawned, can still be running beside it. The restart
   policy's `timeout_ms` starts once the stop returns, so none of the stop time
   counts against it.
3. Device inventory is rediscovered after restart.
4. Circuit opens after `max_attempts` consecutive failures.
5. While the circuit is open, the runtime makes one restart attempt (a
   half-open probe) every 60 seconds. The interval is fixed, not configurable.
   `supervision.circuit_open` stays `true` until a probe restarts the provider;
   a crash soon after that re-opens it.

Operational observability:

1. Use `GET /v0/providers/health` for lifecycle/supervision detail.
2. `GET /v0/runtime/status` is intentionally coarse (`AVAILABLE`/`UNAVAILABLE` only).

Key supervision fields:

1. `lifecycle_state`
2. `last_seen_ago_ms`
3. `uptime_seconds`
4. `supervision.attempt_count`
5. `supervision.circuit_open`
6. `supervision.next_restart_in_ms`

## Building a Provider

Minimal implementation steps:

1. Implement framed stdio transport.
2. Implement required ADPP operations.
3. Implement device capability descriptions with accurate arg constraints.
4. Map hardware errors to meaningful status/quality outputs.

Recommended quality bar:

1. Deterministic startup behavior.
2. Clear error propagation (no silent failure).
3. Bounded operation timeouts.
4. Predictable behavior under reconnect/restart.

## Testing a Provider

Use runtime with a provider entry in runtime YAML:

```yaml
providers:
  - id: my_provider
    command: /path/to/my-provider
    args: []
```

Then validate:

1. Discovery (`/v0/devices`, capabilities endpoint).
2. State quality/staleness (`/v0/state`, `/v0/providers/health`).
3. Call validation and control flow (`POST /v0/call`).
4. Restart/circuit behavior via forced provider failure.

## Safe Initialization Contract

Providers must initialize hardware to safe defaults on process start.

Required startup guarantees:

1. No unintended actuation.
2. No heat/power outputs enabled by default.
3. Motion channels initialized to inactive state.
4. Hardware state is queried and normalized to safe outputs.

Typical safe defaults:

1. Relays: de-energized.
2. Motors: duty/setpoint zero, disabled where supported.
3. Thermal outputs: disabled.
4. Pressure/vacuum/high-voltage outputs: disabled or safe vent state per hardware policy.

This contract is mandatory for hardware deployments.
