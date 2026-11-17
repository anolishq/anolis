# RFC: derived signals

Status: accepted for a first version, whose derived signals are read
and not acted on. Fifth revision. Nothing is implemented but a spike
that is not for merging. A second stage, in which a graph's output is
handed to an actuator, is not accepted: its conditions are under "What
the first version is for". This is a register of decisions. Each is marked as one of
three things. **Decided** is a choice that has been made: to build the
layer (O0), the engine (O11) and that a build may leave it out, what the
first version is for (below), how a graph's outputs are published, what
stands for the first version of all that is proposed, who bounds a command (P2), where a definition lives (O9), that no
provider id is reserved (O8), and what a derived signal may not be used
for (O6). **Proposed** is a claim about how the layer should behave,
with the evidence it has beside it, which is often thin. **Open** has
what would settle it. The first revision and the fourth were each
reviewed independently; [the review](derived-signals-review.md) lists
what the first found, and what the second found is worked into this
text.

## The problem

Anolis presents every device as signals and functions, whatever the
hardware. That is half of a general machine runtime. The other half is
what a machine does with its signals before anything acts on them: filter
one, combine several, run them through a transfer function or a model, and
get a value that is itself a signal. Control platforms that last have this
as part of the platform (see Prior art). Here it is missing.

A signal today is what a provider reads from a device. Nothing between the
state cache and its consumers computes one value from another:

- The behavior tree's own nodes read a signal, check its quality, read a
  parameter, call a device, build call arguments, compare a boolean,
  schedule pulses and pass a value on when it changes
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
way they read a device signal. The computation is a graph of blocks: signals
in, signals out.

The rig above is where the idea is tested, not what justifies it. O0 below
shows that each of its needs could be met some other way.

## Evidence

`tools/derived-signals/` in this repository holds a paper exercise: nine
quantities from that rig written as plain code, run against its recorded
telemetry, then written in three declaration styles. Its README has the
cases, the method and the limits. In short:

- The reference detectors trip at the same tick, for the same reason, as
  `heater_guard.py` on six synthetic traces.
- Replayed over one overnight heater run, hourly relay duty is within 1.0
  point of the figures worked out on the rig at the time, at the one
  decimal `results.txt` gives them to (case 6). Those came from
  another script over the same telemetry, so this is two calculations
  agreeing, not a measurement.
- Replayed over one pH calibration, the steady detector turned true before
  the reading stepped at each of the two calibrations in the window:
  about 14 minutes before the first and under 2 minutes before the
  second (`results.txt`, case 5: steady at 10:09:42 and 11:37:40, moving
  again at 10:24:12 and 11:39:27). A tighter threshold would have missed
  the second.
- The steady-state relation gives 24.9 °C for the liquid against about 25 °C
  on a thermometer the next morning. Its coefficient was fitted on runs that
  include this one, so this checks the arithmetic, not the model.
- Of the nine, seven are needs and two are stress cases (an estimator and a
  total that outlives a restart). Nothing on the rig needs an estimator
  today.

The findings that bear on a decision are cited under it below as "exercise".

A second body of evidence is a spike: FluxGraph's version 2 core linked
into the runtime, closing a speed loop on the simulator's motor through a
behavior tree ([#835](https://github.com/anolishq/anolis/pull/835)). It is throwaway code, and its README has
each run and what it found. Its findings are cited below as "spike".

What the spike is not. But for three runs of signals that are read and
not acted on, which "How a graph's outputs are published" and the
README's last two sections give, every run is of one loop on one
simulated motor:
a first-order plant with no noise, no dead time and no load, stepped ten
times a second and watched over HTTP every 50 ms. The longest of those
is 26 s. Most findings are one run, and the text says where there were
more. The runs were made against several commits of the engine, which
the README names. A stall is the whole runtime stopped, and a hang is
the engine's thread alone stopped; no run stopped the polls or the tree
alone. A clock that steps is an offset added to the times the spike
reads. Nothing ran in `MANUAL` or with automation off. Telemetry was on in
two runs, and went to a stand-in that counts rows and stores nothing.
No hardware was used.

## Decided

### What the first version is for

Decided, after the second review: the first version publishes derived
signals for a person, telemetry, HTTP and the tree's decisions to read.
It does not close a loop. A graph's output handed to an actuator, as
the spike hands a duty to a motor, is a second stage, and is not
accepted with this RFC.

The spike's loop stays in this text as the evidence for that stage and
for what it needs first:

1. **A lifetime and a fail action for every command**, held by the
   runtime (P2).
2. **A check that the graph is alive**, made by whoever reads it (P6).
   Built in the spike.
3. **One read of a value and its quality together**, and no plausible
   number where there is no value (P4).
4. **A way to give a controller its setpoint and its tuning** while the
   runtime runs (O4).
5. **The graph told whether its command was delivered**, and not only
   whether the runtime means to deliver it (P2).
6. **A loop on hardware**, with noise, dead time and a sensor fault
   that does not recover, holding its bound.

### O0. Whether a new layer is needed

Decided: yes, as a capability of the platform. The reference rig does not
force it, and that was tested.

**The case against, worked through.** Each of the rig's seven needs was
placed where it would go without a new layer:

| Need | Where it could live | What that takes |
| --- | --- | --- |
| Heater trip | The heater board's firmware | Over-temperature, sensor-fault and no-rise checks exist there, and its command watchdog covers a lost controller. Their limits are build flags, and the provider cannot yet read or clear a fault. |
| Glitch removal | Firmware | Only the trip needed it. The firmware already waits for three readings in a row. |
| Thermocouple bias | The provider's config, as an offset for that board | The provider has no calibration setting today. |
| Relay duty | The provider | The firmware reports on-time and period for each cycle. The provider exposes the on-time and does not mark it for polling. |
| "Is pH steady" | The workbench | It keeps ten minutes of each reading and already works out its range. |
| Probe compensation | The behavior tree | Nothing. See below. |
| Pad and liquid relation | The runbook, or a calculator in the workbench | It is the only need that crosses providers. |

**The tree can already compute.** `tools/derived-signals/tree-probe/` runs
four trees in the released runtime (0.13.0) with a mock sensor, using only
the script nodes built into the BehaviorTree.CPP library:

- arithmetic on a live reading, and a condition on the result;
- values kept from one tick to the next;
- the whole compensation pattern: a quality gate, smoothing, a deadband, and
  a device call only when the held value changes. It made one call at
  start, none after the input moved by 0.3, and four as the smoothed value
  followed a move of 3.0.

Replayed over the recorded night, that smoothing and deadband send 8 values,
the same as the window version in the exercise.

**Why build it anyway.** The decision is about what the runtime is, not
about this rig:

- A general runtime cannot ask every machine to push its computation into
  firmware, a provider, a tree or a UI, each in a different language, none
  of them able to see the others' values.
- A value computed in a tree is not a signal: it reaches no telemetry, no
  operator panel and no other consumer, and it exists only in `AUTO`.
- A value that needs two providers' signals has nowhere to live.
- Filters, transfer functions and estimators are a body of theory with
  known building blocks. Offering those blocks once, with defined behaviour
  for bad input and for time, is the platform's job.

**What the test leaves behind: a rule for what goes where.**

- Protection that must hold in every mode, and when the runtime is down,
  belongs in the device: firmware, or failing that its provider. The layer
  is not a safety function.
- A correction to one device's own reading that every consumer wants, such
  as a calibration offset or a glitch at the source, belongs in that
  device's provider.
- Sequencing and decisions belong in the tree, which may compute for its own
  use with the scripting it already has.
- A value computed from signals that should itself be a signal belongs in
  this layer.
- A value only a person looks at, once, can stay in the workbench.

### O11. The engine

Decided: FluxGraph, with its core reworked as version 2. FluxGraph becomes
the signal-graph engine for the simulator and for this layer, so one model
can be a simulator's plant and an estimator's predictor.

The fit was measured first. On a branch of FluxGraph
([`spike/signal-blocks`](https://github.com/anolishq/fluxgraph/tree/spike/signal-blocks);
its `spike/FRICTION.md` has each figure here), with nothing in the
library changed, the ten block types of O3 were written as FluxGraph models
and run against test vectors from `tools/derived-signals/vectors.py`. Every
output matches at every tick, by state and by value, including 28,900
comparisons over the nine cases. What the 1.x model interface does not give
a block:

- A signal is one number, so each wire became two signals, a value and a
  state.
- A signal nobody has written reads as 0.
- The compiler does not order models and does not see their inputs. Two
  models that read each other's output compile and run, each a tick behind
  the other, with no error.
- Only a model can take two inputs, so every block was a model, and none of
  FluxGraph's own transforms could be reused.
- A model is not told the period when it is built, or the time when it runs.
- A model's spec has no input or output ports, only parameters.

Those land on the engine, the compiler, the graph spec, the signal store
and the two interfaces: 1,550 of the library's 6,095 lines. The models,
transforms, loaders and units keep their content and change how they read
and write.

**The design is now in two places.** What a value's state is, what a block
does with a bad input, the block catalogue, saving and restoring state, and
the sample clock are the engine's, and are in
[FluxGraph's version 2 semantics](https://github.com/anolishq/fluxgraph/blob/main/docs/semantics_spec_v2.md),
which FluxGraph has accepted as what its 2.0 is built to. This document
keeps what is Anolis's: how a derived signal gets an identity, where the
layer runs, where its definition and its constants live, and the safety
boundary.

### The engine is a part a build may leave out

Decided: the runtime does not depend on FluxGraph. A build has the layer
or has not, by a CMake option, `ANOLIS_ENABLE_DERIVED`, as
`ANOLIS_ENABLE_AUTOMATION` decides whether a build has behaviour trees.

- **Without it nothing of FluxGraph is fetched, compiled or linked.** The
  spike (#835) is built so: without the option `nm` finds no symbol
  naming FluxGraph in `anolis-runtime`, and with it 724. FluxGraph is
  named in two places, the layer's own
  directory, `core/derived/`, and the build file that links it. What the
  layer needs of the rest of the runtime names no engine: the state cache
  telling a listener of each read as it returns, and a provider that runs
  in the runtime's process.
- **A build without the layer refuses a config that asks for one**, as
  the config is loaded and before a provider is started, naming the
  option. The runtime warns of a top-level key it
  does not know and goes on, and that will not do here: a reading or a
  trip that someone believes is there, and is not, is the worst outcome
  this layer has.
- **A build should say which it is**, so that a client can show derived
  signals or not. Open: nothing says today whether a build has
  automation either, but its endpoints answering that it is not enabled.
- **The option is off unless given until the first version is released**,
  and on from then, as automation's is. Until then a build as it is made
  today fetches nothing new and behaves as it does today.

FluxGraph does not know of Anolis either: nothing in its version 2 core
names a host
([its account of that](https://github.com/anolishq/fluxgraph/blob/main/docs/hosts.md)).
Each is something the other can be built without.

### How a graph's outputs are published

Decided, on three runs of signals that are read and not acted on
(spike, findings 21 to 26): two simulators with a graph over a signal
of each and telemetry on, the same with the signals of one alone, and a
graph over a chamber that a second engine simulates.

- **The runtime's config says which device each output is a signal
  of.** A graph's outputs are not one device. The config groups them:

  ```yaml
  derived:
    id: derived0
    graph: chamber.yaml
    devices:
      chamber: [spread, smooth, settled]
      heater: [duty]
  ```

  An output of the graph that is in no device is refused when the
  config is loaded, and so is one in two, and a name the graph has no
  output of. The graph's file names no device: what a device is, is
  the runtime's to say, and the engine knows of no host. Spike, as one
  device: one output that was `FAULT` for 1.5 s made the device
  `FAULT`, the device being as bad as its worst signal, and a window
  that needs 8 s of samples left the device `STALE` for the first 8 s
  of the run. Not run: the grouping itself, which the spike has not
  got.
- **The layer's own counts are not signals.** How many calls it has
  made, how many ticks ran, how many writes were refused: these go to
  the log, and to the metrics the provider health answer already
  carries for each provider, a map of text to text that its schema
  leaves open. Spike: published as signals, they wrote 236 of the
  derived device's 469 telemetry rows in 60 s, more than its five
  derived signals did. As metrics they write no row, and no graph or
  tree can be given one as an input.
- **How often a number is published is the graph's to limit**, with a
  `deadband` block before the output. The layer publishes what the
  graph gave and holds nothing back, so that what is published is what
  the engine's rules say it is and is saved with the rest of the
  graph's state. Spike: a low-pass of a noisy signal wrote 117 rows
  against its source's 119, and 3 through a deadband of 0.01. A graph
  without one costs rows and is not wrong. The documentation has to
  say so where it says how to publish an output.
- **`uncertain` is published as `STALE`**, and there is no fifth
  quality (O1 has the table of what one would touch). A graph that has
  to tell an old value from a doubted one publishes that as a signal of
  its own. Spike: while a low-pass rode through 1.5 s without its
  input, HTTP gave its output as `STALE` and the health of its device
  went to `STALE` and back, and nothing a reader had told that from a
  provider's `STALE`. That is the cost, and it is taken. Not seen: the
  workbench, and a stop chain's view of the device.

### What stands for the first version

Decided: for a first version whose signals are read and not acted on,
what is under "Proposed" below stands as it is written, and four
things that were open are closed. What any of it says of a command, a
loop or a tree that acts is not decided by this, and waits for the
second stage.

- **P1, P3, P5 and P6**: a derived value is a signal; evaluation is
  periodic and in dependency order; declarations are blocks; and the
  layer runs in the runtime's process behind the provider interface.
  One placement was built and that is taken as enough for signals
  that are read (O10).
- **P2**, as far as that the layer produces signals and calls
  nothing.
- **Quality, going in and coming out** (O1): `OK` is `good`, `STALE`
  and `FAULT` are `bad`, `UNKNOWN` is `not_ready`, and the same
  backwards, with `uncertain` as `STALE`.
- **A sample's time is when the runtime's read of it returned** (O7).
  It is decided ahead of the measurements O7 says would settle it:
  latency and skew on the rig. Those are owed before the second stage,
  and until then they bear on which period and which maximum ages a
  machine's graph is given, and not on the rule.
- **A constant is a setting in the graph file, or an input the config
  gives as a constant** (O4), changed by editing a file and starting
  again.
- **An output with no value is published as the last value it had
  when it was usable**, with the quality that says it is not, and as 0
  or `false` only where it has had none (P4). The spike publishes 0
  always, and a reader that passes over quality then has a duty of 0
  or a difference of 0 that nothing measured. An old number that was
  true is the smaller harm. Not run.
- **Nothing is saved across a restart, and a block that would lose by
  that what it cannot get back is refused** (O5): `latch`, `integrate`
  and `pid`, whose integral is a running total. The refusal is the
  runtime's, when the config is loaded, and names the block. A
  hysteresis, a deadband, a timer and a rate limit also keep something
  with no end, and are not refused: each finds its place again from
  what it reads, and a timer that starts again errs by saying "not
  yet". This costs the first version two of the exercise's nine cases
  as `tools/derived-signals/signals.yaml` writes them: the heater
  trip, one of whose three detectors uses a `latch` to know that the
  relay has been on, and the energy total, which is a stress case. Taking the refusal away later stops
  no config that loads. Not run.
- **Nothing is raised when an output stays not `OK`.** The signal's
  quality and its device's health are the notice, as for a provider's
  signal. An event or an alarm with a time on it belongs with the
  second stage, where a command needs one ("What is missing").

## Proposed

These were the starting position. For the first version they are
decided ("What stands for the first version"); for anything that acts
they stand until an open decision contradicts them.

### P1. A derived value is a signal

It has the identity every signal has, `provider_id/device_id/signal_id`, and
a unit and a quality. Reusing the triple under a provider id of the
layer's own (O8) leaves
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

So a derived signal needs a registry entry that no provider process
published, or the handlers and the cache have to change. P6 proposes
the first: the layer is discovered as a provider is, once, at startup.

Prior art agrees on the direction: an EPICS calc record, an Ignition
expression tag and a Home Assistant helper entity are each peers of measured
values.

### P2. The layer produces signals and calls nothing

Every device call goes through the call router from an operator, the tree or
the safe state ([architecture.md](../architecture.md), "Key Invariants").
A derived signal can be the reason a call is made. It does not make one.

This moves a question; it does not answer it. Something else has to act on a
derived trip when the tree is not running. O6 owns that.

Spike: it can be done for a control loop, which is the second stage and
not the first version. The graph gave a duty as a signal, and a tree of
four stock nodes read it, built the call's arguments and called the
motor. What follows from the layer not being the caller is the rest of
this section.

- **The layer is told when nothing acts on its output.** With the motor's
  reads failing, the device-loss latch took the runtime to `FAULT` and the
  safe state set the duty to 0. The graph ran on, took its duty to the
  limit of 1 and published it as `OK`. When an operator re-armed the
  runtime the tree sent that 1 at once, and the speed went 10% over. The
  engine's controller now takes an input for this (its twelfth
  amendment): while it is true the controller gives back what the
  actuator reports and starts from there afterwards. The runtime writes
  it: true while the mode is not `AUTO`, or while the device is held by
  the device-loss latch. With it, the same run rose from rest and did not
  pass its setpoint. So the layer needs the mode and the latch as inputs,
  which no signal carries today.
- **And when the device's provider is lost.** A provider that dies is
  silent to the layer: the polls pause, no read returns, and the graph
  goes on giving its command as `OK` from the last sample until that is
  past its maximum age. With the simulator killed, the tree's third
  failed call took the runtime to `FAULT` in seven runs of eight. In the
  eighth the restart came first: the runtime stayed in `AUTO`, the new
  process had its motor at rest, the controller still held the integral
  of a loop at 1500 rpm, and the speed went 10% over. With a restart
  after 50 ms that happened in four runs of four. The input above is
  now true as well while the device's provider cannot be reached, and
  from then until a read of the device has returned. With that the
  controller followed the motor's report through the restart and the
  speed did not pass its setpoint, in four runs of four. So the layer
  needs a third thing from the runtime: whether a provider can be
  reached.
- **A command is acted on only when it is `OK`.** Proposed, as the
  default of the fail action above and not as the only behaviour. A
  value the graph gives as `uncertain` is for a person, a detector or a
  record to read, and is not sent to an actuator unless its command
  says so. Not sending is holding the last command, which is the safe
  way for some actuators and the dangerous way for others, a coolant
  valve that is shut among them; that is why it is the command's to
  declare. Nothing enforces it today: a tree of stock nodes can send a
  `STALE` value, and the spike has one that does. Spike, three single
  runs on one plant with one length of fault: a controller whose
  measurement rode through 1.5 s of a speed it could not read gave an
  `uncertain` duty. Sent, the duty went to its limit and the speed 62%
  over. Not sent, the motor kept its last duty, and the controller,
  which cannot know its command was not sent, wound up: 13% over.
  Telling the controller the duty the motor reports took the overshoot
  away and made the step take 7.7 s where it takes 3.2 s, in the one
  setting tried. In another run, of another graph with no ride-through
  and a fault of 3 s, the duty was `bad`, the controller waited, and
  nothing overshot. A ride-through on the measurement of a loop looks
  like a hazard, and the catalogue's documentation should say so.
- **"Nothing is acting" is a guess, and the graph should be told the
  fact.** The input above is worked out from the mode, the latch and
  whether the provider can be reached. The runtime thinks it is acting,
  and is not, when the tree is in another branch or a check before the
  call fails; when calls are failing and the third has not yet failed;
  when the tree waits behind a slow call to another device; when the
  device overrides the command itself, a drive in fault or a limit in
  firmware; when an operator calls the device in `AUTO`; when a
  restart fits between two calls of the engine; and when a call is
  clamped and still answers success. It thinks it is not acting, and
  something is, in `MANUAL` when a script reads the output and calls
  the device, and when automation is off. Proposed for the second
  stage: the call path records, for each command, whether its last
  value was delivered and when, and the graph is given that. A device
  that overrides its command needs its own report of what it applied,
  which is the device's to give.
- **How long a command stands is the runtime's to bound.** Decided,
  for the second stage. With the speed reading `FAULT` for 3 s the graph
  had no duty to give, the tree sent nothing, and the motor ran on at
  its last duty with nothing controlling it. With a sensor that does
  not come back that is a heater at its last duty until somebody
  notices, in `AUTO`, with nothing raised. An earlier revision gave the
  bound to the tree. The tree cannot hold it: it has no node that
  counts time without a good value and none that asks for `FAULT`, the
  spike's tree had no bound in any run, and each author would have to
  remember it. Nor is a provider's watchdog behind it: the polls go on
  while the tree is silent, and a device's watchdog counts a read as a
  frame. So each output a graph gives to an actuator declares, in the
  config, how long it may go without a good value and what happens
  then: the last value held for that long, then a stated safe value
  sent through the call router, then `FAULT`; and the runtime raises an
  event when it does. A config that routes an output to a function
  that actuates and declares none is refused. The engine gives an
  output no lifetime, and this RFC asks it for none.

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

Inside the layer "good" is the engine's `good` (O1). The rule holds only
inside the layer, and at the layer's way out it is broken twice.

- **A value there is none of leaves as a number.** The protocol cannot
  say "no value", so the spike publishes 0, or `false`, with a quality
  that is not `OK`. A consumer that reads the value and not the quality
  has a plausible number: a duty of 0, a valve shut, a trip that reads
  "not tripped", a 0 in the historian's average.
- **The tree checks and reads in two steps.** `CheckQuality` and
  `ReadSignal` each read the cache, and a poll can land between them:
  the check passes on the old value and the read gives the new one,
  whatever its quality. `ReadSignal` gives a number whatever the
  quality, and neither node looks at how old the value is.

Proposed: one tree node that reads a value, its quality and its age
from one snapshot and fails unless the quality is `OK` and the age
within a stated bound; and a derived boolean that stands for a trip
declares which way it reads when it is not good. Both are proposed
still, and the node is the tree's. What a number with no value is
published as is decided: the last usable value with its quality ("What
stands for the first version"). Not-a-number says it best and is not
valid JSON.

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
warns on an unknown key (`core/runtime/config.cpp`), though it refuses a
bad value. Refusing an unknown key or setting at load is new here.

### P6. The layer runs in the runtime, behind the provider interface

This was O10, and rests on one spike of one placement. The engine
is wrapped as an implementation of `IProviderHandle` in the runtime's own
process. It is given the graph's inputs from the state cache, and a
thread of its own calls the engine once a period. The graph's published outputs
are signals of one more provider, in the devices the config puts them
in ("How a graph's outputs are published"); in the spike they are all
of one device.

Spike: this is cheap to build, which is not the same as safe to run. At
`b03e050`, `git diff --numstat origin/main...spike/derived-signals-runtime
-- core` gives 663 lines added under `core/derived/`, 86 in the config
loader, 36 at startup and 14 in the state cache, which hand the layer
each read (O7). The registry, HTTP, change events and health reached
the outputs through the code they already have, and so did telemetry
in the two runs that had it on. A host slower than the graph's period was unremarkable in
two runs, on a plant whose time constant is eight periods: called
every 250 ms against a period of 100 ms the loop settled within 0.3 s
of when it does called on the period.

What it costs, and what is not settled:

- It bends the invariant that a provider is an isolated process
  ([architecture.md](../architecture.md)). The handle is not supervised:
  the main loop walks the configured providers.
- The outputs reach the cache up to one poll after the tick that made
  them.
- After a stall the engine's thread can run before the poll threads do,
  and reads what the cache held before the stall. With an input's maximum
  age near its sample interval the engine refuses the old sample and
  waits. With a maximum age of 5 s a controller started again from a speed
  3 s old and 750 rpm wrong. Handing the layer each read as it returns
  (O7) does not change this: the first call after a stall still runs
  before any read has returned. What makes a stall safe is the maximum
  age.
- **The computation shares a process with the stop logic**, and a
  graph is the least proven code on the machine. Three things that
  needs, of which the spike has two.
  - A graph that hangs. Spike: with the engine's thread stopped, every
    read of the layer went on giving the last duty as `OK`; the tree
    sent it 122 times, the runtime stayed in `AUTO` and health read
    well. A provider in a process of its own that hung would fail its
    reads and be latched as lost. In process nothing fails, so whoever
    answers a read has to judge it: the spike now gives every output as
    `FAULT` once no call has run a tick for three call intervals, and
    in the same run the duty read `FAULT` 0.14 s after the hang and the
    tree made no call after it. One run each.
  - A graph that throws. An exception on the engine's thread ended the
    runtime, the software stop and the latch with it. The spike now
    catches it and stops that thread, and the outputs age out as above.
    No run threw.
  - A graph that runs long. After a stall one call may run 100 ticks,
    on a thread with no budget, in the process that serves the software
    stop. Not measured: there is no figure here for how long a tick
    takes, or for how late one runs.

The other placement, in a provider, is not built. It keeps process
isolation and any language through an SDK, and needs a way to receive
other providers' signals, which ADPP does not have. Reading the runtime's
HTTP API back does not work at startup: providers start before the HTTP
server. Established systems tend to have both tiers: light calculations
beside the measured values, and heavy estimators as separate processes
that publish to the same place (ROS 2 `robot_localization`, the PX4
estimator). Nothing here rules that out later.

## Open

Ordered roughly by how much else waits on each.

### O1. Quality, and the "not ready" state

The engine's side is argued in [FluxGraph's version 2 proposal](https://github.com/anolishq/fluxgraph/pull/182), sections 3 and 7. A signal there has
one of four states: `good`; `uncertain`, a value that is usable and rests
on something substituted, held or doubtful; `not_ready` (a window still
filling, a filter not started); and `bad`. A block whose memory has no end,
such as a latch, a timer or a running total, reports by what is in front of
it and gives counters beside its value: how long it has run, how much of
that its input was not good, and how many gaps there were. What stays here
is how those four states and those counters meet the runtime's quality.

What exists:

- ADPP has `OK`, `STALE`, `FAULT` and `UNKNOWN`.
- HTTP and telemetry report `UNKNOWN` as `UNAVAILABLE`
  (`core/http/json.cpp`, `core/state/state_cache.cpp`), and both publish
  quality as a closed set of four names.
- The tree's nodes report it as `UNKNOWN`, and `CheckQuality` does not
  recognise `UNAVAILABLE` (`core/automation/bt_nodes.cpp`).
- "Not good" has at least four shapes in the cache: a quality that is not
  `OK`; no value at all; a value that is old; and a value whose provider is
  down. In the last case the cache marks the provider unavailable
  separately, and keeps the old values with their old quality or, after
  a read that failed, clears them (`core/state/state_cache.cpp`).

Questions, the first three answered below as far as they are:

- Going in: which of those four shapes the runtime writes to the engine as
  `bad`, which as `not_ready`, and whether any is `uncertain`.
- Coming out: do the engine's four states map onto the existing four
  names. `STALE` and `uncertain` are not the same thing: one says a
  value is old, the other that it is usable and doubtful.
- What a consumer does with `uncertain`.
- Still open: whether a `good` value is acted on without reading its
  counters. The proposal lists, in its section 7.11, each value it calls
  `good` that rests on something a block no longer remembers. And how a
  value that came from a fallback is told apart from one that was
  computed: in the engine both are `uncertain`.

Decided for the first version, on the spike:

- Going in: `OK` is `good`, `STALE` and `FAULT` are `bad`, `UNKNOWN` is
  `not_ready`, and a value the cache does not hold is `bad`.
- Coming out: the same backwards. Where the engine has no value the
  protocol still wants one: the last usable value, and 0 where there
  has been none ("What stands for the first version"). The spike
  gives 0 always.

Decided ("How a graph's outputs are published"): `uncertain` is
published as `STALE`. There is no fifth
quality: an earlier revision proposed one for later, and the table
below is why it is withdrawn. A graph that has to tell an old value
from a doubtful one publishes that as a signal of its own. A graph with
an `uncertain` output has been taken through HTTP and health in one
run, and not through the workbench or the stop chain's view of its
device. What it costs is known. An operator sees "stale" on a value
that is new at every tick, and learns to pass over the word. Good,
uncertain and bad are the words of OPC UA and of fieldbuses, and a
historian that has only "stale" cannot give them back.

Going in, `STALE` was first written as `uncertain`: a value its provider
had called too old, usable, with the time its read returned, which a
controller or an estimator takes for a new measurement. Spike, one run
each, of a speed that read `STALE` for 3 s during a step. Written as
`bad`, the controller waited and the speed did not pass its setpoint.
Written as `uncertain`, 4% over. Left unwritten, so that the input's
maximum age judges, the controller ran for a second on the last
sample, its duty `OK` and sent, and the speed went 76% over. So it is
`bad`: the provider says the value is too old and the runtime does not
know how old. The simulator gives no `STALE`; the run renamed a
`FAULT`, and the value beside it was the true speed.

A command is acted on only when it is `OK`, by default (P2). A derived
signal is not a stop chain's status (O6), so the stop chain never reads
an `uncertain` value.

Spike: a graph's `uncertain` output was published as `STALE`, and the
tree, gating on `OK`, did not send it (P2). The runtime judges age when
a value is read and does not write `STALE` into the cache, so but for
what a provider itself calls `STALE` the engine's own maximum age is
the only judge of an old sample.

#### Who makes a quality, and who reads one

Read from the code of the runtime, the protocol, both SDKs, the
providers, the exporter, the workbench and the operator UI. Nothing was
run for it.

There are three vocabularies. ADPP has `OK`, `STALE`, `FAULT` and
`UNKNOWN`, and `UNSPECIFIED` for a provider that set none. HTTP, the
event stream and telemetry have `OK`, `STALE`, `UNAVAILABLE` and
`FAULT`. The tree's nodes have `OK`, `STALE`, `FAULT`, `UNKNOWN` and
`UNSPECIFIED`, and no `UNAVAILABLE`, though `ReadSignal`'s port
description says otherwise.

Made by:

| Where | Which | When |
| --- | --- | --- |
| A provider, through an SDK | `OK` unless it says otherwise | The C++ SDK's `quality_from` gives `UNKNOWN` with no sample yet, `FAULT` after a failed read, `STALE` past the signal's `stale_after_ms`. The Python SDK turns an `OK` reading `STALE` on the same test |
| A provider, by itself | `FAULT`, `UNKNOWN` | An open thermocouple; a sensor before its first measurement |
| The state cache, at a write | whatever arrived | Kept as it is, a number it does not know among them. Nothing is mapped and nothing is refused |
| The state cache, as time passes | none | Age is judged by whoever reads, each with its own limit. An old value that arrived `OK` is still published `OK`, with its age beside it, and no event says it aged |
| The state cache, on a provider lost | none | The signals keep their last quality, or are cleared on a failed read; the device is marked unavailable beside them |

Read by:

| Where | With `STALE` | With a value it does not know |
| --- | --- | --- |
| HTTP, a signal (`core/http/json.cpp`) | `STALE` | `UNAVAILABLE` |
| HTTP, a device's own quality | the worst of its signals, by the number of the ADPP enum | a number above the enum's wins: the device reads `UNAVAILABLE`, over a `FAULT`. A signal with none set loses to `OK` |
| Event stream and telemetry rows (`core/state/state_cache.cpp`) | `STALE` | `UNAVAILABLE` |
| Tree, `ReadSignal` | gives the value, and `STALE` beside it | gives the value, and `UNSPECIFIED` |
| Tree, `CheckQuality` | fails unless `STALE` is what it expects | fails; and a name it does not know, given as what to expect, matches only a signal whose provider set no quality |
| Stop chain status (`core/control/stop_chain.cpp`) | not known: only `OK` is judged | not known |
| Stop chain, "does the device still answer" | does not answer | answers |
| Device health (`core/health/health_snapshot.cpp`) | the device is `STALE` | healthy, unless old |
| Exporter | passed through as text | passed through |
| Grafana device-health counts | counted | in none of the four counts |
| Workbench: a reading | noted "stale" | shown as good |
| Workbench: start requirements, sparkline | not met; a gap | not met; a gap |
| Operator UI: a signal's badge | `STALE` | `UNKNOWN` |
| Operator UI: a device's card | stale | ok |

The set of four names is closed in two published schemas, each
vendored by a consumer under a lock: the HTTP API's
(`schemas/http/runtime-http.openapi.v0.yaml`, three places) and the
telemetry row's (`schemas/telemetry/telemetry-timeseries.schema.v1.json`),
whose baseline says its qualities "are limited to" the four and that
"changing key semantics is breaking". A row may carry fields the schema
does not name.

So a fifth quality is the dear way to say `uncertain`. It is one
number in ADPP, a minor change there, and then both SDKs, five mappings
in the runtime, the device's worst-of, which ranks by the enum's
number, two closed schemas with their fixtures and baselines, two
vendored copies, both user interfaces and the dashboards. Where one of
those is missed the value is shown as unavailable or as good, which are
the two things `uncertain` is not. Published as `STALE`, an `uncertain`
value is already left alone by everything that asks for `OK`, which is
what P2 now requires of a command, and shown as doubtful by everything
that shows quality. What it loses is the difference between old and
doubtful, and a graph can publish that itself, as one more signal.

Three things follow for a derived signal whichever mapping stands. A
`not_ready` output, published `UNKNOWN`, makes its device read
`UNAVAILABLE` over HTTP, and `STALE` in health, while any output is
filling. One `uncertain` or `bad` output makes its device `STALE` or
`FAULT` in health, and every output that shares the device with it;
which outputs do is for the config to say ("How a graph's outputs are
published"), and `health.excluded_signals` is there for an output that
should not count.
And a tree cannot gate on `UNAVAILABLE` at all.

### O2. Bad input inside a stateful block, and three-valued booleans

Moved to the engine: section 7 of [FluxGraph's version 2 proposal](https://github.com/anolishq/fluxgraph/pull/182) has the rules and the rule table.

The evidence stays here. One bad pad reading in the middle of a 30-minute
hold moves the trip from 1902.5 s to 2805.0 s, because `heater_guard.py`
restarts its timer, and a detector that holds through it trips at 1902.5 s.
No declaration style made the author choose. With the pad reading bad, "pad
above 70" is neither true nor false, so a `trip` that ORs four detectors is
not good on every tick where a thermocouple is bad and nothing has tripped
yet.

### O3. The block catalogue and its schema

Moved to the engine: sections 4, 9 and 11 of [FluxGraph's version 2 proposal](https://github.com/anolishq/fluxgraph/pull/182).

`tools/derived-signals/engine.py` stays as the Python reference the first
test vectors are written from. Ten block types express all nine cases. It
is not an independent oracle:

- The review broke it 37 ways and the comparison with `reference.py` caught
  4. The recorded night has no bad sample, and both were written by one
  person on one sample-and-hold grid.
- `vectors.py` and the C++ runner on the FluxGraph branch compare by state,
  at every tick, on synthetic inputs that include bad and missing samples.
  That comparison caught sixteen of sixteen deliberate breakages, once two
  vectors were added. Its expected values are still this engine's own.

A replay over recorded data still earned its place: the exercise's first
draft of the compensation signal rounded to 0.5 °C and sent 111 values in
one night, where a deadband sent 8. Only the replay showed it.

### O4. A home for machine constants

Five of the nine cases read a number that is neither a signal nor an operator
setting: the thermocouple bias, the loss coefficient, the heater's wattage,
the filter tuning. Today they are in the runbook. `automation.parameters`
is the only place for a number, it is the operator's, and it exists only
when automation is enabled.

Decided for the first version, on no run: a constant is a setting of
a block in the graph file, or an input the config gives as a constant,
as the spike's setpoint is. It is changed by editing a file and
starting the runtime again. A derived signal does not read an operator
parameter: those exist only with automation enabled and are not kept
across a restart.

That is enough for a detector's threshold or a sensor's bias and not
for a controller. A loop whose setpoint and gains change only by a
restart is a process dropped to its safe state at every step of
commissioning, since a start drives the outputs safe, and people will
route round it. A setpoint and a tuning that can be written while the
runtime runs, taken up without a bump, are a condition of the second
stage.

### O5. State across a restart

The mechanism moved to the engine: section 8 of [FluxGraph's version 2 proposal](https://github.com/anolishq/fluxgraph/pull/182) gives every block a
way to save and restore its state.

Decided, on no run: the first version saves nothing. Every start of
the runtime is a start of the graph from nothing. A running total that
has to survive a restart is not served until saving is added, and the
engine's save and restore are there for that.

Saying so in the documentation is not enough. A latch, a total or a
timer loads and reads `good` after a restart that wiped what it held: a
latched over-temperature clears itself, and a dose total reads zero.
Decided: `latch`, `integrate` and `pid` are refused until saving
exists ("What stands for the first version"). Giving `uncertain` from
a start until something says the history is whole was the other
course, and is a rule of the engine's that it has not got. The engine's counters (`elapsed`, `gaps`) already say
how long a block has been going, for a consumer that reads them.

What is left for then: which state the runtime saves, where it keeps
it, which events reset it, and what a changed graph does with an old
file. A running total wants to survive a runtime restart and to reset
when a run starts. A window or a median wants neither. Parameters set
over HTTP are already not persisted ([automation.md](../automation.md),
"Runtime Parameters").

### O6. From a detector to an action, and the safety boundary

`heater_guard.py` ends in `POST /v0/estop`. A derived boolean alone does not
replace it: the tree has no node that requests a stop or `FAULT`, and does
not run in `MANUAL`, where the heater stays on.

Two things are already clear:

- **A derived signal must not be a stop chain's status.** Decided: a
  stop chain whose status names a derived signal is refused when the
  config is loaded. The stop chain
  observes a stop that has already happened. When its status reads in force
  or unknown, the runtime holds the latch and sends no safe-state calls
  (`core/runtime/runtime.cpp`, `core/control/stop_chain.cpp`). A derived over-temperature trip named
  there would latch the machine and leave the heater on, and one that is not
  ready at startup would hold on every boot. Nothing forbids it today: the
  stop chain accepts any cached boolean with quality `OK`
  (`core/control/stop_chain.cpp`).
- **Acting on a derived trip is a new cause of an autonomous stop.**
  [safety.md](../safety.md) lists the cases in which the runtime enters
  `FAULT` by itself. A derived trip would add one, and it needs a stated
  direction of failure: what happens when the trip is not good.

Decided: the runtime acting on a derived trip by itself, in every mode,
is not in this RFC. It is a new cause of an autonomous stop and gets an
RFC of its own, with its own account of what a trip that is not good
does. So the need this began with, a trip that works in `MANUAL`, is
not met by the first version, and that is said here and not left to be
found. Here a derived signal is read by the tree, an operator, telemetry
and HTTP, and the tree acts on it in `AUTO` as it acts on any signal.
`heater_guard.py` stays until that RFC lands: it stops in `MANUAL`, and
on a failed HTTP read, which nothing inside the runtime can observe.

O0's boundary rule stands beside this: protection that has to hold in
every mode belongs in the device.

### O7. Time

The engine's side is written, in sections 6, 7.2 and 8 of [FluxGraph's version 2 proposal](https://github.com/anolishq/fluxgraph/pull/182). A graph has one fixed period. The host passes the time to each
call, as whole nanoseconds, and the engine runs every tick that is due.
Every write carries the time its sample was taken; the write with the
latest time stands, and is not good past the input's maximum age. A call
says whether it was a gap: a stall too long to catch up, a restore, or the
host saying its clock stepped.

What the engine asks of its host, and what the runtime does today:

- **A clock that does not step, shared by every sample time and every
  call.** Signal timestamps are each provider's own wall clock, and
  freshness is judged against the runtime's
  ([#796](https://github.com/anolishq/anolis/issues/796)). A value stamped
  ahead of the runtime's clock goes stale late
  ([#793](https://github.com/anolishq/anolis/issues/793)).
- **Every sample written as it is taken, not only the latest.** The cache
  keeps one value per signal. A host that stalls and then hands the engine
  only what it holds has the ticks it missed run on that one value: in the
  proposal's own test of this, a latched trip and eight times the energy.
  The engine cannot see what it was never given.
- **A word when the clock steps or a state is restored**, so that the next
  call is a gap and not an hour of catch-up, **and how long the graph was
  away** where the runtime knows. Told, a restored total has the downtime
  in what it missed. Not told, the engine takes the gap as longer than
  anything it counts, and a total with a limit on what it may miss keeps
  its value and is `uncertain` until it is reset.

What stays here is how the runtime meets that: which clock it uses, when it
ticks the graph, what sample time it gives each cached value, and what the
period and the maximum ages should be.

Decided for the first version, on the spike: **a sample's time is the time the runtime's read
of it returned, on the runtime's monotonic clock**, and the engine is
called on that clock. Every sample time and every call are then one clock
by construction, and a step of the wall clock does not reach the engine.
It is decided ahead of the measurement this section says would settle
it, and it has faults of its own, listed after the spike's runs.

- Spike: the spike used the provider's timestamps, and a provider whose
  clock is not the runtime's stopped the loop. With every sample stamped
  2 s behind, each was past its maximum age of 1 s when it arrived. With
  every sample 2 s ahead, the engine threw each away, as its rule for a
  sample stamped far ahead says. Either way the output was `bad` from
  start to end, the motor never moved, and the only trace was the
  engine's count of writes thrown away.
- What it costs: a sample's time is late by as long as the read took,
  and a provider's own, more exact timestamp is not used. The provider's
  timestamp stays what telemetry and HTTP show.
- Spike: built this way, with the state cache telling the layer of each
  read as it returns, the loop did what it had on the provider's times
  in each of five runs repeated, and every read reaches the engine and
  not only the latest. It took 14 lines in the state cache.
- Spike, on the provider's times, with a step added to the times the
  layer reads: the calls and the sample times stepped together by 30 s,
  as one machine's wall clock stepping, did not move the loop. Forward
  the engine saw a gap of 300 ticks; back it refused one call, was
  rebased, and gave no output for one poll. The sample times stepped
  alone by 2 s, as a provider's clock stepping by itself, stopped the
  loop for good, either way: every sample from then on was thrown away
  or too old, the output was `bad`, the runtime stayed in `AUTO`, and
  the motor ran on at its last duty. On arrival times there is no such
  clock to step. No real clock was stepped.

What a time of arrival gets wrong, none of it run:

- **A reading that has stopped changing.** A provider whose sensor
  thread has died, or a device that answers with its last conversion,
  gives the same value at every poll, and each has a new time of
  arrival. The maximum age never fires. The provider's own timestamp
  was the evidence, and it is not read.
- **A maximum age is how long a graph runs blind.** With samples that
  stop and say nothing, the last one stands until its age runs out:
  the run above that went 76% over (O1), and a provider that dies
  (P2). An age of 1 s is ten samples of the spike's loop.
- **A suspend.** The monotonic clock on Linux does not run while the
  machine is suspended. After a resume the sample from before it is as
  young as it was, no gap is reported, and a controller goes on from a
  measurement that may be an hour old. The spike's stall stops the
  process, during which that clock runs, and could not show this.
- **Latency is taken for time.** A time of arrival is the sample's time
  and the transport's and the wait behind a call. A derivative and an
  estimator take their step from sample times, so jitter in the read is
  noise in both.
- **A graph of two devices.** The engine asks that the inputs of one
  scan have one time. Each provider is polled on a thread of its own
  and each device is stamped as its own read returns, so there is no
  scan. The need O0 says only this layer serves, a value of two
  providers' signals, has no run.
- **The sample rate follows the commands.** A loop that commands reads
  its device at the tree's rate. When the tree stops sending, the rate
  falls to the poll interval, 500 ms unless given, against a maximum
  age of 1 s in the spike: two samples of margin when the loop is
  trying to come back.

Open, and what would settle each: whether the provider's timestamp is
kept beside the time of arrival, to find a sample that has not moved,
which needs no change to ADPP; a clock that counts a suspend, or a
rebase on resume; a run of an input that is truly old and called
`STALE`; a run of a graph of two providers; and distributions of read latency and of skew between
devices, measured on the rig.

Two more things the spike measured:

- The poll interval is not a loop's sample rate. The call router polls a
  device at once after every call to it, so a loop that commands at the
  tree's 10 Hz reads the motor at 10 Hz whatever `polling.interval_ms`
  says. A graph that only watches sees the poll interval.
- A slow call is dead time twice. With every call to the motor taking
  300 ms the speed went 25% over. The tree waits for each call, and the
  provider answers one request at a time, so the poll of the motor waits
  behind the call.

- Polling is one thread for each provider, reading its devices one
  after another (one thread in all, when this was first written), and a
  successful call re-reads its device from the caller's thread
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

- A transfer function, a filter or an estimator is defined for a sample
  period. Its inputs arrive by polling, at a nominal period with jitter,
  each device at its own instant. The layer therefore needs a sample clock
  of its own and a stated rule for bringing each input onto it.

Question: what period does the layer run at and on which thread, how is an
input brought onto that period, how old may an input be, and what would
telemetry need to record for a definition to replay faithfully?

Settled by: measuring input age and cross-device skew on recorded data.

### O8. Identity and the contracts touched

P1 reuses the triple. Still open:

- Decided: no provider id is reserved. The config names the id the
  derived signals appear under, `derived0` unless given, and a config
  in which that is also a provider's id is refused when it is loaded.
  No config that loads today stops loading, and a second graph later
  needs no new rule. Whether a signal is derived is for its registry
  entry to say, and not its name. No config in the project's own
  repositories uses an id beginning `derived`
  (a search of their YAML, JSON and Markdown for `id: derived` and
  `provider_id: derived`); a config held elsewhere was not seen.
- What the middle segment means when there is no device. The exercise
  invented `heater`, `ph` and `probes`. Decided so far: it is a name the
  config gives to a group of outputs ("How a graph's outputs are
  published"). Whether such a group may be named for a real device of
  another provider, and what that would claim, is open.

Answered below, from reading the code: how a derived signal gets a
registry entry, what the surfaces that list providers show, and whether
a number that changes at every call floods events and telemetry.

#### What the layer touches

Read from the code and the documents; nothing was run for it. The rule
that decides a bump is [dependencies.md](../dependencies.md): before
1.0, "a change that makes an existing config, install or client behave
differently (a changed default, a refused request, a new failure) is
breaking and bumps MINOR", and "Anything else bumps PATCH, new features
and fields included".

Has to change, all of it within that second sentence so long as the
layer exists only where a config has a `derived:` section:

| What | Today | Needs |
| --- | --- | --- |
| The config loader (`core/runtime/config.cpp`) | An unknown top-level key is warned of and ignored, so an older runtime given `derived:` runs with no derived signals and says little | The section read; the id refused where it is a provider's; the upgrade notes naming the first runtime that reads it |
| The runtime config schema, its baseline and fixtures | The root allows any key, so `derived:` passes unchecked there and in the workbench, which validates against a vendored copy | The section in the schema; a valid fixture and one refused for the id; the workbench's lock moved to that release |
| Provider startup (`core/runtime/runtime.cpp`) | Walks the configured providers | One step more, after them; the handle is not given to the supervisor |
| Key invariants 2 and 5 ([architecture.md](../architecture.md)) and every place a provider is called a process | "Registry changes only on provider restart"; "Providers are isolated processes" | Rewording. The handle is discovered once and never restarted, which keeps the first. The second is not true of it: a fault in a graph is a fault in the runtime |
| A source provider restarted with other devices | The registry entry of the provider is replaced whole | The graph's inputs checked again against it, as declared calls are |
| Stop chain config | Any cached boolean that is `OK` may be a stop chain's status | The refusal O6 decides |
| The poll plan | A signal with no poll hint is not polled, and is in no state answer | Every output given a hint near the call rate. Spike: done so |

Needs nothing, with the reason:

- **ADPP.** The handle fills the protocol's messages in process and
  nothing new is on the wire.
- **The telemetry row schema and the exporter.** The four tags are as
  they were. A deployment that allows providers by name leaves the
  derived id out until it is named.
- **The machine profile.** Its validator compares its providers with
  the configured ones, which the derived id is not among. A reading or
  a requirement may already name any `provider/device/signal`.
- **Ownership, the actuation gates and the safe state.** Each goes by
  a device's functions or its claim tag, and the device has neither.
  A call to it finds no such function.
- **The provider health endpoint and its telemetry.** They go by
  registered handles, so the layer is listed, as running, with no
  supervision. Its uptime reads 0, which is the one value that
  misleads. A new lifecycle state for it would change a closed
  enumeration and is not proposed.
- **The supervision loop.** It walks the configured providers and
  passes the handle by, so nothing restarts it and nothing notices that
  it has stopped. A read it failed would be taken for a lost device and
  latch it, which helps nobody. So it answers every read, and says that
  its graph has stopped by the quality of each output (P6).

Costs that are not contracts:

- **Every change of a value is an event and a telemetry row.** The
  cache compares doubles bit for bit and has no deadband. An output
  that moves in its last place at every call writes a row at every
  poll that follows one: 864,000 a day at 100 ms, for one signal. So
  an output that is published needs a deadband in the graph, which the
  catalogue has, and the documentation has to say so: decided ("How a
  graph's outputs are published"). A deadband in
  the cache would change every existing signal.
- **A signal named `estop`** on the derived device would be read as
  its e-stop by a stop chain that lists the device. Refused with the
  rest of O6.
- **Outputs that share a device** make it as bad as the worst of
  them, in health and over HTTP, which is why the config groups them.

Not found: a rule for when the HTTP API's own version changes, and any
reserved provider, device or signal id.

### O9. Where the definition lives

Decided: in a file of its own, in the engine's format, that the runtime
config names, as it names the behavior tree. The engine's compiler then
refuses a bad definition when it is loaded, which is P5's refusal, and
the engine's conformance vectors are of the same format a machine runs.
The runtime's config says only which file, how often it is called and
where each input comes from. Spike: built so.

### O10. Where the layer runs

Decided for signals that are read, as P6: in the runtime, behind the provider interface. One of the
two spikes this asked for is built, and the other, in a provider, is not;
keeping changes to ADPP out of scope is part of why, since that
placement needs one. One spike is taken as enough for signals that are
read. It is not taken as enough for commands. What is open is under
P6: what a graph may cost the process it shares, and the race with the
poll after a stall, which handing the layer each read did not remove.

## What is missing

What a reviewer of a control system asks for first, and is not here.

**What each failure does.** For a derived signal that is only read, in
the first version:

| The output is | A reader sees | Who notices | Bounded by |
| --- | --- | --- | --- |
| `bad` | `FAULT`, and the last usable value, or 0 or `false` where there was none | Health, for the device as a whole | Nothing: it stands until the input comes back |
| `uncertain` | `STALE`, with a value | Health; a consumer that asks for `OK` | Nothing |
| `not_ready` | `UNAVAILABLE`, and the same | The same | The window or the filter filling |
| late: the graph called late | The last value, with its quality, for as long | Nobody, short of three call intervals | The check that the graph is alive (P6) |
| stopped: the graph hung or threw | `FAULT` after three call intervals | Health | The same |
| wrong, and `good` | `OK` | Nobody | Nothing. An estimator's `good` says its own tests passed |
| absent: no `derived:` section read, on an older runtime | No such signal | A consumer that names it | Nothing: a warning in the log |

For an output handed to an actuator, in the second stage, the last
column is the lifetime and the fail action P2 decides, and until those
exist the answer in every row is that the actuator keeps its last
command.

**Raising it.** In two findings of the spike a loop stopped for good,
in `AUTO`, with the motor at its last duty, and the only trace was a
count. Nothing here says what event, alarm or change of mode follows an
output that has not been `OK` for a stated time. P2 requires one for a
command; a signal that is read has none, and for the first version
that is decided: its quality and its device's health are the notice.

**Which graph ran.** Nothing in telemetry or in the runtime's status
names the graph file a value came from, by hash or by version, so a
recorded run cannot be tied to its definition.

**Starting.** The first output is not usable: the engine is called
before the first poll. Nothing says whether `AUTO` may be entered
before a graph is ready.

**Timing.** No figure for how long a tick takes, how late one runs, or
how old an input is when it is used, on Linux or on Windows.

**Hardware.** Every figure of the spike is from a simulator (Evidence).

**Modes of a loop.** Manual, automatic and cascade for each loop, an
operator's output in manual, a setpoint that ramps: none is here, and
each belongs to the second stage.

**A class of machine.** What the layer may and may not be used on, in
the terms of [safety.md](../safety.md). The layer is not a safety
function. A control function whose failure moves an actuator still
starts events, and the table above is the beginning of saying which.

## Rejected

- **Computing in the behavior tree only.** The tree runs only in `AUTO`, and
  its blackboard is visible to nothing. The tree may still compute for its
  own use, with the scripting it already links (O0).
- **Computing in the historian only.** InfluxDB tasks or Grafana queries can
  show a derived value. Nothing on the machine can act on it, and the
  definition would live outside the machine's configuration.

Computing in a device's own provider or firmware is not rejected. O0 says
what belongs there.

## Not in scope

- Any particular estimator or model. Transfer functions, filters and
  estimators are what the layer is for; which ones ship first is the
  catalogue's question (O3).
- Any change to ADPP. O1 proposed one, a fifth quality, and withdrew it.
- Workbench surfaces.
- The runtime stopping the machine on a derived trip by itself (O6). It
  gets its own RFC.

## How this proceeds

1. O0 and O11 are decided, and the choices the status line names.
2. The engine's semantics are argued in FluxGraph's proposal, and its
   conformance vectors are written there. Its third revision was amended
   a section at a time, thirteen times, and its fourth has those
   amendments worked into its text. Time and the host interface, how doubt passes through a block's
   memory, what a gap does to each block, how models are stepped, and the
   estimator are written. It keeps a list of what is known to be wrong
   as it stands: at this writing a derivative of a low-pass, an input
   true for one
   call that a fast host can miss, that a value does not say why it is
   not good, and that an output has no lifetime (P2 says whose that
   is). A model is linear and stepped exactly, and one too
   fast for the graph's period is refused, which bears on O7's question
   of what the period should be. An estimator's `good` means that none of
   its own tests has failed, not that it is right: whatever acts
   on one (O6) has to allow for a sensor that is biased or stuck,
   which no test of its sees. The whole text has since been read cold
   and amended for it, and its host contract is checked on every short
   sequence of writes and calls. Three things in that bear on a host here
   (O7): after a restore or a stepped clock it writes again only the
   inputs that have no maximum age, an input's initial value does not
   come back, and it gives the inputs of one scan one time. Its
   catalogue now has a rate limit and a PID. With an input it cannot use
   a PID gives no value, so what an actuator gets then is this runtime's
   to say (O6), and it winds up behind a limit unless it is told what
   was applied. Five amendments since bear on a host. A graph lists the
   outputs its host hands to an actuator, and a controller that feeds
   one acts once a call and not once for every tick a late call catches
   up: in the spike, with samples that stayed usable through a 3 s
   stall, the graph that listed no command overshot by 26% and the one
   that did, not at all. A controller's derivative and a window's slope
   are over the times the samples carry. A total that has missed more
   than it may keeps its value, as `uncertain`. A controller takes
   `follow` (P2). And an estimator works between samples: its state
   stands where its last measurement was, each measurement is met at its
   own sample time, and one older than the state is passed by and
   counted. Spike: an estimator of the motor's speed, with the engine
   called 30% slower and faster than its period, was not OK in 66 and 38
   of some 370 samples under the rule as it was and in 32 and 0 under
   the amended one. It asks of a host what O7 proposes: a sample time
   that is when the sample was taken, on the clock the engine is called
   on. The simulator steps its motor ten times a second, so a read
   between two steps is up to 100 ms older than the time it returned,
   and nothing the runtime has says so: a provider that knows when its
   sample was taken has no way to tell the runtime on the runtime's
   clock.
3. The remaining open decisions are worked one at a time, each by the method
   named under it, and each lands here as a change to this file.
4. O10 is decided as P6 for signals that are read, on one spike. The fourth revision was reviewed
   independently, by a reading for control and safety and by a check of
   each claim against its source; this revision is the answer to both.
   What they asked for and is not done is under "What is missing".
5. It is accepted for the first version, which becomes an epic with
   increments. The second stage comes back here first.

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
