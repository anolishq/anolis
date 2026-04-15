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

Schema source: `external/anolis-protocol/spec/device-provider/protocol.proto`.

## Runtime/Provider Contract

1. Provider owns hardware IO and protocol state.
2. Runtime owns shared machine state (`StateCache`) and call validation (`CallRouter`).
3. External layers (HTTP/UI/automation) must go through runtime APIs, never directly to provider processes.

Practical consequences:

1. Provider crashes isolate to that provider.
2. Runtime can keep operating other providers.
3. Recovery behavior is driven by restart policy and supervision state.

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
2. Runtime applies backoff then attempts restart.
3. Device inventory is rediscovered after restart.
4. Circuit opens after `max_attempts` consecutive failures.

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
