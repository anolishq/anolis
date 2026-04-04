# Anolis Automation Demo Walkthrough

Complete end-to-end demonstration of automation features.

## Prerequisites

- Anolis runtime and anolis-provider-sim built (Release configuration)
- InfluxDB running (optional, for telemetry visualization)
- Grafana running with Anolis dashboards (optional)

## Demo Scenario Overview

This walkthrough demonstrates:

1. **Startup in IDLE mode** - Safe default state
2. **Parameter inspection and updates** - Runtime configuration without code changes
3. **Mode transition to AUTO** - Enabling automated control
4. **Behavior tree execution** - Observing automated device orchestration
5. **Manual call policy enforcement** - Testing BLOCK vs OVERRIDE policies
6. **Fault handling** - Transitioning to FAULT mode and recovery
7. **Telemetry verification** - Viewing events in Grafana

## Step 1: Start the Runtime

Start the Anolis runtime with automation enabled:

```bash
# Linux
./build/dev-release/core/anolis-runtime --config=./config/anolis-runtime.yaml

# Windows
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config=.\config\anolis-runtime.yaml
```

**Expected Output:**

```text
[Runtime] Loading configuration from ./config/anolis-runtime.yaml
[Runtime] Starting in IDLE mode
[Runtime] HTTP server listening on 127.0.0.1:8080
[Runtime] Provider 'sim0' registered
[Runtime] Automation enabled, loading behavior tree
[Runtime] BT loaded: behaviors/demo.xml
[Runtime] Startup complete
```

**Verification:**

- Runtime starts without errors
- HTTP server is accessible at `http://127.0.0.1:8080`
- Default mode is IDLE (BT not running yet)

## Step 2: Check Runtime Status

Query the runtime status via HTTP API:

```bash
curl http://127.0.0.1:8080/v0/runtime/status
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "mode": "IDLE",
  "uptime_seconds": 3600,
  "polling_interval_ms": 500,
  "device_count": 2,
  "providers": [
    {
      "provider_id": "sim0",
      "state": "AVAILABLE",
      "device_count": 2
    }
  ]
}
```

**Observations:**

- Runtime is in `IDLE` mode (automation not running)
- Provider `sim0` is `AVAILABLE`
- `device_count` reports two registered devices for this provider

## Step 3: Inspect Current Parameters

Retrieve all runtime parameters:

```bash
curl http://127.0.0.1:8080/v0/parameters
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "parameters": [
    {
      "name": "temp_setpoint",
      "type": "double",
      "value": 25.0,
      "min": 10.0,
      "max": 50.0
    },
    {
      "name": "motor_max_duty",
      "type": "int64",
      "value": 80,
      "min": 0,
      "max": 100
    },
    {
      "name": "control_enabled",
      "type": "bool",
      "value": true
    }
  ]
}
```

**Observations:**

- Default parameters loaded from YAML configuration
- Each parameter has type constraints and current value

## Step 4: Update a Parameter

Change the temperature setpoint from 25.0°C to 30.0°C:

```bash
curl -X POST http://127.0.0.1:8080/v0/parameters \
  -H "Content-Type: application/json" \
  -d '{"name": "temp_setpoint", "value": 30.0}'
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "parameter": {
    "name": "temp_setpoint",
    "type": "double",
    "value": 30.0
  }
}
```

**Console Output:**

```text
[Runtime] Parameter 'temp_setpoint' changed: 25.0 -> 30.0
```

**Telemetry Event:**

- `parameter_change` event emitted to InfluxDB (if enabled)
- Visible as annotation in Grafana dashboards

## Step 5: Transition to MANUAL, then AUTO

Move into MANUAL for operator-approved control, then enable automation:

```bash
# Transition to MANUAL first
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode": "MANUAL"}'

# Then transition to AUTO
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode": "AUTO"}'
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "mode": "AUTO",
  "manual_gating_policy": "BLOCK"
}
```

**Console Output:**

```text
[Runtime] Mode change event emitted: IDLE -> MANUAL
[Runtime] Mode change event emitted: MANUAL -> AUTO
[BTRuntime] Mode changed to AUTO, starting tick loop
[BTRuntime] Tick 1: Running behavior tree
[BTRuntime] Reading signal: sim0/tempctl0/tc1_temp
[BTRuntime] Getting parameter: temp_setpoint (30.0)
[BTRuntime] Calling device function: sim0/tempctl0/set_target_temp(30.0)
```

**Observations:**

- Mode transitions follow the enforced path: IDLE → MANUAL → AUTO
- Behavior tree begins execution (tick loop started)
- BT reads current state, retrieves parameters, and calls device functions
- `mode_change` event emitted to telemetry

## Step 6: Observe Behavior Tree Execution

Watch the behavior tree orchestrate device control (view console or query state):

```bash
curl http://127.0.0.1:8080/v0/state/sim0/tempctl0
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "device": {
    "provider_id": "sim0",
    "device_id": "tempctl0"
  },
  "values": [
    {
      "signal_id": 1,
      "signal_name": "tc1_temp",
      "value": 28.5,
      "quality": "OK",
      "timestamp_ms": 1738713600000
    },
    {
      "signal_id": 2,
      "signal_name": "target_temp",
      "value": 30.0,
      "quality": "OK",
      "timestamp_ms": 1738713600000
    }
  ]
}
```

**Observations:**

- `target_temp` has been updated to 30.0°C (by the BT)
- `tc1_temp` is ramping toward the setpoint
- BT continues to run at configured tick rate (default 10 Hz)

## Step 7: Attempt Manual Call in AUTO Mode (BLOCK Policy)

Try to manually set the motor duty while in AUTO mode:

```bash
curl -X POST http://127.0.0.1:8080/v0/call \
  -H "Content-Type: application/json" \
  -d '{
    "provider_id": "sim0",
    "device_id": "motorctl0",
    "function_id": 10,
    "args": {
      "motor_index": {"type": "int64", "int64": 1},
      "duty": {"type": "double", "double": 0.75}
    }
  }'
```

**Expected Response (BLOCK policy):**

```json
{
  "status": {
    "code": "PERMISSION_DENIED",
    "message": "Manual call blocked: runtime is in AUTO mode (policy: BLOCK)"
  }
}
```

**Console Output:**

```text
[CallRouter] Manual call blocked in AUTO mode: sim0/motorctl0/set_motor_duty
```

**Observations:**

- Manual call is rejected because runtime is in AUTO mode
- BLOCK policy prevents manual intervention during automation
- This ensures BT has exclusive control during AUTO mode

## Step 8: Test OVERRIDE Policy (Optional)

Update the manual gating policy to allow overrides:

**Note:** This requires modifying `config/anolis-runtime.yaml`:

```yaml
automation:
  enabled: true
  behavior_tree: ./behaviors/demo.xml
  tick_rate_hz: 10
  manual_gating_policy: OVERRIDE # Changed from BLOCK
```

Restart the runtime and transition to AUTO again. Now retry the manual call:

```bash
curl -X POST http://127.0.0.1:8080/v0/call \
  -H "Content-Type: application/json" \
  -d '{
    "provider_id": "sim0",
    "device_id": "motorctl0",
    "function_id": 10,
    "args": {
      "motor_index": {"type": "int64", "int64": 1},
      "duty": {"type": "double", "double": 0.75}
    }
  }'
```

**Expected Response (OVERRIDE policy):**

```json
{
  "status": {
    "code": "OK"
  }
}
```

**Console Output:**

```text
[CallRouter] Manual call overriding AUTO mode: sim0/motorctl0/set_motor_duty
```

**Observations:**

- Manual call succeeds even in AUTO mode
- OVERRIDE policy allows human intervention
- Useful for tuning or emergency adjustments

## Step 9: Transition to FAULT Mode

Simulate a fault condition by switching to FAULT mode:

```bash
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode": "FAULT"}'
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "mode": "FAULT",
  "manual_gating_policy": "BLOCK"
}
```

**Console Output:**

```text
[Runtime] Mode change event emitted: AUTO -> FAULT
[BTRuntime] Mode changed to FAULT, stopping tick loop
```

**Observations:**

- Mode transition from AUTO → FAULT
- Behavior tree stops executing
- Manual calls are now allowed (FAULT mode permits recovery actions)
- `mode_change` event emitted to telemetry

## Step 10: Manual Recovery in FAULT Mode

Perform manual recovery operations:

```bash
# Read current state
curl http://127.0.0.1:8080/v0/state

# Manually adjust device (e.g., reset motor duty to 0)
curl -X POST http://127.0.0.1:8080/v0/call \
  -H "Content-Type: application/json" \
  -d '{
    "provider_id": "sim0",
    "device_id": "motorctl0",
    "function_id": 10,
    "args": {
      "motor_index": {"type": "int64", "int64": 1},
      "duty": {"type": "double", "double": 0.0}
    }
  }'
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  }
}
```

**Observations:**

- Manual calls are allowed in FAULT mode
- Operator can safely recover the system
- BT remains stopped until mode is changed

## Step 11: Return to MANUAL Mode

After recovery, transition back to safe MANUAL mode:

```bash
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode": "MANUAL"}'
```

**Expected Response:**

```json
{
  "status": {
    "code": "OK"
  },
  "mode": "MANUAL",
  "manual_gating_policy": "BLOCK"
}
```

**Console Output:**

```text
[Runtime] Mode change event emitted: FAULT -> MANUAL
```

**Observations:**

- System returns to safe MANUAL mode
- BT remains stopped
- Operator retains full manual control

## Step 12: Verify Telemetry in Grafana (Optional)

If InfluxDB and Grafana are running:

1. **Open Grafana** at `http://localhost:3000`
2. **Navigate to System Overview dashboard**
3. **Verify annotations:**
   - Red vertical lines mark mode changes (MANUAL → AUTO → FAULT → MANUAL)
   - Blue vertical lines mark parameter changes (temp_setpoint: 25.0 → 30.0)

**InfluxDB Query (mode changes):**

```flux
from(bucket: "anolis")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "mode_change")
  |> filter(fn: (r) => r._field == "previous_mode")
```

**InfluxDB Query (parameter changes):**

```flux
from(bucket: "anolis")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "parameter_change")
  |> filter(fn: (r) => r._field == "parameter_name")
```

**Expected Results:**

- At least 3 mode change events visible
- At least 1 parameter change event visible
- Events are timestamped and include metadata (old/new values)

## Summary

This demo demonstrated all automation features:

✅ **Runtime Modes** — MANUAL/AUTO/FAULT transitions with proper state machine
✅ **Behavior Tree Execution** — Automated device orchestration via BT
✅ **Parameters** — Runtime configuration and updates via HTTP API
✅ **Manual/Auto Contention** — BLOCK and OVERRIDE policies enforced
✅ **Telemetry Integration** — Mode and parameter events visible in Grafana

## Next Steps

- **Customize Behavior Tree:** Edit `behaviors/demo.xml` to implement custom control logic
- **Add Parameters:** Define new parameters in `config/anolis-runtime.yaml` for your use case
- **Integrate Real Hardware:** Replace `anolis-provider-sim` with a real hardware provider
- **Build Operator UI:** Implement web interface for mode control and parameter tuning

## Troubleshooting

**BT doesn't start:**

- Check `automation.enabled: true` in config
- Verify `behaviors/demo.xml` exists and is valid XML
- Check console for BT loading errors

**Manual calls always blocked:**

- Verify current mode: `curl http://127.0.0.1:8080/v0/mode`
- Check `manual_gating_policy` setting in config
- Ensure not in AUTO mode if policy is BLOCK

**No telemetry events:**

- Verify InfluxDB is running and accessible
- Check `telemetry.enabled: true` in config
- Confirm InfluxDB connection settings (host, port, bucket, token)

**Parameter updates rejected:**

- Check parameter constraints (min/max, allowed_values)
- Verify parameter name matches config exactly
- Ensure value type matches parameter type

## Architecture Notes

**Automation is Policy, Not Safety:**

- Automation layer orchestrates device calls but doesn't replace safety systems
- External E-stops, interlocks, and watchdogs are still required for real hardware
- BT provides repeatable behavior, not safety-rated control

**Layer Boundaries:**

- BT reads state via StateCache (no direct provider access)
- BT acts via CallRouter (all calls go through kernel validation)
- Automation sits cleanly above the kernel, not beneath it
