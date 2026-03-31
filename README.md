# anolis

[![CI](https://github.com/FEASTorg/anolis/actions/workflows/ci.yml/badge.svg)](https://github.com/FEASTorg/anolis/actions/workflows/ci.yml)
[![Extended](https://github.com/FEASTorg/anolis/actions/workflows/extended.yml/badge.svg)](https://github.com/FEASTorg/anolis/actions/workflows/extended.yml)

**Anolis** is a modular control runtime for building machines from heterogeneous devices.

It provides a **hardware-agnostic core** that discovers devices, understands their capabilities,
maintains live state, and coordinates control actions — independent of how the hardware is connected or implemented.

Anolis is designed to sit **between low-level device interfaces and high-level behavior**, acting as the system’s operational brain.

> Just as anoles adapt to diverse environments,
>
> Anolis adapts to diverse hardware ecosystems.

<img src="docs/assets/anolis_logo_1024.png" alt="Anolis Logo" width="300"/>

---

## What Anolis Is

Anolis is:

- A **runtime kernel** for machines composed of many devices
- A **provider-based system**, where hardware integrations live out of process
- A **capability-driven control layer**, not hardcoded device logic
- A **bridge** between:
  - device buses (I²C, SPI, GPIO, CAN, BLE, etc.)
  - orchestration layers (dashboards, schedulers, behavior trees)

Anolis does **not** assume:

- a specific bus
- a specific MCU
- a specific control paradigm
- a specific UI

---

## Core Concepts

### Providers

A _provider_ is an external process that exposes one or more devices using a standard protocol.

Providers may:

- talk to microcontrollers
- wrap drivers or SDKs
- simulate hardware
- proxy other systems

Anolis communicates with providers over a simple message protocol and treats them as isolated, replaceable components.

---

### Devices

A _device_ represents a functional unit with:

- signals (telemetry / state)
- functions (actions or configuration)
- metadata and capabilities

Devices are **described**, not hardcoded.

---

### Capabilities

Capabilities define:

- what signals exist
- what functions can be called
- what arguments and constraints apply

Anolis uses capabilities to:

- validate control actions
- drive UIs automatically
- enable generic orchestration

---

### State

Anolis maintains a live, cached view of device state by polling providers.

All reads go through this cache.  
All control actions flow through a single validated path.

---

## What Anolis Is Not

Anolis is **not**:

- a device driver
- a hardware abstraction layer
- a dashboard
- a PLC replacement
- a firmware framework

Those concerns live _around_ Anolis, not inside it.

---

## Typical Stack

```text
[ Dashboards / APIs / Schedulers ]
↓
[ Anolis Core ]
↓
[ Providers ]
↓
[ Hardware / Systems ]
```

Examples:

- An HTTP dashboard issues a control request → Anolis validates → Provider executes
- A behavior tree reads state → Anolis cache → decides next action
- A simulation provider is swapped for real hardware with no core changes

---

## Quick Start

### Preset Quickstart

Install host prerequisites (including Ninja and vcpkg) first: [docs/getting-started.md](docs/getting-started.md).

Linux/macOS:

```bash
git clone https://github.com/FEASTorg/anolis.git
cd anolis
git submodule update --init --recursive
cmake --preset dev-release
cmake --build --preset dev-release --parallel
ctest --preset dev-release
./build/dev-release/core/anolis-runtime --config ./anolis-runtime.yaml
```

Windows (PowerShell):

```powershell
git clone https://github.com/FEASTorg/anolis.git
Set-Location anolis
git submodule update --init --recursive
cmake --preset dev-windows-release
cmake --build --preset dev-windows-release --parallel
ctest --preset dev-windows-release
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\anolis-runtime.yaml
```

### Validation & Acceptance Testing

Run pytest integration/scenario suites directly:

```bash
# Integration suites (non-stress)
python -m pytest tests/integration/test_integration.py -m "not stress and not slow"

# Scenario suites (non-stress)
python -m pytest tests/scenarios/test_scenarios.py -m "not stress and not slow"

# Stress/slow coverage
python -m pytest tests/integration/test_integration.py tests/scenarios/test_scenarios.py -m "stress or slow"
```

Build/dependency/CI governance: [docs/dependencies.md](docs/dependencies.md).

Cross-repo compatibility is validated in CI via the pinned `anolis-provider-compat` lane (see `.ci/dependency-pins.yml`).

---

## HTTP API Examples

```bash
# List devices
curl -s http://127.0.0.1:8080/v0/devices | jq

# Get device state
curl -s http://127.0.0.1:8080/v0/state/sim0/motorctl0 | jq

# Set motor duty to 75%
curl -s -X POST http://127.0.0.1:8080/v0/call \
  -H "Content-Type: application/json" \
  -d '{
    "provider_id": "sim0",
    "device_id": "motorctl0",
    "function_id": 10,
    "args": {
      "motor_index": {"type": "int64", "int64": 1},
      "duty": {"type": "double", "double": 0.75}
    }
  }' | jq

# Get runtime status
curl -s http://127.0.0.1:8080/v0/runtime/status | jq
```

### API Endpoints

| Endpoint                                       | Method | Description               |
| ---------------------------------------------- | ------ | ------------------------- |
| `/v0/devices`                                  | GET    | List all devices          |
| `/v0/devices/{provider}/{device}/capabilities` | GET    | Get signals and functions |
| `/v0/state`                                    | GET    | Get all device states     |
| `/v0/state/{provider}/{device}`                | GET    | Get single device state   |
| `/v0/call`                                     | POST   | Execute device function   |
| `/v0/runtime/status`                           | GET    | Get runtime status        |
| `/v0/mode`                                     | GET    | Get automation mode       |
| `/v0/mode`                                     | POST   | Set automation mode       |
| `/v0/parameters`                               | GET    | List runtime parameters   |
| `/v0/parameters`                               | POST   | Update runtime parameter  |

See [docs/http-api.md](docs/http-api.md) for full API reference.

---

## Automation Quickstart

Enable automation and parameters in your runtime config:

```yaml
automation:
  enabled: true
  behavior_tree: ./behaviors/demo.xml
  tick_rate_hz: 10
  manual_gating_policy: BLOCK
  parameters:
    - name: temp_setpoint
      type: double
      default: 25.0
      min: 10.0
      max: 50.0
```

Update a parameter and enter AUTO mode:

```bash
curl -s -X POST http://127.0.0.1:8080/v0/parameters \
  -H "Content-Type: application/json" \
  -d '{"name": "temp_setpoint", "value": 30.0}' | jq

curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode": "AUTO"}' | jq
```

The demo behavior tree uses:

```xml
<GetParameter param="temp_setpoint" value="{target_temp}"/>
```

---

## Design Goals

- **Hardware-agnostic**
- **Process-isolated integrations**
- **Explicit capabilities**
- **Deterministic control paths**
- **Composable systems**

Anolis is intended to scale from small experimental rigs to complex multi-device machines.

---

## Status

_Anolis_ is under active development.

Current focus:

- Core runtime
- Provider interface
- State and control infrastructure

Higher-level orchestration and UI layers build on top of this foundation.

---

## Dependency Stack

### Core Runtime (C++)

- **protobuf** — ADPP protocol serialization
- **cpp-httplib** — HTTP server for REST API
- **nlohmann-json** — JSON parsing and generation
- **yaml-cpp** — Configuration file parsing
- **behaviortree-cpp** (v4.6.2) — Behavior tree engine for automation

### Provider Implementation

- **protobuf** — ADPP protocol (same as runtime)

### Testing & Automation (Python)

- **requests** — HTTP API testing
- **pyyaml** — Config generation for tests

### Observability Stack (Optional)

- **InfluxDB** (v2.x) — Time-series telemetry storage
- **Grafana** (v9.x+) — Visualization dashboards
- **Docker** + **Docker Compose** — Container orchestration

### Build & Development

- **CMake** (≥3.20) — Build system
- **vcpkg** — C++ package management
- **C++17** compiler — MSVC 2022, GCC 11+, or Clang 14+

---

## Name

_Anolis_ is named after [anoles](https://en.wikipedia.org/wiki/Dactyloidae).
They are adaptable lizards known for thriving across diverse environments.

The name reflects the system’s goal:  
**adaptable control over diverse hardware ecosystems.**
