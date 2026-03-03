# Anolis Automation Layer

Automation components for behavior tree orchestration.

## Architecture Constraints (Critical)

The automation layer is a **consumer of kernel services**, NOT a replacement for core IO:

| Constraint                                | Implementation                                               |
| ----------------------------------------- | ------------------------------------------------------------ |
| **BT nodes read via StateCache**          | No direct provider access; state read through typed service context |
| **BT nodes act via CallRouter**           | All device calls go through validated control path           |
| **No new provider protocol features**     | Automation uses existing ADPP v1 capabilities                    |
| **No device-specific logic in BT engine** | BT runtime is capability-agnostic                            |

## Blackboard Contract

### Critical Semantics

- BT nodes consume a typed service context refreshed before every `tick()`
- Signal reads are live `StateCache` queries (thread-safe), not a copied snapshot
- State may change between node executions if polling updates occur concurrently
- BT is **NOT for hard real-time control**; call latency is acceptable

### Blackboard Schema

Populated in `BTRuntime::populate_blackboard()` before each tick:

```cpp
BTServiceContext services{
  .state_cache = &state_cache_,
  .call_router = &call_router_,
  .provider_registry = &provider_registry_,
  .parameter_manager = parameter_manager_,
};
blackboard->set(kBTServiceContextKey, services);
```

Typed context fields consumed by BT nodes:

- `state_cache` (required): read path for `ReadSignal` and `CheckQuality`
- `call_router` (required): write path for `CallDevice`
- `provider_registry` (required): execution dependency for `CallRouter::execute_call`
- `parameter_manager` (optional): read path for `GetParameter`

**Important:** We pass **references**, not full snapshots, for efficiency.
StateCache's `get_signal_value()` is thread-safe. This design is acceptable because:

1. Polling happens every 500ms, ticks every 100ms (10 Hz)
2. BT execution is fast compared to poll rate
3. If a value changes mid-tick, next tick will see the change
4. BT is for orchestration policy, not hard real-time control

## Thread Model

- **Single-threaded tick loop** in dedicated thread
- Tick rate configurable (default 10 Hz = 100ms period)
- Sleep until next tick (not busy-wait)
- BT nodes may **block on device calls** - trees must be designed for call latency

## Custom Nodes

Base classes for Anolis-specific BT nodes:

- `ReadSignalNode` - Reads from StateCache via blackboard
- `CallDeviceNode` - Invokes device function via CallRouter
- `CheckQualityNode` - Verifies signal quality (OK/STALE/FAULT)
- `GetParameterNode` - Reads numeric runtime parameters (`double`/`int64`)

All nodes registered with BehaviorTree.CPP factory.

## Safety Disclaimer

**Automation is a control policy layer, not a safety-rated system.**

External safety systems (e.g., E-stops, interlocks) are still required for real hardware.

FAULT mode is _policy_, not a certified safety mechanism.

## Demo & Documentation

Demo behavior tree available at: `behaviors/demo.xml`
Full documentation: `docs/automation.md`

Enable automation in `anolis-runtime.yaml`:

```yaml
automation:
  enabled: true
  behavior_tree: ./behaviors/demo.xml
  tick_rate_hz: 10
  manual_gating_policy: BLOCK # or OVERRIDE
```

## References

- [BehaviorTree.CPP Documentation](https://www.behaviortree.dev/)
