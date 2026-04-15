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
- Enforce exponential backoff and circuit breaker policies
- Coordinate device cleanup and rediscovery on restart

### Device Registry (core/registry/)

- Run discovery at startup (Hello → ListDevices → DescribeDevice)
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

## Data Flow

**Discovery (startup)**:

```text
Runtime → Provider Host → Provider Process
         ← Hello
         ← ListDevices
         ← DescribeDevice
→ Device Registry (immutable)
```

**State Polling (continuous)**:

```text
State Cache → Provider Host → Provider Process
            ← ReadSignals
→ Update cache with timestamps
```

**Control Commands (Manual or Automated)**:

```text
External API (HTTP, CLI, Behavior Tree) → Call Router
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
     BTs use same validation/enforcement as manual control.
```

## Key Invariants

1. **Only providers talk to hardware** - No direct GPIO/serial/network from runtime
2. **Registry is read-only after discovery** - No hot-plug (v0)
3. **All reads go through State Cache** - Never query provider directly (external layers, BTs, HTTP, CLI)
4. **All writes go through Call Router** - No bypass allowed (manual or automated, all use same path)
5. **Providers are isolated processes** - No shared memory, crash-safe
6. **No hard-coded device semantics** - Core is capability-driven, not device-specific
7. **External layers use core APIs only** - BTs, HTTP, UI never talk directly to providers

## Technology Stack

- **Language**: C++17
- **Build**: CMake 3.20+
- **Dependencies**: vcpkg (protobuf 6.33.4, yaml-cpp)
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
  - EventEmitter: Lock-free queue with atomic operations
  - ModeManager: `std::mutex` guards mode transitions

## Configuration

```yaml
runtime:
  name: anolis-main # Optional runtime instance identifier

providers:
  - id: sim0
    command: path/to/anolis-provider-sim
    args: []
  - id: modbus0
    command: path/to/anolis-provider-modbus
    args: ["--port", "/dev/ttyUSB0"]

polling:
  interval_ms: 500

logging:
  level: info
```

## Extension Points

External layers integrate via:

- **State Cache**: `get_device_state()`, `get_signal_value()`
- **Call Router**: `execute_call(device, function, args)`
- **Registry**: `get_device()`, `get_all_devices()`

HTTP, automation, and UI integrations should remain on these runtime surfaces.

## Packaging Boundary Note

Public SDK packaging (`include/` export/install surface) is not part of this runtime architecture baseline.
Current architecture decisions in this document apply to runtime behavior, provider boundaries, and control/state flow.
