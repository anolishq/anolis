# Safety Operations Guide

This guide establishes safety procedures for operating Anolis-controlled hardware systems.

## Core Safety Principles

1. **Safe by Default**: System starts in IDLE mode with control operations blocked
2. **Explicit Transitions**: All mode changes require deliberate operator action
3. **Fail-Secure**: Error conditions automatically transition to FAULT mode
4. **Operational Visibility**: Read-only queries work in all modes for diagnostics

---

## Startup Procedures

### Standard Startup Sequence

**Goal**: Safely bring system from power-on to automated operation

1. **System Initialization** (IDLE mode)
   - Runtime process starts in IDLE mode (enforced at startup, not configurable via runtime YAML)
   - Providers initialize with safe device defaults (relays off, motors stopped, heaters disabled)
   - Device discovery runs (capabilities advertised)
   - **Control operations blocked** (safety interlock active)

2. **Operator Verification** (remain in IDLE)
   - Check runtime status: `GET /v0/runtime/status`
   - Verify all expected devices discovered: `GET /v0/devices`
   - Inspect initial device states: `GET /v0/state/{provider}/{device}`
   - Confirm all actuators in safe states (relays off, motors stopped, etc.)
   - Review logs for any initialization warnings or errors

3. **Transition to MANUAL** (enable control)
   - **Decision point**: Operator confirms system ready for control operations
   - Execute mode transition: `POST /v0/mode` with `{"mode": "MANUAL"}`
   - Verify transition: `GET /v0/mode` confirms MANUAL mode
   - **Control operations now enabled**

4. **Manual Verification** (MANUAL mode)
   - Execute test sequences to verify device functionality
   - Check sensor readings for expected values
   - Perform calibration or homing procedures if required
   - Verify interlocks and safety systems (if applicable)
   - Document any anomalies or deviations

5. **Transition to AUTO** (enable automation)
   - **Decision point**: Operator confirms system ready for automated operation
   - Review behavior tree configuration and parameters
   - Execute mode transition: `POST /v0/mode` with `{"mode": "AUTO"}`
   - Monitor initial automation cycles for correct behavior
   - Remain accessible to intervene if needed

### Quick Start (Development/Testing)

For development and testing scenarios where hardware safety is not a concern:

1. Start runtime normally (it will still enter IDLE mode by default)
2. Verify devices discovered
3. Transition to MANUAL with `POST /v0/mode`
4. Proceed with testing

**⚠️ Warning**: Do not use quick start for:

- Production deployments
- Systems controlling hazardous equipment
- Integration with safety-critical processes
- Hardware that can cause injury or damage if mis-operated

---

## Mode Transition Procedures

### IDLE → MANUAL

**When to use**: Initial startup, recovery from FAULT, returning from standby

**Preconditions**:

- All providers available and devices discovered
- No active error conditions
- Operator has verified safe states

**Procedure**:

```bash
# 1. Verify current mode
curl http://localhost:8080/v0/mode

# 2. Check device states
curl http://localhost:8080/v0/state/{provider}/{device}

# 3. Transition to MANUAL
curl -X POST http://localhost:8080/v0/mode \\
  -H "Content-Type: application/json" \\
  -d '{"mode": "MANUAL"}'

# 4. Verify transition
curl http://localhost:8080/v0/mode
```

**Post-transition**: Control operations now allowed; operator responsible for all device commands

### MANUAL → AUTO

**When to use**: Enabling automated sequences

**Preconditions**:

- System verified functional in MANUAL mode
- Behavior tree configured and validated
- Automation parameters reviewed
- Area clear of personnel (if applicable)

**Procedure**:

```bash
# 1. Review automation parameters
curl http://localhost:8080/v0/automation/parameters

# 2. Ensure manual gating policy configured (BLOCK recommended)
# Check runtime config: automation.manual_gating_policy

# 3. Transition to AUTO
curl -X POST http://localhost:8080/v0/mode \\
  -H "Content-Type: application/json" \\
  -d '{"mode": "AUTO"}'

# 4. Monitor automation execution
curl http://localhost:8080/v0/runtime/status
```

**Post-transition**: Behavior tree running; manual calls gated by policy (BLOCK: rejected, OVERRIDE: allowed)

### AUTO → MANUAL (Normal Shutdown)

**When to use**: Graceful automation stop

**Procedure**:

```bash
# Transition to MANUAL
curl -X POST http://localhost:8080/v0/mode \\
  -H "Content-Type: application/json" \\
  -d '{"mode": "MANUAL"}'

# Verify automation stopped and devices in known state
curl http://localhost:8080/v0/state/{provider}/{device}
```

**Post-transition**: Behavior tree stopped; operator control resumed

### MANUAL → IDLE (Standby)

**When to use**: Putting system in safe standby (e.g., overnight, awaiting next operation)

**Procedure**:

```bash
# 1. Command all actuators to safe states manually first
curl -X POST http://localhost:8080/v0/call \\
  -H "Content-Type: application/json" \\
  -d '{
    "provider_id": "{provider}",
    "device_id": "{device}",
    "function_id": 10,
    "args": {}
  }'

# 2. Verify safe states
curl http://localhost:8080/v0/state/{provider}/{device}

# 3. Transition to IDLE
curl -X POST http://localhost:8080/v0/mode \\
  -H "Content-Type: application/json" \\
  -d '{"mode": "IDLE"}'
```

**Post-transition**: Control operations blocked; system in safe standby

### Any → FAULT (Automatic)

**Trigger**: System-detected error condition (provider crash, BT failure, hardware fault)

**Automatic actions**:

- Behavior tree stops immediately
- All control operations blocked
- Error state logged and recorded
- Read-only queries remain functional for diagnostics
- Operator intervention required to recover (transition to MANUAL)

### FAULT → MANUAL (Recovery)

**When to use**: Recovering from error condition

**Preconditions**:

- Root cause identified and resolved
- Hardware verified in safe state
- Provider(s) restarted if needed

**Procedure**:

```bash
# 1. Investigate fault condition
curl http://localhost:8080/v0/runtime/status
# Check logs for error details

# 2. Resolve root cause
# (Restart provider, fix configuration, address hardware issue, etc.)

# 3. Verify system state
curl http://localhost:8080/v0/devices
curl http://localhost:8080/v0/state/{provider}/{device}

# 4. Transition to MANUAL
curl -X POST http://localhost:8080/v0/mode \\
  -H "Content-Type: application/json" \\
  -d '{"mode": "MANUAL"}'

# 5. Perform verification tests
# (Test device functions, verify interlocks, etc.)
```

**⚠️ Critical**: Do not transition FAULT → AUTO directly. Recovery path must go through MANUAL for operator verification.

---

## Emergency Stop Procedures

### Immediate Stop

**Scenario**: Dangerous condition detected, immediate cessation required

**Level 1 - Application Stop**:

```bash
# Stop automation immediately (AUTO → MANUAL)
curl -X POST http://localhost:8080/v0/mode \\
  -H "Content-Type: application/json" \\
  -d '{"mode": "MANUAL"}'
```

**Level 2 - Process Termination**:

```bash
# Terminate runtime process
# Linux/macOS:
killall anolis-runtime

# Windows:
taskkill /IM anolis-runtime.exe /F
```

**Level 3 - Hardware Disconnect**:

- Disconnect provider process from hardware (provider-specific)
- Physical emergency stop button (if installed)
- Remove power from actuators/controllers

### Post-Emergency Actions

After emergency stop:

1. **Do not restart immediately** - Assess situation
2. **Inspect hardware** - Verify no damage occurred
3. **Review logs** - Identify trigger condition
4. **Document incident** - Record cause and response
5. **Verify safe state** - Confirm all actuators inactive before restart
6. **Follow [Standard Startup Sequence](#standard-startup-sequence)** - Do not skip verification steps

---

## Common Pitfalls and Mitigations

### Pitfall:Skipping IDLE verification

**Risk**: Starting control operations before confirming safe states

**Mitigation**:

- Always check device states in IDLE before transitioning to MANUAL
- Establish verification checklist for your specific hardware
- Use scripted verification procedures

### Pitfall: Assuming Hardware Power-On-Reset is Safe

**Risk**: Hardware may not initialize to safe state on power-up

**Mitigation**:

- Providers MUST explicitly command safe states on initialization
- Never assume hardware defaults are correct
- Verify safe states in provider code (see [Provider Safe Initialization Contract](providers.md#safe-initialization-contract))

### Pitfall: Direct FAULT → AUTO Transition

**Risk**: Resuming automation without resolving underlying issue

**Mitigation**:

- Runtime blocks FAULT → AUTO transition (must go through MANUAL)
- Always investigate and resolve fault cause before resuming automation
- Perform verification tests in MANUAL mode after fault recovery

### Pitfall: Manual Override During Automation

**Risk**: Manual control interfering with automated sequence

**Mitigation**:

- Use `manual_gating_policy: BLOCK` (default) to prevent manual calls in AUTO mode
- If OVERRIDE policy needed, establish clear SOPs for when manual intervention is permitted
- Log all manual overrides for post-operation review

### Pitfall: Ignoring Provider Unavailability

**Risk**: Automation continues with missing devices

**Mitigation**:

- Monitor provider health via `/v0/runtime/status`
- Configure provider supervision with `restart_policy` for automatic recovery
- Behavior trees should check device availability before use

### Pitfall: Insufficient Error Handling in Behavior Trees

**Risk**: BT failure causes unexpected system state

**Mitigation**:

- Use timeout nodes for all device calls
- Implement fallback/recovery logic in BT design
- Use preconditions to validate state before actuation
- Test fault injection scenarios (see [anolis-provider-sim fault injection](../anolis-provider-sim/README.md#fault-injection-api))

---

## Hardware Integration Safety Checklist

Use this checklist when integrating new hardware with Anolis:

### Provider Safety

- [ ] Provider initializes devices in safe states on startup
- [ ] Provider queries hardware state before commanding actuators
- [ ] Provider documents safe defaults in README
- [ ] Provider handles hardware communication errors gracefully
- [ ] Provider restart does not cause unsafe transitions

### Device Capability Documentation

- [ ] Device capabilities accurately reflect actual hardware (no missing functions)
- [ ] Function preconditions document safety requirements
- [ ] Function argument ranges match hardware safe operating limits
- [ ] Signal quality reporting works correctly (FAULT on error)

### Integration Testing

- [ ] Startup sequence tested (power-on → IDLE verification → MANUAL control)
- [ ] Emergency stop tested (AUTO → MANUAL transition)
- [ ] Provider crash recovery tested (supervision restart)
- [ ] Fault injection tested (device unavailable, function failure)
- [ ] Mode transitions tested (all valid paths)

### Operational Procedures

- [ ] Startup checklist created for operators
- [ ] Emergency stop procedure documented
- [ ] Fault recovery procedure documented
- [ ] Hardware-specific hazards identified and mitigated
- [ ] Training materials created for operators

### Supervision Configuration

- [ ] Provider restart policy configured (if appropriate for hardware)
- [ ] Restart backoff delays tuned for hardware initialization time
- [ ] Circuit breaker thresholds set to prevent infinite restart loops
- [ ] Monitoring/alerting configured for circuit breaker opens

---

## Safety vs Convenience Tradeoffs

### Development/Testing Environments

**Lower Safety Requirements**:

- May start in MANUAL mode (skip IDLE verification)
- OVERRIDE policy acceptable for manual intervention during automation
- Fewer verification steps

**Rationale**: Hardware is test equipment or simulation; consequences of error are low

### Production/Hardware Environments

**Higher Safety Requirements**:

- MUST start in IDLE mode (explicit verification required)
- BLOCK policy recommended (prevent accidental manual interference)
- Full verification checklist enforcement

**Rationale**: Hardware can cause injury, damage, or process failures

### Configuration Guidance

**Development** (`config/anolis-runtime.yaml` or a composer-generated system):

```yaml
automation:
  manual_gating_policy: OVERRIDE # Allow manual calls during automation
```

Transition to MANUAL explicitly with `POST /v0/mode` after basic startup checks.

**Production** (`systems/<project>/anolis-runtime.yaml` or validated deployment config):

```yaml
automation:
  manual_gating_policy: BLOCK # Prevent manual interference
```

Startup still enters IDLE automatically. The production distinction is that the
operator follows the full verification path before transitioning to MANUAL or AUTO.

---

## Related Documentation

- [Provider Safe Initialization Contract](providers.md#safe-initialization-contract)
- [Configuration Schema](configuration-schema.md)
- [Automation Layer](automation.md)
- [HTTP API Reference](http-api.md)
- [anolis-provider-sim Safety Compliance](../anolis-provider-sim/README.md#safe-initialization-in-provider-sim)

---

## Support

For safety-critical deployments, consider:

- Establishing site-specific safety procedures
- Conducting hazard analysis (FMEA, fault tree analysis)
- Implementing hardware interlocks independent of software control
- Consulting with safety professionals for high-risk applications

Anolis provides software safety mechanisms (IDLE mode, FAULT transitions, provider supervision),
but **hardware safety remains the responsibility of the system integrator**.
