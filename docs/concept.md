# Concept

## What is Anolis?

Anolis is a **capability-oriented machine runtime** that makes diverse industrial devices look uniform.

Devices expose:

- **Signals**: Readable state (temperature, position, status)
- **Functions**: Validated actions (set_relay, move_motor, configure)

All automation and control (manual or automated) operates through these capabilities.

## The Problem

Industrial systems have:

- Different hardware (sensors, actuators, motors, relays)
- Different protocols (Modbus, CAN, GPIO, serial)
- Different vendors (Arduino, PLC, custom boards)
- Different languages (C++, Python, Rust)

Building automation requires talking to all of them. This is tedious and fragile.

## The Solution

**Providers**: Small processes that speak to one type of hardware and expose it via a standard protocol.
(ADPP - Anolis Device Provider Protocol).

**Runtime**: Core system that discovers devices, polls state, routes control commands, and exposes unified APIs.

**Result**: Write control logic once. Swap hardware by changing config.

## Key Ideas

1. **Providers are isolated processes** - No shared memory, crash-safe
2. **Protocol-based communication** - stdio framing, protobuf messages
3. **Single source of truth** - Core is authoritative for machine state (StateCache)
4. **Unified control path** - All control goes through CallRouter, no bypasses
5. **Capability-driven** - Core never hard-codes device semantics

## What Anolis Is NOT

- Not a SCADA system (no HMI built-in)
- Not a PLC replacement (complementary)
- Not IoT cloud platform (local-first)
- Not real-time (soft real-time only)

## Current Runtime Capabilities

The current runtime includes:

- Provider lifecycle and supervision (spawn, health, restart/backoff, circuit behavior)
- Capability registry and state polling cache
- Unified validated control path (`CallRouter`)
- Runtime YAML contracts (schema + runtime semantic validation)
- Runtime HTTP `/v0` surface for state, control, mode, parameters, and events
- Behavior-tree automation layer with mode gating and runtime parameters
- Contract-tested integration surfaces for Composer and Workbench tooling

## Use Cases

- **Lab automation**: Mix Arduino sensors with NI DAQ cards
- **Test rigs**: Control motors, read thermocouples, log data
- **Small production**: Monitor + control with behavior trees
- **Prototyping**: Swap mock providers for real hardware transparently
