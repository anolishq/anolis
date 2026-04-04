# Providers

## What is a Provider?

A **provider** is a small executable that:

1. Speaks to specific hardware (sensors, actuators, PLCs, etc.)
2. Exposes devices via ADPP (Anolis Device Provider Protocol)
3. Runs as an isolated process (stdio communication)

## Provider Protocol (ADPP)

Communication via **stdin/stdout** with **uint32 length-prefixed protobuf messages**.

### Message Flow

```text
Runtime ────┐
            │ Request (protobuf, length-prefixed)
            ▼
         Provider
            │
            ▼ Response (protobuf, length-prefixed)
Runtime ◄───┘
```

### ADPP Operations

Required for baseline runtime integration:

1. **Hello**: Provider identifies itself
2. **ListDevices**: Returns device IDs
3. **DescribeDevice**: Returns capabilities (signals, functions)
4. **ReadSignals**: Returns current signal values
5. **Call**: Executes function (e.g., set_relay, move_motor)

Optional but recommended for production providers:

1. **GetHealth**: Reports provider and per-device health
2. **WaitReady**: Signals provider initialization readiness

See `external/anolis-protocol/spec/device-provider/protocol.proto` for schema.

## Example: anolis-provider-sim

Simulated provider with:

- **tempctl0**: Temperature controller (2 relays, temp/humidity sensors)
- **motorctl0**: Motor controller (speed, position, status)

Source: <https://github.com/FEASTorg/anolis-provider-sim>

Key files:

- `src/main.cpp`: stdio framing + message dispatch
- `src/handlers.cpp`: ADPP operation handlers
- `src/sim_devices.cpp`: Device state simulation

## Creating a Provider

### Minimal Steps

1. **Pick a language**: C++, Rust, Python - anything that can do stdio + protobuf
2. **Implement framing**: Read/write uint32_le length prefix
3. **Implement ADPP handlers used by runtime**: Hello, ListDevices, DescribeDevice, ReadSignals, Call (plus WaitReady/GetHealth when supported)
4. **Handle hardware**: Your code, your protocol (Modbus, SPI, etc.)

### Provider Template (pseudocode)

```cpp
while (true) {
    Request req = read_framed_stdin();

    Response resp;
    switch (req.type) {
        case HELLO:
            resp = handle_hello();
            break;
        case LIST_DEVICES:
            resp = handle_list_devices();
            break;
        case DESCRIBE_DEVICE:
            resp = handle_describe(req.device_id);
            break;
        case READ_SIGNALS:
            resp = handle_read(req.device_id, req.signal_ids);
            break;
        case CALL:
            resp = handle_call(req.device_id, req.function_id, req.args);
            break;
    }

    write_framed_stdout(resp);
}
```

### Rules

- **Stateless preferred**: Runtime caches state, you just read hardware
- **Blocking OK**: Runtime handles concurrency
- **Crash = unavailable**: Runtime marks devices offline (supervision may restart)
- **No stdin spam**: Only respond to requests
- **Quality matters**: Report STALE/FAULT when hardware fails

## Provider Supervision

The runtime can automatically monitor and restart crashed providers:

```yaml
providers:
  - id: hardware
    command: ./my-provider
    restart_policy:
      enabled: true
      max_attempts: 3
      backoff_ms: [200, 500, 1000]
      timeout_ms: 30000
```

### Crash Detection

The supervisor detects provider crashes when:

- Process exits unexpectedly
- ADPP operations timeout repeatedly
- Provider becomes unresponsive

### Restart Flow

1. **Crash Detected**: Supervisor logs crash with attempt counter
2. **Backoff Wait**: Delays restart according to `backoff_ms[attempt - 1]`
3. **Device Cleanup**: Clears all devices from registry before restart
4. **Process Restart**: Spawns new provider process
5. **Device Rediscovery**: Runs Hello → ListDevices → DescribeDevice for each device
6. **Recovery Tracking**: Resets crash counter on successful restart

### Circuit Breaker

After `max_attempts` consecutive crashes, the circuit breaker opens:

- No further automatic restarts
- Devices remain unavailable
- Manual intervention required (restart runtime or fix provider)

The circuit breaker resets when the provider successfully recovers.

### Supervision Observability

The runtime exposes real-time supervision state for every provider via `GET /v0/providers/health`.
`GET /v0/runtime/status` intentionally stays coarse (`AVAILABLE`/`UNAVAILABLE`) for compatibility;
use `/v0/providers/health` for restart/backoff detail.

**Key fields per provider:**

| Field              | Description                                                                                      |
| ------------------ | ------------------------------------------------------------------------------------------------ |
| `lifecycle_state`  | Additive lifecycle signal: `RUNNING`, `RECOVERING`, `RESTARTING`, `CIRCUIT_OPEN`, `DOWN`.        |
| `last_seen_ago_ms` | Milliseconds since the last healthy poll. Counts up while UNAVAILABLE. `null` before first poll. |
| `uptime_seconds`   | Seconds since the first healthy poll of the current process instance. `0` when UNAVAILABLE.      |
| `supervision`      | Supervision block — always an object, never null (even when policy is disabled).                 |

**Supervision state at each lifecycle stage:**

| Stage                       | `state`       | `lifecycle_state` | `attempt_count`  | `circuit_open` | `next_restart_in_ms`         |
| --------------------------- | ------------- | ----------------- | ---------------- | -------------- | ---------------------------- |
| Running normally            | `AVAILABLE`   | `RUNNING`         | `0`              | `false`        | `null`                       |
| Available but stabilizing   | `AVAILABLE`   | `RECOVERING`      | `> 0`            | `false`        | `0` or `null`                |
| Crashed, in backoff         | `UNAVAILABLE` | `RESTARTING`      | `>= 1`           | `false`        | positive integer (countdown) |
| Restart eligible now        | `UNAVAILABLE` | `RESTARTING`      | `>= 1`           | `false`        | `0`                          |
| Circuit open (max exceeded) | `UNAVAILABLE` | `CIRCUIT_OPEN`    | `> max_attempts` | `true`         | `null`                       |
| Down (no restart metadata)  | `UNAVAILABLE` | `DOWN`            | `0`              | `false`        | `null`                       |

**Distinguishing the two `null` cases for `next_restart_in_ms`:**

`next_restart_in_ms` is `null` in two situations: healthy (no crash) and circuit-open (no more restarts).
Always read `circuit_open` to tell them apart.

**Example — polling supervision from a script:**

```bash
# Wait until provider is available
until curl -sf http://127.0.0.1:8080/v0/providers/health | \
    jq -e '.providers[] | select(.provider_id=="sim0") | .state == "AVAILABLE"' > /dev/null; do
  sleep 0.5
done

# Check if circuit is open
curl -s http://127.0.0.1:8080/v0/providers/health | \
    jq '.providers[] | select(.provider_id=="sim0") | .supervision'
```

See [HTTP API Reference - GET /v0/providers/health](http-api.md#get-v0providershealth)
for the full field reference including `next_restart_in_ms` disambiguation.

### Backoff Strategy

The `backoff_ms` array defines delays before each restart attempt:

```yaml
# Conservative: Long delays for stable hardware
backoff_ms: [1000, 3000, 5000]

# Aggressive: Quick recovery for transient issues
backoff_ms: [100, 200, 500]

# Production: Balanced approach
backoff_ms: [200, 500, 1000]
```

### Best Practices

- **Enable for production providers**: Hardware can fail, supervision ensures resilience
- **Disable for development**: Crashes during development should stop execution for debugging
- **Tune backoff delays**: Match your hardware's restart characteristics
- **Set reasonable max_attempts**: Avoid infinite restart loops for permanently failed hardware
- **Monitor circuit breaker**: Alert when circuit opens (indicates persistent provider failure)

### Provider Internal State (Important)

Providers may maintain **ephemeral protocol state** required by hardware:

- Multi-step read sequences (e.g., CRUMBS staged reads: select → fetch)
- Communication buffers
- Hardware-specific state machines

Critical boundary:

- ✅ **Provider internal state**: Protocol implementation details
- ✅ **Core single source of truth**: Machine state visible to rest of system (StateCache)
- ❌ **Never**: Expose provider state directly to UIs/automation

Example: CRUMBS provider may buffer staged reads internally, but Anolis core remains authoritative for what the "current temperature" is.

## Provider Examples (Planned)

- `anolis-provider-modbus`: Modbus RTU/TCP devices
- `anolis-provider-arduino`: Arduino via serial
- `anolis-provider-canbus`: CAN bus devices
- `anolis-provider-crumbs`: FEAST CRUMBS integration
- `anolis-provider-ni`: National Instruments DAQ

## Testing Your Provider

Use `anolis-runtime`:

```yaml
# config/anolis-runtime.yaml
providers:
  - id: my_provider
    command: /path/to/my-provider
    args: ["--port", "/dev/ttyUSB0"]
```

Run and check logs for discovery, polling, and control operations.

## Safe Initialization Contract

**Critical Safety Principle**: Providers MUST initialize devices in a **safe, inactive state** on startup.
This ensures physical safety during runtime startup, configuration changes, and recovery scenarios.

### Provider Responsibilities

Providers must guarantee that when the process starts:

1. **No Actuation**: All actuators start in their safe, inactive position
2. **No Heating/Cooling**: Thermal controls start disabled
3. **No Motion**: Motors, stages, and moving parts start stationary
4. **No State Assumptions**: Don't assume hardware is already in a safe state
5. **Hardware Verification**: Query current hardware state and command it to safe defaults if needed

### Safe Default Definition

A **safe default** is the state a device should be in when:

- System is powered but not operating
- No automation is running
- No operator is actively controlling equipment
- Recovery from an error condition is in progress

### Example Safe States by Device Type

| Device Type              | Safe State                           | Implementation Notes                                 |
| ------------------------ | ------------------------------------ | ---------------------------------------------------- |
| **Relay/Switch**         | Open (de-energized)                  | Fail-safe: loss of power = safe                      |
| **Motor Controller**     | Duty cycle = 0, disabled             | Both PWM and enable signals off                      |
| **Temperature Control**  | Open-loop mode, heaters off          | Monitor-only until explicitly enabled                |
| **Linear Actuator**      | Position hold or retract to home     | Depends on mechanical fail-safe design               |
| **Valve**                | Closed (or safe position for system) | May be normally-open or normally-closed per hardware |
| **Laser/Light Source**   | Disabled, shutter closed             | Both electronic disable and mechanical safety        |
| **Vacuum Pump**          | Off                                  | Vent valve open if vacuum retention is unsafe        |
| **Pressure Regulator**   | Vent position, zero setpoint         | Depressurize system unless hold is explicitly safe   |
| **High Voltage Supply**  | Output disabled, voltage = 0         | Hardware-level disable, not just software            |
| **Communication Bridge** | Pass-through disabled                | Don't forward commands until runtime confirms ready  |

### Verification Checklist

Before deploying a provider, verify:

- [ ] **Startup behavior validated**: Provider process starts with no side effects
- [ ] **Hardware queried**: Current state is read before any commands issued
- [ ] **Safe state commanded**: Explicit commands sent to hardware to ensure safe defaults
- [ ] **Power-on-reset tested**: Provider works correctly after hardware power cycle
- [ ] **Crash recovery tested**: Provider restart doesn't cause unsafe transitions
- [ ] **Configuration validated**: Invalid config fails gracefully without actuating hardware
- [ ] **Emergency stop path**: Provider can be terminated safely at any time

### Runtime Integration

The anolis runtime coordinates safe startup:

1. **Runtime starts in IDLE mode** (control operations blocked)
2. **Providers initialize** (safe defaults applied)
3. **Device discovery runs** (capabilities advertised)
4. **Operator transitions to MANUAL** (enables control operations)
5. **Verification procedures run** (confirm safe operation)
6. **Operator transitions to AUTO** (enables automation)

This sequence ensures no actuation occurs until:

- Hardware is in a known safe state
- Operator has verified expected behavior
- Runtime mode explicitly permits operations

### Common Pitfalls

| Pitfall                                        | Consequence                                           | Mitigation                                                   |
| ---------------------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------ |
| Assuming hardware is already safe              | Startup after crash/reboot may find actuators enabled | Always command safe state explicitly                         |
| Not querying current state                     | May miss hardware faults or unexpected conditions     | Read before write; log discrepancies                         |
| Relying on hardware power-on defaults          | PCB redesign or component change breaks assumptions   | Verify safe state in provider code                           |
| Skipping safe init for "read-only" devices     | Misconfiguration could enable hidden control paths    | Always safe-init; hardware may have undocumented features    |
| Using config file for safety-critical settings | Config typo or version mismatch → unsafe state        | Hard-code safe defaults; config only for non-critical params |

### Example: anolis-provider-sim Compliance

See [FEASTorg/anolis-provider-sim > README > safe-initialization-in-provider-sim](https://github.com/FEASTorg/anolis-provider-sim?tab=readme-ov-file#safe-initialization-in-provider-sim)
for reference implementation.

## Provider Isolation Benefits

- **Crash safety**: Provider crash doesn't kill runtime
- **Language freedom**: Use best tool for hardware
- **Security**: No shared memory, limited blast radius
- **Testing**: Mock providers for CI/CD
