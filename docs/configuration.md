# Configuration

Anolis Runtime is configured via a YAML file (sample checked in at `config/anolis-runtime.yaml`).

For composer-managed systems, the generated runtime YAML lives at
`systems/<project>/anolis-runtime.yaml` and is derived from that project's
`system.json`.

> **📖 Complete Schema Reference:**
> See [configuration-schema.md](configuration-schema.md) for the canonical v1.0 schema with validation rules, constraints, and migration notes.

## Structure

```yaml
runtime:
  name: "anolis-main" # Optional: instance identifier
  shutdown_timeout_ms: 2000 # Provider graceful shutdown (500-30000ms)
  startup_timeout_ms: 30000 # Overall startup timeout (5000-300000ms)
  # Note: IDLE mode is enforced at startup (not configurable)
  # Use HTTP POST /v0/mode to transition: IDLE -> MANUAL -> AUTO

http:
  enabled: true
  bind: 127.0.0.1
  port: 8080
  # CORS allowlist ("*" = allow all; recommended to pin your UI origin)
  cors_allowed_origins:
    - http://localhost:3000
    - http://127.0.0.1:3000
  cors_allow_credentials: false
  thread_pool_size: 40 # Worker threads (default: 40)

polling:
  interval_ms: 500 # Device polling interval

logging:
  level: info # debug, info, warn, error

telemetry:
  enabled: false
  # ... (InfluxDB settings)

providers:
  - id: sim0
    command: ./path/to/provider
    timeout_ms: 5000
    args: []

automation:
  enabled: false
  behavior_tree: path/to/tree.xml
  tick_rate_hz: 10
```

## Options

### Runtime Parameters

**`runtime.name`** (optional, string)

- Instance identifier for multi-runtime deployments
- Used in logs and telemetry to disambiguate runtime instances
- Default: "" (empty)

**`runtime.shutdown_timeout_ms`** (optional, int)

- Provider graceful shutdown timeout in milliseconds
- Min: 500ms (hardware needs time to respond)
- Max: 30000ms (30s, beyond this indicates hung provider)
- Default: 2000ms (2 seconds)
- Use case: Hardware providers with slow power-down sequences may need longer timeout

**`runtime.startup_timeout_ms`** (optional, int)

- Overall startup timeout in milliseconds for fail-fast behavior
- Min: 5000ms (5s, minimum for spawn + discovery + capabilities)
- Max: 300000ms (5 minutes, beyond this indicates serious problem)
- Default: 30000ms (30 seconds)
- Use case: CI/testing needs predictable failures; different hardware has different startup times

**Runtime Mode:** IDLE mode is **enforced at startup** (not configurable).
This ensures safe-by-default behavior where control operations are blocked until explicit operator transition to MANUAL or AUTO via HTTP API.

### Runtime Modes

Runtime mode is **not configurable in YAML**. The system always starts in IDLE mode (safe-by-default).
Mode can be changed at runtime via HTTP API (`POST /v0/mode`).

**Available Modes:**

- **IDLE**: Safe/off state. Control operations blocked, read-only allowed. Use as startup default.
- **MANUAL**: Manual control allowed, automation stopped. Use for operator-driven testing and manual operations.
- **AUTO**: Automation running, manual gated by policy. Use for normal production operation.
- **FAULT**: System-detected fault, automatic transition. All operations blocked until recovery.

**Mode Semantics:**

| Mode   | Control Ops | Read-Only  | Automation | Use Case              |
| ------ | ----------- | ---------- | ---------- | --------------------- |
| IDLE   | ❌ Blocked  | ✅ Allowed | ❌ Stopped | Startup, verification |
| MANUAL | ✅ Allowed  | ✅ Allowed | ❌ Stopped | Testing, manual ops   |
| AUTO   | Policy      | ✅ Allowed | ✅ Running | Normal operation      |
| FAULT  | ❌ Blocked  | ✅ Allowed | ❌ Stopped | Error state, recovery |

**Operation Types:**

- **Control Operations**: `CallDevice` (actuates hardware) - modifies device state
- **Read-Only Operations**: Health checks, status queries, `ReadSignals`, device discovery

**Valid Transitions:**

- ✅ IDLE ↔ MANUAL
- ✅ MANUAL ↔ AUTO
- ✅ Any → FAULT
- ✅ FAULT → MANUAL (recovery path)
- ❌ FAULT → AUTO (must recover through MANUAL first)
- ❌ AUTO → IDLE (must go through MANUAL first)

**Rationale for IDLE Default:**

Starting in IDLE mode enforces a safe startup sequence:

1. Runtime starts in IDLE (control operations blocked)
2. Providers initialize (devices in safe defaults)
3. Operator verifies system state (read-only queries work)
4. Operator transitions to MANUAL (enables control operations)
5. Operator performs checks/calibration
6. Operator transitions to AUTO (enables automation)

This ensures no unsafe actuation occurs during startup, configuration changes, or crash recovery.

### HTTP

- **bind/port**: Interface and port to listen on.
- **cors_allowed_origins**: List of allowed origins. Use `"*"` only for development; pin exact origins in validation/production.
- **cors_allow_credentials**: Emit `Access-Control-Allow-Credentials: true` when your UI needs cookies/auth headers.
- **thread_pool_size**: Adjust based on concurrent SSE clients. Formula: `max_sse_clients + 8`.

### Logging

- **level**: Controls verbosity. Use `debug` for troubleshooting provider issues.

### Providers

- **id**: Unique provider identifier
- **command**: Path to provider executable
- **args**: Command-line arguments
- **timeout_ms**: ADPP operation timeout (default: 5000ms)
- **restart_policy**: Automatic supervision configuration (optional)

#### Restart Policy

Automatic supervision monitors provider health and restarts crashed providers:

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

- **enabled**: Enable automatic restart (default: false)
- **max_attempts**: Restart attempts before circuit breaker opens (default: 3)
- **backoff_ms**: Delay before each restart attempt in milliseconds. Array length must match max_attempts.
- **timeout_ms**: Maximum time to wait for restart (default: 30000ms)

**Behavior:**

- Provider crashes → supervisor records crash → waits backoff delay → attempts restart
- Successful restart → crash counter resets
- Circuit breaker opens after max_attempts → no further restarts until manual intervention
- Existing provider inventory remains in place until replacement discovery and ownership validation succeed

### Telemetry (InfluxDB)

- **enabled**: Enable/disable telemetry sink
- **url**: InfluxDB server URL (supports both HTTP and HTTPS)
- **org**: InfluxDB organization name
- **bucket**: InfluxDB bucket name
- **token**: InfluxDB API token (required when enabled)

Both HTTP and HTTPS URLs are supported:

```yaml
# Local development
url: http://localhost:8086

# Cloud/production with TLS
url: https://influx.example.com:8086
```

**TLS Certificate Verification:** HTTPS connections verify server certificates using the system's default CA store.
For self-signed certificates, either add them to the system trust store or use HTTP with a reverse proxy.
