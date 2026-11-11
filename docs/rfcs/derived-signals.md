# RFC: derived signals

Status: draft. Nothing is implemented and nothing here is accepted. This
version is a register of decisions: a few proposed, most open, each open one
with what would settle it.

## The problem

A signal today is what a provider reads from a device. Nothing between the
state cache and its consumers computes one value from another:

- The behavior tree's nodes read a signal, read a parameter, call a device
  and build call arguments ([automation.md](../automation.md), "Custom BT
  Nodes"). None does arithmetic, compares two values or remembers a previous
  one. The tree runs only in `AUTO`.
- Telemetry records device signals, health, and mode and parameter changes
  (`core/telemetry/influx_sink.hpp`). A value worked out inside the tree is
  recorded nowhere.
- A provider sees only its own devices. ADPP carries nothing from the runtime
  to a provider except call arguments.

So a machine that needs a computed value gets it from a person or from a
script beside the runtime. On the bioreactor reference rig
([anolis-projects](https://github.com/anolishq/anolis-projects)
`projects/bioreactor-v1`):

- `scripts/heater_guard.py` polls the HTTP API and sends the software stop on
  over-temperature, on no good reading for 120 s, and on the pad reading
  below the liquid for 30 minutes. Its docstring gives the reason: "the
  runtime has no signal thresholds".
- The operator chooses the heater pad setpoint from
  `pad ≈ target + k × (target − room)` and corrects the liquid thermocouple's
  bias by hand (`docs/runbook.md`, "Heater").
- pH and dissolved-oxygen temperature compensation is an operator parameter,
  not the measured liquid temperature.
- "Is the reading steady" before a calibration is judged by eye.
- Relay duty is worked out after a run from exported telemetry.

A derived signal is a value computed from other signals and published as a
signal, so that the tree, an operator, telemetry and a dashboard read it the
way they read a device signal.

## Evidence

`tools/derived-signals/` in this repository holds a paper exercise: nine quantities from that rig written as plain code,
run against its recorded telemetry, then written in three declaration
styles. Its README has the cases, the method and the limits. In short:

- The reference detectors trip at the same tick, for the same reason, as
  `heater_guard.py` on five synthetic traces.
- Replayed over one overnight heater run and one pH calibration, the
  reference agrees with what was recorded on the rig at the time: hourly
  relay duty within one point, the pH reading turning steady before each
  calibration call, and a predicted liquid temperature of 24.9 °C against
  about 25 °C on a thermometer the next morning.
- Of the nine, seven are needs and two are stress cases (an estimator and a
  total that outlives a restart). Nothing on the rig needs an estimator
  today.

The findings that bear on a decision are cited under it below as "exercise".

## Proposed

These are the starting position. Each stands until one of the open decisions
below contradicts it.

### P1. A derived value is a signal

It has the identity every signal has, `provider_id/device_id/signal_id`, and
a unit and a quality, and it enters the state cache. HTTP state, events,
telemetry, the tree's `ReadSignal` and a machine profile's `operator.readings`
then read it unchanged. Reusing the triple under a reserved provider id
changes no closed contract: the telemetry tag set
(`schemas/telemetry/telemetry-timeseries.schema.v1.json`) and the
machine-profile signal pattern stay as they are.

Prior art agrees: an EPICS calc record, an Ignition expression tag and a Home
Assistant helper entity are each peers of measured values.

### P2. The layer produces signals and calls nothing

Every device call goes through the call router from an operator, the tree or
the safe state ([architecture.md](../architecture.md), "Key Invariants").
A derived signal can be the reason a call is made. It does not make one.

### P3. Evaluation is periodic, at one instant, in dependency order

All derived signals are evaluated together, each from the latest cached value
of its inputs, in an order fixed when the definition is loaded. A dependency
loop is a load error unless it passes through an explicit delay. A derived
signal may read another (exercise: the detach detector had to read the
de-glitched pad temperature, not the raw one).

Evaluating on every input change is rejected: each input arrives at its own
time, so one evaluation would see a mix of old and new values and the result
would depend on arrival order.

### P4. A value that is not good is never used as a number

A derived signal whose input is not good is not good, unless its declaration
says what to do instead. The opposite defaults are the documented regrets
elsewhere: EPICS links that do not propagate severity unless asked, and Home
Assistant templates that turn `unavailable` into 0.

### P5. Declarations are typed blocks; formulas are one block type

A definition is a graph of blocks, each with a type, named inputs, settings
and named outputs, some of them published as signals. Arithmetic is an
`expr` block whose inputs are declared. This is the shape that held all nine
cases in one form (exercise):

- One expression per signal was shortest, and could not express a block with
  two outputs or shared state.
- A source with a filter chain read best for single-signal cases, and needed
  a second or third form for 9 of its 21 nodes.
- Blocks cost length in arithmetic, which the `expr` block removes.

A block has the same shape as a behavior-tree node and a provider's config
schema: a type and a set of declared settings that a schema can check.

## Open

Ordered roughly by how much else waits on each.

### O1. Quality, and the "not ready" state

A derived signal needs to say three things: a good value, not ready yet (a
window still filling, a filter not started), and bad input. ADPP has
`OK`, `STALE`, `FAULT` and `UNKNOWN`. HTTP and telemetry report `UNKNOWN` as
`UNAVAILABLE` (`core/http/json.cpp`, `core/state/state_cache.cpp`); the
tree's nodes report it as `UNKNOWN` (`core/automation/bt_nodes.cpp`).

Question: do the three states map onto the existing values, or does "not
ready" need its own?

Settled by: a table of every producer and consumer of quality in the runtime,
the protocol and the workbench, with what each would show for the three
states.

### O2. Bad input inside a stateful block, and three-valued booleans

Exercise: one bad pad reading in the middle of a 30-minute hold decided the
outcome. `heater_guard.py` resets its timer and never trips; a detector that
holds through the bad reading trips at 1902.5 s. No style made the author
choose. And with the pad reading bad, "pad above 70" is neither true nor
false, so a `trip` that ORs four detectors needs a rule for unknown operands.

Question: what is the default policy of each stateful block, is stating it
required, and what does a consumer of a boolean see when it is unknown?

The spike had to pick rules to run at all. They are a starting point, not a
decision:

- A stateful block has no default. `held_for`, `window_stats` and
  `integrate` must each state `on_bad`, and a file that leaves it out does
  not load.
- `and` and `or` are three-valued: one true operand decides an `or`, one
  false operand an `and`, and otherwise an operand that is not good makes
  the result not good.
- `held_for` with `on_bad: reset` gives its input's bad state as its output
  while the input is bad.
- A block may name a fallback for an output that is not good. Nothing marks
  the result as a fallback, so a consumer cannot tell it from a computed
  value. That belongs with O1.

Settled by: the rule table for every block type, with a synthetic trace for
each row.

### O3. The block catalogue and its schema

The exercise used 17 block types. Several overlap, and it was one rig.

Question: what is the smallest catalogue that covers the seven needs, and
how is a block type's settings schema written and checked? A misspelt
setting was caught in none of the three styles; catching it needs the
catalogue.

A spike has answered the first half. `tools/derived-signals/engine.py` runs
a block file off the runtime, in Python:

- Ten block types cover all nine cases: `median`, `expr`, `all_good`,
  `since_true`, `latch`, `held_for`, `window_stats`, `deadband`, `kalman1`
  and `integrate`. Arithmetic, comparison and logic are all `expr`.
- Its outputs equal the reference at every tick of the overnight run and on
  every synthetic trace, and it trips where `heater_guard.py` trips.
- Eleven mistakes in a block file are refused at load, the misspelt setting
  among them.

It is the oracle a later implementation is tested against, and a way to run
a definition over recorded data before trusting it. The exercise's own first
draft of the compensation signal rounded to 0.5 °C and changed value 111
times in one night; a deadband changed 8 times. Only the replay showed it.

Recorded data is not enough of an oracle by itself. With the engine's two
window policies swapped, the comparison still passed, because the recordings
have no bad sample inside a window. Each bad-input policy needs a synthetic
trace of its own.

Still open: how a block type's schema is published (in the spike it is
Python), who may add a type, whether the `expr` language is the right one
(the spike uses a small subset of Python's expression syntax), and whether
units are checked or only carried.

### O4. A home for machine constants

Five of the nine cases read a number that is neither a signal nor an operator
setting: the thermocouple bias, the loss coefficient, the heater's wattage,
the filter tuning. Today they are in the runbook. `automation.parameters`
is the only place for a number, and it is the operator's.

Question: where does a constant found on the bench live, and can an operator
change it? The spike names them in a `constants` map at the top of the block
file, beside the blocks that use them.

### O5. State across a restart

A running total wants to survive a runtime restart and to reset when a run
starts. A window or a median wants neither. Parameters set over HTTP are
already not persisted ([automation.md](../automation.md), "Runtime
Parameters").

Question: which block state persists, where, and which events reset it?

### O6. From a detector to an action, and the safety boundary

`heater_guard.py` ends in `POST /v0/estop`. A derived boolean alone does not
replace it: the tree has no node that requests a stop or `FAULT`. And this is
a derived signal feeding a safety function. The stop chain already accepts
any cached boolean signal with quality `OK` as its status
(`core/control/stop_chain.cpp`), so a derived signal could be named there
today unless something forbids it.

Question: how does a derived boolean cause a stop or `FAULT`, and which
safety settings may name a derived signal?

Settled by: reading it against [safety.md](../safety.md),
[stop-chain.md](stop-chain.md) and
[provider-declared-safe-state.md](provider-declared-safe-state.md).

### O7. Time

- Polling is one thread reading devices one after another, and a successful
  call re-reads its device from the caller's thread
  (`core/control/call_router.cpp`). There is no moment at which the cache is
  one consistent snapshot.
- A derived statistic cannot be better than the poll. Exercise: a relay with
  a 10 s period, polled every 2.5 s, gives an hourly duty that agrees with
  the rig's record and a 10-minute duty that swings by about ten points.
- Telemetry rows are written on change and stamped when the event is emitted,
  not when the provider sampled (`core/events/event_types.hpp`). Exercise:
  recorded data replays values, not freshness, so detectors on age could
  only be tested on synthetic traces.

Question: what period does the layer run at, how old may an input be, and
what would telemetry need to record for a definition to replay faithfully?

Settled by: measuring input age and cross-device skew on recorded data.

### O8. Identity and the contracts touched

P1 reuses the triple. Still open: the reserved provider id, what the middle
segment means when there is no device (the exercise invented `heater`, `ph`
and `probes`), and what the surfaces that list providers show.
`GET /v0/providers/health` and the health telemetry enumerate provider
processes (`core/health/health_snapshot.cpp`); `GET /v0/state` enumerates
devices.

Settled by: a list of every schema and endpoint that changes, with the
version bump each needs under [dependencies.md](../dependencies.md).

### O9. Where the definition lives

In the runtime config beside `automation`, in a file of its own that the
config names (as it names the behavior tree), or in the machine profile.

### O10. Where the layer runs

Left until last on purpose: O1 to O8 decide what any placement has to
provide. The candidates and what is known about each:

- **In the runtime.** Signals enter the state cache directly, so P1 costs
  nothing more. The computation shares a process with the stop logic, and on
  the poll thread a slow evaluation would delay every device read.
- **In a provider.** Process isolation, and any language through an SDK. It
  needs a way to receive other providers' signals, which ADPP does not have.
  Reading the runtime's HTTP API back does not work at startup: providers
  start before the HTTP server (`core/runtime/runtime.cpp`).
- **Both.** Established systems tend to have two tiers: light calculations
  beside the measured values, and heavy estimators as separate processes
  that publish to the same place (ROS 2 `robot_localization`, the PX4
  estimator).

Settled by: two throwaway spikes, one per placement, each running the same
block file against the oracle from O3.

## Rejected

- **Computing in the behavior tree.** The tree runs only in `AUTO`, and its
  blackboard is visible to nothing. Whether the tree needs arithmetic of its
  own is a separate question.
- **Computing in the historian only.** InfluxDB tasks or Grafana queries can
  show a derived value. Nothing on the machine can act on it, and the
  definition would live outside the machine's configuration.
- **One expression per signal, or filter chains, as the whole form.** See P5.
- **FluxGraph as the engine, as it stands.** Its edges take one input and
  give one output, a signal is a number and a unit with no quality or time,
  and a missing input reads as 0 or the last value. Its compile-time rules
  are the ones P3 copies: one writer per signal, a fixed evaluation order,
  loops only through a delay. Revisit if one plant model is wanted for both
  the simulator and an estimator.

## Not in scope

- Estimators, as a need. The design should not rule them out (P5 is partly
  chosen for that), and nothing asks for one yet.
- Any change to ADPP. O10 may propose one; this RFC does not.
- Workbench surfaces.

## How this proceeds

1. This draft is reviewed by someone who did not write it, against the
   evidence, before any decision is marked accepted.
2. The open decisions are worked one at a time, each by the method named
   under it, and each lands here as a change to this file.
3. O3's spike came first, so that O1, O2 and O5 can be tested against it and
   not only argued.
4. O10 is decided last, and reviewed independently before it is.
5. Accepted, the RFC becomes an epic with increments.

## Prior art

Where the computation lives, and how quality passes through it:

- [EPICS calc records](https://docs.epics-controls.org/projects/base/en/latest/calcRecord.html):
  in the same database as the I/O records. Alarm severity passes along a
  link only when the link asks for it.
- [Ignition expression tags](https://www.docs.inductiveautomation.com/docs/8.1/platform/tags/types-of-tags):
  in the central tag provider, evaluated per tag group. A tag that reads
  others usually takes the worst of their qualities.
- [Home Assistant filter](https://www.home-assistant.io/integrations/filter/)
  and [threshold](https://www.home-assistant.io/integrations/threshold/)
  helpers: chained filters in order; a threshold with hysteresis.
- [Prometheus recording rules](https://prometheus.io/docs/prometheus/latest/configuration/recording_rules/):
  rules in a group run in sequence "with the same evaluation time".
- [FMI](https://fmi-standard.org/docs/3.0.1/): a model packaged once with
  its whole state saved and restored, which the standard names as a use for
  nonlinear Kalman filters.
