# Safety Operations Guide

This guide defines safe operating procedures for Anolis-managed hardware systems.

## Core Principles

1. Safe startup: runtime always starts in `IDLE`.
2. Explicit control: mode transitions require operator action (`POST /v0/mode`).
3. Controlled automation: in `AUTO`, manual calls are policy-gated (`BLOCK` or `OVERRIDE`).
4. Visibility first: read-only diagnostics remain available across modes.
5. Hardware safety remains external: software controls do not replace interlocks/E-stop systems.

## Runtime Mode Safety Semantics

| Runtime Mode | Automation Loop | Control Operations (`POST /v0/call`) | Notes |
| --- | --- | --- | --- |
| `IDLE` | Stopped | Blocked | Safe startup/standby mode |
| `MANUAL` | Stopped | Allowed | Operator-driven commissioning/control |
| `AUTO` | Running | Manual calls policy-gated | Automation calls continue normally |
| `FAULT` | Stopped | Allowed | Recovery/diagnostic mode; transition restrictions still apply |

Transition rules:

1. `IDLE <-> MANUAL`
2. `MANUAL <-> AUTO`
3. `Any -> FAULT` (valid transition target)
4. `FAULT -> MANUAL`
5. `FAULT -> AUTO` is invalid
6. `AUTO -> IDLE` is invalid

Note: FAULT is not globally auto-entered for every error condition in the current runtime; it is a defined mode and transition target with strict recovery pathing.

## Standard Startup Sequence

1. Start runtime (enters `IDLE`).
2. Verify provider/device availability and safe initial states.
3. Transition to `MANUAL`.
4. Run manual verification/calibration checks.
5. Transition to `AUTO` only after operator acceptance.

Use this canonical transition command:

```bash
curl -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}'
```

Use the same command shape for `AUTO`, `IDLE`, and `FAULT`.

## Mode Procedure Checklist

### IDLE -> MANUAL

1. Confirm providers are `AVAILABLE`.
2. Confirm actuator signals are in safe states.
3. Transition to `MANUAL`.

### MANUAL -> AUTO

1. Validate behavior tree configuration and parameters.
2. Confirm manual verification is complete.
3. Transition to `AUTO`.
4. Monitor `/v0/automation/status` and `/v0/providers/health`.

### AUTO -> MANUAL

1. Transition to `MANUAL` for planned operator takeover.
2. Verify actuators and signals settle to expected state.

### MANUAL -> IDLE

1. Command actuators to safe outputs.
2. Verify safe state via `/v0/state`.
3. Transition to `IDLE`.

### Any -> FAULT, then FAULT -> MANUAL

1. Enter `FAULT` when explicit recovery isolation is needed.
2. Diagnose and resolve root cause.
3. Transition `FAULT -> MANUAL`.
4. Re-verify before returning to `AUTO`.

## Emergency Response

1. Immediate software stop: transition to `MANUAL`.
2. If software path is insufficient: terminate runtime process.
3. If physical hazard persists: use hardware E-stop / power isolation.
4. After incident: inspect hardware, collect logs, and restart from full startup checklist.

## Common Risks and Mitigations

1. Skipping IDLE checks:
   - Always validate safe initial state before `MANUAL`.
2. Unsafe power-on assumptions:
   - Providers must actively drive safe defaults on startup.
3. Invalid FAULT recovery path:
   - Never attempt `FAULT -> AUTO`; recover through `MANUAL`.
4. Manual interference in AUTO:
   - Prefer `manual_gating_policy: BLOCK` for production.
5. Ignored provider health degradation:
   - Monitor `/v0/providers/health` and supervision fields continuously.

## Hardware Integration Safety Checklist

### Provider safety

1. Provider startup sets actuators to safe defaults.
2. Communication failures surface as degraded quality/error status.
3. Restart behavior does not create unsafe transient outputs.

### Capability and limits

1. Function argument constraints match physical limits.
2. Signals represent actionable safety state.
3. Device fault conditions are exposed clearly.

### Operational readiness

1. Startup/recovery SOP exists and is tested.
2. Emergency stop SOP exists and is tested.
3. Site-specific hazard analysis is complete.

## Development vs Production Policy

Development/testing:

1. Runtime still starts in `IDLE`.
2. Faster workflows are acceptable after minimal safety verification.
3. `OVERRIDE` may be acceptable for controlled bench testing.

Production/hardware operation:

1. Keep explicit IDLE verification and full startup checklist.
2. Use `manual_gating_policy: BLOCK` unless a written override policy exists.
3. Require documented recovery steps for FAULT entry and exit.

## Related Documentation

1. [Provider Safe Initialization Contract](providers.md#safe-initialization-contract)
2. [Automation Layer](automation.md)
3. [Runtime Configuration](configuration.md)
4. [HTTP API Reference](http-api.md)
5. [anolis-provider-sim fault injection reference](https://github.com/FEASTorg/anolis-provider-sim/blob/main/README.md#fault-injection-api)
6. [anolis-provider-sim safe initialization reference](https://github.com/FEASTorg/anolis-provider-sim/blob/main/README.md#safe-initialization-in-provider-sim)
