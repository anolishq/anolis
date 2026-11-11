# RFC: derived signals

Status: draft, second revision. Nothing is implemented and nothing here is
accepted. This is a register of decisions: a few proposed, most open, each
open one with what would settle it. The first revision was reviewed
independently; [the review](derived-signals-review.md) lists what it found
and what changed.

## The problem

A signal today is what a provider reads from a device. Nothing between the
state cache and its consumers computes one value from another:

- The behavior tree's own nodes read a signal, read a parameter, call a
  device, build call arguments, compare a boolean and schedule pulses
  (`core/automation/bt_runtime.cpp`). None does arithmetic on a reading,
  smooths it or keeps a window of it. The tree runs only in `AUTO`.
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

`tools/derived-signals/` in this repository holds a paper exercise: nine
quantities from that rig written as plain code, run against its recorded
telemetry, then written in three declaration styles. Its README has the
cases, the method and the limits. In short:

- The reference detectors trip at the same tick, for the same reason, as
  `heater_guard.py` on six synthetic traces.
- Replayed over one overnight heater run, hourly relay duty is within 1.1
  points of the figures worked out on the rig at the time. Those came from
  another script over the same telemetry, so this is two calculations
  agreeing, not a measurement.
- Replayed over one pH calibration, the steady detector turned true before
  both calibration calls in the window: 14 minutes before the first and 104
  seconds before the second. A tighter threshold would have missed the
  second.
- The steady-state relation gives 24.9 °C for the liquid against about 25 °C
  on a thermometer the next morning. Its coefficient was fitted on runs that
  include this one, so this checks the arithmetic, not the model.
- Of the nine, seven are needs and two are stress cases (an estimator and a
  total that outlives a restart). Nothing on the rig needs an estimator
  today.

The findings that bear on a decision are cited under it below as "exercise".

## Open before anything else

### O0. Whether a new layer is needed

The strongest case against building this:

- **Most of the needs read one device.** Of the seven, the glitch filter,
  the bias correction, the heater trip and the relay duty read only the
  heater board, and the steady test reads only the pH probe. A provider can
  compute a value from its own device's readings, and firmware can do more:
  the runbook's own diagnosis of the heater guard is that "nothing in the
  runtime or the gen1 firmware limits temperature". A trip inside the
  provider or the firmware can turn its own heater off in any mode, with no
  runtime involved, which no layer above the runtime can do.
- **The compensation case is a call the tree already makes.** It needs a
  reading, a quality check and an offset, and then smoothing and a deadband
  (exercise: without them the value changed 110 times in one night).
- **Only the pad and liquid relation crosses providers**, and it is an aid
  to the operator, not a control input.
- **The tree may already be able to compute.** The BehaviorTree.CPP library
  the runtime links has a scripting language with arithmetic and
  comparison, and [safety.md](../safety.md) already describes precondition
  scripts in trees. No tree or test here uses it.

What survives of the case for it:

- The heater stays on in `MANUAL`, where the tree does not run, so a tree
  cannot guard it.
- A value computed in the tree is not a signal: no telemetry, no operator
  panel, no dashboard.
- A computation in a provider is written again in every provider that needs
  it, in that provider's language, and a provider author cannot know the
  machine's constants.
- A value that crosses providers has nowhere to live.

Question: is the need a general layer, or a small set of fixes in other
places (a threshold check in the heater firmware or provider, tree
scripting, one smoothing node)?

Settled by: writing each of the seven needs in the place the case against
would put it, and seeing which are left.

The rest of this document assumes the answer is a layer. If it is not, most
of it falls away.

## Proposed

These are the starting position. Each stands until an open decision
contradicts it.

### P1. A derived value is a signal

It has the identity every signal has, `provider_id/device_id/signal_id`, and
a unit and a quality. Reusing the triple under a reserved provider id leaves
two closed contracts as they are: the telemetry tag set
(`schemas/telemetry/telemetry-timeseries.schema.v1.json`) and the
machine-profile signal pattern.

It is not free, which the first revision said it was:

- `GET /v0/state` lists devices from the device registry and looks each one
  up in the state cache (`core/http/handlers/state_handlers.cpp`). A value
  in the cache with no registry entry is not served.
- Units and descriptions are in the registry's capabilities, not in the
  cache.
- The registry is filled only by provider discovery, and
  [architecture.md](../architecture.md) states "Registry changes only on
  provider restart".
- Nothing reserves a provider id today. `providers[].id` accepts any
  non-empty string.

So a derived signal needs a registry entry that no provider published, or
the handlers and the cache have to change. O8 owns that.

Prior art agrees on the direction: an EPICS calc record, an Ignition
expression tag and a Home Assistant helper entity are each peers of measured
values.

### P2. The layer produces signals and calls nothing

Every device call goes through the call router from an operator, the tree or
the safe state ([architecture.md](../architecture.md), "Key Invariants").
A derived signal can be the reason a call is made. It does not make one.

This moves a question; it does not answer it. Something else has to act on a
derived trip when the tree is not running. O6 owns that.

### P3. Evaluation is periodic, in dependency order

All derived signals are evaluated together, once per period, in an order
fixed when the definition is loaded. A dependency loop is a load error
unless it passes through an explicit delay.

The reason is a deterministic number of evaluations: one per period,
whatever the inputs did. Evaluating on each input change would run a block
a varying number of times, and a window or a timer would then mean a
different span each time. Periodic evaluation does not make the inputs one
consistent snapshot; O7 owns that, and the period.

### P4. A value that is not good is never used as a number

A derived signal whose input is not good is not good, unless its declaration
says what to do instead. The opposite defaults are the documented regrets
elsewhere: EPICS links that do not propagate severity unless asked, and Home
Assistant templates that turn `unavailable` into 0.

"Good" is not yet defined, and O1 has to define it. This holds only inside
the layer: `ReadSignal` gives the tree a number whatever its quality.

### P5. Declarations are blocks with declared inputs

A definition is a graph of blocks. A block has a type, named inputs,
settings and named outputs, some of them published as signals. About half
the blocks in the worked file are formulas and the rest hold state
(a window, a timer, a filter). The form is closer to "expressions, plus
function blocks for anything with state" than to a block diagram.

The exercise compared three styles and does not by itself carry this
choice: the comparison was written by one person, and the expression style
was not allowed unpublished intermediates or multi-output calls, which is
most of why it failed two cases. What it does show:

- A value with state and two outputs (an estimator) needs something that is
  not a single expression.
- A source with a chain of filters cannot take two inputs.
- Whatever the form, a misspelt setting is caught only with a catalogue of
  types and their settings.

A typed block matches how a behavior-tree node's ports are declared. It does
not match how the runtime reads its own config, which is parsed by hand and
warns on an unknown key (`core/runtime/config.cpp`). Refusing a definition
at load is a new convention here.

## Open

Ordered roughly by how much else waits on each.

### O1. Quality, and the "not ready" state

A derived signal needs to say three things: a good value, not ready yet (a
window still filling, a filter not started), and bad input.

What exists:

- ADPP has `OK`, `STALE`, `FAULT` and `UNKNOWN`.
- HTTP and telemetry report `UNKNOWN` as `UNAVAILABLE`
  (`core/http/json.cpp`, `core/state/state_cache.cpp`), and both publish
  quality as a closed set of four names.
- The tree's nodes report it as `UNKNOWN`, and `CheckQuality` does not
  recognise `UNAVAILABLE` (`core/automation/bt_nodes.cpp`).
- "Not good" has at least four shapes in the cache: a quality that is not
  `OK`; no value at all; a value that is old; and a value whose provider is
  down. In the last case the cache keeps the old value with its old quality
  and marks the provider unavailable separately
  (`core/state/state_cache.cpp`).

Questions: which of those four make an input not good; do the three states
map onto the existing names or does "not ready" need its own, which would be
a breaking change to two published enumerations; and how is a value that
came from a fallback told apart from one that was computed.

Settled by: a table of every producer and consumer of quality in the
runtime, the protocol, the exporter and the workbench, then a decision on
the enumeration.

### O2. Bad input inside a stateful block, and three-valued booleans

Exercise: one bad pad reading in the middle of a 30-minute hold moves the
trip from 1902.5 s to 2805.0 s, because `heater_guard.py` restarts its
timer. A detector that holds through the bad reading trips at 1902.5 s. No
style made the author choose. And with the pad reading bad, "pad above 70"
is neither true nor false, so a `trip` that ORs four detectors is not good
on every tick where a thermocouple is bad and nothing has tripped yet.

Question: what is the default policy of each stateful block, is stating it
required, and what does a consumer of a boolean see when it is unknown?

The spike had to pick rules to run at all. They are a starting point, not a
decision:

- `held_for`, `window_stats` and `integrate` must each state `on_bad`, and a
  file that leaves it out does not load. `median`, `kalman1`, `latch`,
  `since_true` and `deadband` have no such setting, and `median` goes on
  reporting its last good values through any length of bad input, which
  breaks P4.
- `and` and `or` are three-valued: one true operand decides an `or`, one
  false operand an `and`, and otherwise an operand that is not good makes
  the result not good.
- `held_for` with `on_bad: reset` gives its input's bad state as its output
  while the input is bad. With `hold` its timer runs on and can trip while
  the input is still bad.
- A block may name a fallback for an output that is not good. On a block
  with two outputs it replaces both.

Settled by: the rule table for every block type and each of bad and not
ready, with a synthetic trace for each row, compared at every tick.

### O3. The block catalogue and its schema

The exercise used 17 block types. Several overlap, and it was one rig.

Question: what is the smallest catalogue that covers the seven needs, and
how is a block type's settings schema written and checked?

A spike has answered part of the first half. `tools/derived-signals/engine.py`
runs a block file off the runtime, in Python:

- Ten block types express all nine cases, with arithmetic, comparison and
  logic all in `expr`. The ninth case is expressed and not implemented: the
  total neither persists nor resets.
- On the overnight run, eleven of its seventeen published signals equal the
  reference at every tick. The trip signal goes true where
  `heater_guard.py` trips on the synthetic traces.
- Eleven mistakes in a block file are refused at load, the misspelt setting
  among them.

It is not yet the oracle it is meant to become. The review broke the engine
37 ways and the comparison caught 4:

- The recorded night has no bad sample, so nothing in it tests bad input,
  three-valued logic or a fallback.
- The comparison treats bad and not ready as the same thing.
- The engine and the reference were written by one person, one after the
  other, and share their arithmetic and their sample-and-hold grid.
- The synthetic traces compare only the first trip.

So recorded data is not enough of an oracle, and neither is one author's
second implementation. The check needs a synthetic trace for each row of
O2's table, compared by state at every tick, and expected values worked out
independently of the engine.

A replay still earned its place: the exercise's first draft of the
compensation signal rounded to 0.5 °C and sent 111 values in one night,
where a deadband sent 8. Only the replay showed it.

Still open: how a block type's schema is published (in the spike it is
Python), who may add a type, whether the `expr` language is the right one
(the spike uses a small subset of Python's expression syntax), whether
units are checked or only carried, what a not-a-number reading with quality
`OK` does, and which settings are checked for range at load.

### O4. A home for machine constants

Five of the nine cases read a number that is neither a signal nor an operator
setting: the thermocouple bias, the loss coefficient, the heater's wattage,
the filter tuning. Today they are in the runbook. `automation.parameters`
is the only place for a number, it is the operator's, and it exists only
when automation is enabled.

Question: where does a constant found on the bench live, can an operator
change it, and may a derived signal read an operator parameter at all? The
spike names constants in a map at the top of the block file and also reads
two operator parameters.

### O5. State across a restart

A running total wants to survive a runtime restart and to reset when a run
starts. A window or a median wants neither. Parameters set over HTTP are
already not persisted ([automation.md](../automation.md), "Runtime
Parameters").

Question: which block state persists, where, and which events reset it?

### O6. From a detector to an action, and the safety boundary

`heater_guard.py` ends in `POST /v0/estop`. A derived boolean alone does not
replace it: the tree has no node that requests a stop or `FAULT`, and does
not run in `MANUAL`, where the heater stays on.

Two things are already clear:

- **A derived signal must not be a stop chain's status.** The stop chain
  observes a stop that has already happened. When its status reads in force
  or unknown, the runtime holds the latch and sends no safe-state calls
  (`core/control/safe_state.cpp`). A derived over-temperature trip named
  there would latch the machine and leave the heater on, and one that is not
  ready at startup would hold on every boot. Nothing forbids it today: the
  stop chain accepts any cached boolean with quality `OK`
  (`core/control/stop_chain.cpp`).
- **Acting on a derived trip is a new cause of an autonomous stop.**
  [safety.md](../safety.md) lists the cases in which the runtime enters
  `FAULT` by itself. A derived trip would add one, and it needs a stated
  direction of failure: what happens when the trip is not good.

Question: is acting on a derived signal in scope at all, and if so what
invokes the software stop, in which modes, and what does a trip that is not
good do? `heater_guard.py` also stops on a failed HTTP read, which nothing
inside the runtime can observe.

Settled by: O0 first. If the heater's trip belongs in its firmware or
provider, the remaining needs are aids to the operator and this decision
reduces to the prohibition above.

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
- If evaluation shares the poll thread and a provider read blocks, a
  detector for "no good reading for 120 s" does not run for as long as the
  read blocks.

Question: what period does the layer run at and on which thread, how old may
an input be, and what would telemetry need to record for a definition to
replay faithfully?

Settled by: measuring input age and cross-device skew on recorded data.

### O8. Identity and the contracts touched

P1 reuses the triple. Still open:

- How a derived signal gets a registry entry, against the invariant that the
  registry changes only on provider restart.
- How the provider id is reserved, and what happens to a config that already
  uses it.
- What the middle segment means when there is no device. The exercise
  invented `heater`, `ph` and `probes`.
- What the surfaces that list providers show. `GET /v0/providers/health` and
  the health telemetry enumerate provider processes
  (`core/health/health_snapshot.cpp`); the poll loop reports a device whose
  provider is missing as unavailable.
- Whether a derived number that changes a little on every evaluation floods
  events and telemetry, which are written on any change of value.

Settled by: a list of every schema, endpoint and loop that changes, with
the version bump each needs under [dependencies.md](../dependencies.md).

### O9. Where the definition lives

In the runtime config beside `automation`, in a file of its own that the
config names (as it names the behavior tree), or in the machine profile.
This decides how P5's load-time refusal is implemented, so it comes before
P5 is accepted.

### O10. Where the layer runs

Left until O1 to O9 have said what any placement has to provide. The
candidates and what is known about each:

- **In the runtime.** No new process and no protocol change. The
  computation shares a process with the stop logic, a slow evaluation on the
  poll thread would delay every device read, and P1's registry entry still
  has to be made.
- **In a provider.** Process isolation, any language through an SDK, and a
  registry entry by the ordinary route, which answers most of O8. It needs
  a way to receive other providers' signals, which ADPP does not have.
  Reading the runtime's HTTP API back does not work at startup: providers
  start before the HTTP server (`core/runtime/runtime.cpp`).
- **Both.** Established systems tend to have two tiers: light calculations
  beside the measured values, and heavy estimators as separate processes
  that publish to the same place (ROS 2 `robot_localization`, the PX4
  estimator).

Settled by: two throwaway spikes, one per placement, each running the same
block file against the oracle from O3. They have to include the registry
and health integration, which is where the cost is, not only the
computation.

## Rejected

- **Computing in the behavior tree only.** The tree runs only in `AUTO`, and
  its blackboard is visible to nothing. Whether the tree should compute for
  its own use, with the scripting it already links, is part of O0.
- **Computing in the historian only.** InfluxDB tasks or Grafana queries can
  show a derived value. Nothing on the machine can act on it, and the
  definition would live outside the machine's configuration.
- **FluxGraph as the engine, as it stands.** Its edges take one input and
  give one output, so combining signals needs a compiled model. A signal is
  a number and a unit with no quality or time, and a missing input reads as
  0 or the last value. Its compile-time rules are the ones P3 copies: one
  writer per signal, a fixed evaluation order, loops only through a delay.
  Revisit if one plant model is wanted for both the simulator and an
  estimator.

Not rejected, and decided under O0: computing in the device's own provider
or firmware.

## Not in scope

- Estimators, as a need. The design should not rule them out, and nothing
  asks for one yet.
- Any change to ADPP. O10 may propose one; this RFC does not.
- Workbench surfaces.

## How this proceeds

1. O0 is worked first: each of the seven needs written where the case
   against would put it.
2. If a layer is still wanted, O1 and O2 come next, with the engine check
   rebuilt as O3 describes so that they can be tested and not only argued.
3. The remaining open decisions are worked one at a time, each by the method
   named under it, and each lands here as a change to this file.
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
