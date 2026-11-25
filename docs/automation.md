# Automation

Automation in Anolis is a behavior-tree layer above core runtime services.

## Contract and Build Context

1. Runtime config contract: `schemas/runtime/runtime-config.schema.json`
2. Runtime HTTP contract: `schemas/http/runtime-http.openapi.v0.yaml`
3. Runtime mode/parameter endpoints:
   - `GET/POST /v0/mode`
   - `GET/POST /v0/parameters`
   - `GET /v0/automation/status`
   - `GET /v0/automation/tree`

Build-time option:

```bash
cmake -DANOLIS_ENABLE_AUTOMATION=OFF ...
```

When compiled out, automation endpoints return `UNAVAILABLE`.

## Architecture Boundaries

Automation is a consumer of kernel services, not a replacement for them.

1. Reads: through `StateCache`.
2. Writes: through `CallRouter`.
3. Inventory/capabilities: not read by the BT; `CallRouter` validates each call
   against `DeviceRegistry` and dispatches it through `ProviderRegistry`.
4. Parameters: through `ParameterManager`.
5. No provider-specific behavior is encoded in the BT runtime engine.

## Runtime Modes

| Mode | Automation | Manual Calls | Control Behavior |
| --- | --- | --- | --- |
| `IDLE` | Stopped | Blocked | Startup mode; outputs set by `safety.on_startup` |
| `MANUAL` | Stopped | Allowed | Operator control |
| `AUTO` | Running | Policy-gated | Automated control |
| `FAULT` | Stopped | Allowed | Manual recovery state |

Transition rules:

1. `IDLE <-> MANUAL`
2. `MANUAL <-> AUTO`
3. `Any -> FAULT` (valid transition target)
4. `FAULT -> MANUAL`
5. `FAULT -> AUTO` is not allowed directly.
6. `AUTO -> IDLE` is not allowed directly.

Startup is always `IDLE`.

## Manual Gating Policy in AUTO

`automation.manual_gating_policy` controls manual calls while mode is `AUTO`.

1. `BLOCK` (default): manual `POST /v0/call` rejected.
2. `OVERRIDE`: manual calls allowed during AUTO.

Control-operation guardrails in current runtime:

1. `IDLE` blocks control operations at `CallRouter`.
2. `AUTO` applies manual-call gating policy (`BLOCK`/`OVERRIDE`).
3. `FAULT` is primarily a recovery mode; transition constraints apply, but control calls are not globally blocked by `CallRouter`.

## Provider-declared function policy

A provider's `FunctionPolicy` can restrict a function, and the runtime enforces
it in `CallRouter`, as the protocol requires (#566). `GET
/v0/devices/{provider_id}/{device_id}/capabilities` shows all three fields.

1. `allowed_modes`: a call in any other mode is refused with
   `FAILED_PRECONDITION`, naming the modes and the current one. This holds for
   manual calls, the behavior tree and mode-transition hooks alike. Empty means
   every mode. A runtime without automation has no mode and judges calls as
   `MANUAL`.
2. `min_interval_ms`: a call that arrives sooner than this after the last one
   dispatched to that function is refused with `FAILED_PRECONDITION` and the
   time remaining. It is refused, not deferred: retrying is the caller's
   decision.
3. `requires_lease`: protocol v1 leases are not implemented. Discovery logs a
   WARN naming each function that sets it, and the function is called without a
   lease.

Software-stop and startup safe-state calls are exempt from `allowed_modes` and
`min_interval_ms`, so the ladder can always drive outputs safe.

## Runtime Parameters

Parameters are declared in runtime YAML and mutable via HTTP.

With `automation.enabled: true`, each default must satisfy its own `min`,
`max` and `allowed_values`, and each name must be unique. A config that
breaks either fails `--check-config`, and the runtime refuses to start on it
rather than run without the parameter
([configuration-schema.md](configuration-schema.md#automation)).

Values set over HTTP are not persisted. Every start begins at the config
defaults, and the startup log lists each parameter with its default value.
`GET /v0/parameters` reports `source: default` until a `POST` changes a value,
then `source: operator`. The runtime always starts in `IDLE`, so a value that
reverted on restart takes effect only once an operator re-enters `AUTO`.

Example:

<!-- validate: runtime-config fragment -->

```yaml
automation:
  enabled: true
  behavior_tree: ./tests/integration/fixtures/behaviors/demo.xml
  parameters:
    - name: temp_setpoint
      type: double
      default: 25.0
      min: 10.0
      max: 50.0
    - name: control_enabled
      type: bool
      default: true
```

**Safe-state hooks (required to enter AUTO with actuators).** Automation that
drives actuating outputs must declare safe-state `mode_transition_hooks`; the
runtime refuses `MANUAL -> AUTO` (HTTP 409) without them — see
[safety.md](safety.md#manual-to-auto). Add a `*->FAULT` hook that drives actuators
to a safe value:

> This hook is **not** the software stop's safe state. `POST /v0/estop` runs
> `safety.safe_state`, which this gate does not check — declare both. A machine
> with the hook and no `safety.safe_state` passes the gate while its software stop
> has no safe state of its own: the stop runs this hook on its way into FAULT
> (except in IDLE, where the IDLE gate refuses `before_transition` calls) and
> reports `safe_state_achieved: false`. Once `safety.safe_state` declares a rung,
> the stop's latch refuses this hook and the rung alone drives the outputs.

A hook of a transition into `AUTO`, `FAULT -> MANUAL` or `IDLE -> MANUAL` is also
refused once a FAULT has been requested since its transition was requested. If
that happens before the transition commits, the transition does not commit (#775,
see [safety.md](safety.md#runtime-mode-safety-semantics)).

<!-- validate: runtime-config fragment -->

```yaml
automation:
  mode_transition_hooks:
    before_transition:
      - to: FAULT
        fail_on_error: false
        calls:
          - device_handle: sim0/motorctl0
            function_name: set_motor_duty
            args: { motor_index: 1, duty: 0.0 }
```

`fail_on_error` (default `true`) makes a failed `before_transition` call veto
the transition: the runtime issues none of the remaining calls and stays in the
current mode. On entry to `FAULT` it has no effect. FAULT cannot be vetoed, so
every call in every matching hook is attempted, each failure is logged at error
level, and the mode still changes to FAULT.

Supported types:

1. `double`
2. `int64`
3. `bool`
4. `string`

Constraint rules:

1. `min/max` are numeric-only.
2. `allowed_values` is string-only.

## Custom BT Nodes (Runtime)

Core runtime nodes include the following. `CheckQuality` and `CheckBool` are
Conditions; the rest are Actions.

1. `CallDevice`: calls `function_name` on `device_handle`. Its `args` is a
   JSON object after the `json:` prefix (`args='json:{"target":30.0}'`), or a
   blackboard reference holding one (`args="{target_args}"`); with no `args`
   it sends no arguments. Literal `args` that are not a JSON object refuse the
   tree when it loads. Args that cannot be read at tick fail the node and count
   as a failed call, so they reach FAULT after
   `automation.max_consecutive_call_failures`.
2. `ReadSignal`: writes a signal's value to its `value` port as a double, and
   its quality to `quality` with the names HTTP and events use (`OK`, `STALE`,
   `FAULT`, `UNAVAILABLE`; a quality the provider did not set is
   `UNAVAILABLE`). It reads numeric and bool signals (a bool as 1.0 or 0.0); a
   string, bytes or unset signal fails the node with the reason logged. Every
   failure writes `quality`: the reading's own, or `UNAVAILABLE` when there is
   no reading, such as a signal the cache does not hold. `value` is written
   only on SUCCESS, so read it only after one.
3. `CheckQuality`: SUCCESS when a signal's quality is `expected_quality`
   (default `OK`): one of `OK`, `STALE`, `FAULT` or `UNAVAILABLE`, the names
   HTTP and events use (`UNKNOWN` is accepted for `UNAVAILABLE`). Any other
   name refuses the tree when it loads, or fails the node when it comes from
   the blackboard.
4. `GetParameter`
5. `GetParameterBool`
6. `GetParameterInt64`
7. `CheckBool`
8. `PeriodicPulseWindow`: `active` is true for `pulse_s` at the start of
   each `interval_s`, or, with a positive `max_pulses_per_hour` (0, the
   default, is no cap), each 3600/`max_pulses_per_hour` seconds if that is
   longer. The schedule starts at the node's first enabled tick, and
   `startup_delay_s` counts from there. `enabled="false"` writes
   `active=false`, `pulse_index=-1` and `elapsed_ms=0` and restarts the
   schedule. The inputs are checked first, even when disabled: a schedule
   it refuses (`interval_s` or `pulse_s` 0 or less, `pulse_s` longer than
   `interval_s`, a negative `startup_delay_s` or `max_pulses_per_hour`)
   writes those same outputs and fails the node, keeping the schedule's
   start for when the inputs are valid again. A failing node stops a
   sequence, so catch the failure, for instance with a `Fallback`, where
   the tree must still command the output off.
9. `RequestedPulseWindow`: one pulse, `delay_s` after a request and
   `pulse_s` long, each time the `request` counter parameter increases.
   Requests that arrive while one runs wait their turn (`queued`), up to
   `max_queued` outstanding, the running one included (default 1): with
   the default, pressing again while a pulse is pending or running adds
   nothing. The
   counter's value when the tree starts is the baseline, so a start runs
   nothing. An operator panel increases the counter with an
   `operator.actions` button in the machine profile:

   ```xml
   <GetParameterInt64 param="sample_requests" value="{sample_requests}"/>
   <RequestedPulseWindow request="{sample_requests}" delay_s="{sample_delay_s}"
                         pulse_s="{sample_pulse_s}" active="{sample_active}" queued="{sample_queued}"/>
   ```

Use runtime capability metadata and parameter constraints to keep BT logic generic.

### Underscore attributes

Any node may carry these attributes, and no others that start with `_`:

- the pre and post conditions of the linked BehaviorTree.CPP. In 4.9.0,
  the version this runtime links, those are `_failureIf`, `_successIf`,
  `_skipIf`, `_while`, `_onSuccess`, `_onFailure`, `_onHalted` and `_post`;
- `_description`, and the `_uid` and `_fullpath` that BehaviorTree.CPP's
  writer adds to a tree saved with its metadata;
- `_autoremap`, which BehaviorTree.CPP accepts on any node and acts on only
  on a `SubTree`: on any other node it does nothing, and the check does not
  see it.

Any other underscore attribute on a node, such as a misspelt `_skipif` or
the `_onlyIf` that BehaviorTree.CPP 4.11 and Groot2 offer, refuses the tree
when it loads and at `--check-config`, naming the node and the attribute.
BehaviorTree.CPP alone would store such an attribute and ignore it, so a
misspelt guard would never stop the node. The check covers the nodes the
tree runs; an attribute on a `<BehaviorTree>` element itself, or in a tree
nothing references, is not checked.

### Scripts and conditions

A `Script`, `ScriptCondition` or `Precondition`, or a pre or post condition,
that reads a blackboard name nothing in the tree sets refuses the tree when
it loads and at `--check-config`, naming the node and the name. A name is set
when an output port writes it (`value="{name}"`), when `SetBlackboard`
names it with a literal `output_key="name"`, when a literal on a `SubTree`
port (or the port's model default) gives it a value as the tree is built,
or when a script creates it with `:=`. `=`, `+=` and the like assign only to
a name that exists: one a port declares, or one created with `:=`.
`SetBlackboard output_key="{name}"` does not set `name`: it reads the name of
the entry to write from `name`. Without the check the tree loaded and its
first tick in AUTO threw, which the runtime turns into FAULT.

The check follows the blackboard each node sees, so a name set in the main
tree does not count inside a `SubTree` that neither remaps it nor sets
`_autoremap`; a root name (`@name`) is seen from any SubTree. It does not
follow the order nodes run in: a name set anywhere in the tree counts as
set, so a script that reads a name before the node that sets it has run, or
after an `UnsetBlackboard` removed it, still throws at that tick. A name a
script creates with `:=`, or a literal `SetBlackboard` key, counts as set in
every SubTree. Some reads that throw at tick still load: `x := x + 1` where
nothing else sets `x`, and `x += 1` where a port declares `x` but nothing
writes it. A script passed into a `SubTree` through a port (`code="{code}"`,
with the script as a literal on the `SubTree` element) is read from that
literal.

### Inputs from the blackboard

A node input left out of the tree takes its default. An input given as a
blackboard reference (`enabled="{en}"`) must be readable when the node
ticks: an entry nothing has written, or one whose value cannot be converted
to the port's type (the string `soon` for a number), fails the node, naming
the port, instead of falling back to the default. A
`PeriodicPulseWindow` whose `enabled` entry was never written would
otherwise run on its default schedule.

When they fail, `EmitOnChangeOrInterval` writes `emit=false` and
`BuildArgsJson` writes an empty `json`, so a `Fallback` that catches the
failure acts on neither the last tick's emit nor its arguments. A
`CallDevice` whose `args` reads an empty value fails as a counted call
instead of sending one with no arguments.

### Empty attributes

An empty attribute on a port of an Anolis node (`now_ms=""`) is read as the
port left out when the port has no default or an empty default, so the
node runs as if the attribute were not there. Groot2 1.10.0 with its default preferences
saves every port a node's model declares, writing `attr=""` for each one the
author left empty, and a tree saved that way loads. An empty attribute on a
port whose default is not empty (`interval_s=""` on `PeriodicPulseWindow`)
refuses the tree, naming the node and the port: leave the port out to take
the default, or give it a value.

Three cases keep an empty attribute as written, as before: an input the
node needs, with no fallback (`device_handle`, `function_name`,
`signal_id`, `param`, `CheckBool`'s `value`, `RequestedPulseWindow`'s
`request` and `EmitOnChangeOrInterval`'s `key`), which the config check or
the node treats as it did; `args=""` on
`CallDevice`, which means no arguments; and every port of a
BehaviorTree.CPP node, such as `SetBlackboard value=""`, which sets an
empty string.

The cost is that an empty attribute cannot pass an empty string: a lone
`arg1_str=""` on `BuildArgsJson` is a slot with a name and no value, and
the node fails. Pass an empty string from the blackboard instead
(`arg1_str="{empty}"`). Only the tree file the config names is read this
way; a file it brings in with `<include>` is not.

### SubTrees

Anolis nodes work in a `SubTree` at any depth, with or without
`_autoremap`: they take the runtime's services from the tree's root
blackboard. A SubTree's own blackboard still holds its own keys, so a value
one node writes inside a SubTree is seen outside it only through a port
remapping, `_autoremap="true"`, or a `{@key}` port, which BehaviorTree.CPP
writes to the root blackboard.

## Quick Walkthrough

### 1) Start runtime with automation enabled

`examples/anolis-runtime.yaml` ships with `automation.enabled: false`. Before
starting, set `automation.enabled: true` and `behavior_tree`, and add the
`parameters` and `mode_transition_hooks` blocks from
[Runtime Parameters](#runtime-parameters). Without the hook, step 4 returns
HTTP 409.

```bash
# Linux
./build/dev-release/core/anolis-runtime --config ./examples/anolis-runtime.yaml

# Windows
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\examples\anolis-runtime.yaml
```

### 2) Verify startup mode

```bash
curl -s http://127.0.0.1:8080/v0/mode | jq
```

Expected mode at startup: `IDLE`.

### 3) Inspect and update parameters

```bash
curl -s http://127.0.0.1:8080/v0/parameters | jq

curl -s -X POST http://127.0.0.1:8080/v0/parameters \
  -H "Content-Type: application/json" \
  -d '{"name":"temp_setpoint","value":30.0}' | jq
```

### 4) Move into AUTO

```bash
curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"MANUAL"}' | jq

curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"AUTO"}' | jq
```

### 5) Check automation status

```bash
curl -s http://127.0.0.1:8080/v0/automation/status | jq
```

### 6) Optional: inspect loaded tree

```bash
curl -s http://127.0.0.1:8080/v0/automation/tree | jq
```

## Troubleshooting

1. `UNAVAILABLE` from automation endpoints:
   - automation disabled in config or compiled out.
2. BT load failure:
   - invalid tree path/XML, an unregistered node, a port type mismatch, an
     underscore attribute the runtime does not run (see
     [Underscore attributes](#underscore-attributes)), or a literal `param`
     or `device_handle` the config does not declare: a parameter missing
     from `automation.parameters` or of a type the node does not read
     (`GetParameter` reads double or int64, `GetParameterBool` bool,
     `GetParameterInt64` int64), a provider missing from `providers[]`, or
     a `device_handle` that is not `provider_id/device_id`.
     A name passed into a `SubTree` through a port is not checked. A script
     or condition that reads a name nothing in the tree sets is refused too
     (see [Scripts and conditions](#scripts-and-conditions)).
     `anolis-runtime --check-config` loads the tree and reports these before
     a start. A relative `automation.behavior_tree` is resolved against the
     config file's directory.
3. Manual calls blocked unexpectedly:
   - verify mode and `manual_gating_policy`.
4. Parameter update rejected:
   - check type/constraint mismatch against declared parameter.

## Safety Reminder

Automation provides orchestration policy, not safety certification.
External safety systems (E-stop/interlocks/watchdogs) remain required for real hardware deployments.
