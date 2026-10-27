# Architecture

## System Diagram

```text
┌─────────────────────────────────────────┐
│         External Interfaces             │
│     (HTTP, Behavior Trees, Tools)       │
└───────────────┬─────────────────────────┘
                │
┌───────────────▼─────────────────────────┐
│          Anolis Runtime (core/)         │
│                                         │
│  ┌─────────────┐      ┌──────────────┐  │
│  │   Device    │◄────►│  State Cache │  │
│  │  Registry   │      │   (polls)    │  │
│  └──────┬──────┘      └──────▲───────┘  │
│         │                    │          │
│         ▼                    │          │
│  ┌─────────────┐             │          │
│  │    Call     │─────────────┘          │
│  │   Router    │                        │
│  └──────┬──────┘                        │
│         │                               │
│  ┌──────▼──────┐                        │
│  │  Provider   │                        │
│  │    Host     │                        │
│  └──────┬──────┘                        │
└─────────┼───────────────────────────────┘
          │ ADPP (stdio)
┌─────────▼─────────────────────────────────┐
│       Provider Processes                  │
│  (anolis-provider-sim and other providers) │
└───────────────────────────────────────────┘
```

## Component Responsibilities

### Provider Host (core/provider/)

- Spawn/manage provider processes
- Frame stdio communication (uint32_le length prefix)
- Send ADPP requests, receive responses
- Supervise provider health (crash detection, automatic restart)
- Track the configured restart backoff schedule (`restart_policy.backoff_ms`) and circuit breaker state

### Device Registry (core/registry/)

- Run discovery at startup and on provider restart (ListDevices → DescribeDevice per device, after the Provider Host's Hello)
- Store immutable device capabilities
- Provide lookup for validation

### State Cache (core/state/)

- Poll default signals from all devices (500ms interval)
- Track timestamps and staleness
- Expose read-only snapshot API
- Thread-safe via shared_ptr copies

### Call Router (core/control/)

- Validate control requests (device exists, function exists, args valid)
- Serialize calls per-provider (mutex locks)
- Forward to provider via ProviderHandle
- Trigger immediate post-call state poll

### Runtime (core/runtime/)

- Load config (yaml)
- Launch providers
- Initialize state cache polling config (`StateCache::initialize`)
- Prime one synchronous snapshot (`StateCache::poll_once`)
- Start background polling only after initialization (`StateCache::start_polling`)
- Enter main loop
- Restart crashed providers and rediscover their devices (when `restart_policy.enabled`)

## Data Flow

**Discovery (startup)**:

```text
Runtime → Provider Host → Provider Process
         ← Hello
         ← WaitReady (if supports_wait_ready)
         ← ListDevices
         ← DescribeDevice (per device)
→ Device Registry (replaced per provider on restart)
```

**State Polling (continuous)**:

```text
State Cache → Provider Host → Provider Process
            ← ReadSignals
→ Update cache with timestamps
```

**Control Commands (Manual or Automated)**:

```text
External API (HTTP, Behavior Tree) → Call Router
                                   → Validate against Registry
                                   → Provider Host → Provider Process
                                                  ← CallResponse
                                   → State Cache (immediate poll)
                                   ← Result
```

**Behavior Trees (Above Kernel)**:

```text
Behavior Tree (reads) → State Cache
Behavior Tree (writes) → Call Router → Provider

Key: BTs never bypass CallRouter or StateCache.
     BTs use the same validation as manual control. BT calls are exempt
     from the manual-in-AUTO BLOCK policy and are subject to the device-loss latch.
```

## Key Invariants

1. **Only providers talk to hardware** - No direct GPIO/serial/network from runtime
2. **Registry changes only on provider restart** - A provider's devices are replaced as a whole on rediscovery; no hot-plug
3. **All signal reads go through State Cache** - Never query provider directly for signals (external layers, BTs, HTTP); provider health (`GetHealth`) is fetched from providers on demand
4. **All writes go through Call Router** - No bypass allowed (manual or automated, all use same path)
5. **Providers are isolated processes** - No shared memory, crash-safe
6. **No hard-coded device semantics** - Core is capability-driven, not device-specific
7. **External layers use core APIs only** - BTs and UI never talk directly to providers; HTTP does so only to fetch provider health (`GetHealth`)

## Technology Stack

- **Language**: C++20
- **Build**: CMake 3.20+ (4.2+ on Windows, for the Visual Studio 2026 generator)
- **Dependencies**: vcpkg (protobuf, yaml-cpp, cpp-httplib, nlohmann-json, BehaviorTree.CPP)
- **Protocol**: ADPP (protobuf over framed stdio)
- **Platform**: Windows, Linux

## Concurrency Model (Current: v1)

**Current Architecture:** Multi-threaded

- **State Polling**: Dedicated background thread in StateCache
  - Runs continuous loop at configured interval (default 500ms)
  - Thread-safe via mutex-protected state snapshots
  - Non-blocking to runtime main loop

- **Behavior Tree Automation**: Dedicated tick thread in BTRuntime
  - Runs at configured rate (default 10Hz)
  - Only active in AUTO mode
  - Accesses state via thread-safe StateCache API

- **HTTP Server**: Embedded httplib server with thread pool
  - Configurable thread pool size (default 40)
  - Concurrent request handling
  - Thread-safe access to all runtime components

- **Provider Communication**: Serialized per-provider
  - Per-provider mutex locks ensure atomic call execution
  - Prevents concurrent calls to same provider process
  - Multiple providers can be called in parallel

- **Synchronization**:
  - StateCache: `std::mutex` guards device state map
  - CallRouter: Per-provider `std::mutex` for call serialization
  - EventEmitter: mutex-protected per-subscriber bounded queues
  - ModeManager: `std::mutex` guards mode transitions

## Configuration

```yaml
runtime:
  name: anolis-main # Optional runtime instance identifier

providers:
  - id: sim0
    command: path/to/anolis-provider-sim
    args: ["--config", "path/to/provider-sim.yaml"]
  - id: bread0
    command: path/to/anolis-provider-bread
    args: ["--config", "path/to/provider-bread.yaml"]

polling:
  interval_ms: 500

logging:
  level: info
```

## Extension Points

External layers integrate via:

- **State Cache**: `get_device_state()`, `get_signal_value()`
- **Call Router**: `execute_call(const CallRequest&, ProviderRegistry&)`
- **Registry**: `get_device_copy()`, `get_device_by_handle_copy()`, `get_all_devices()`

HTTP, automation, and UI integrations should remain on these runtime surfaces.

## Packaging Boundary Note

Public SDK packaging (`include/` export/install surface) is not part of this runtime architecture baseline.
Current architecture decisions in this document apply to runtime behavior, provider boundaries, and control/state flow.
