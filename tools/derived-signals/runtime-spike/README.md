# The engine in the runtime: a spike

Throwaway. It answers part of O10 of the
[derived-signals RFC](https://github.com/anolishq/anolis/pull/790): what it
costs to host the engine in the runtime's own process, and what a real
host does to a graph. It is not a design and is not for merging.

## What it is

`core/derived/` wraps the version 2 core of FluxGraph
([proposal](https://github.com/anolishq/fluxgraph/pull/182),
[core](https://github.com/anolishq/fluxgraph/pull/185)) behind
`IProviderHandle`, in process. The state cache tells it of each read of a
device as it returns; a thread of its own writes those samples to the
engine and calls it once a period. The
graph's published outputs are the signals of one device, `graph`, of one
more provider, so the registry, the poll thread, HTTP, telemetry and
health reach them through the code they already have. It calls nothing
(P2): `tree.xml` reads the graph's `duty` and calls the device.

A config names the graph and where each input comes from:

```yaml
derived:
  id: derived0
  graph: motor-pid.yaml
  inputs:
    setpoint: {constant: 1500}
    speed: {device_handle: sim0/motorctl0, signal_id: motor1_speed}
    applied: {device_handle: sim0/motorctl0, signal_id: motor1_duty}
```

`motor-pid.yaml` is a PI speed loop on the simulator's motor (first order,
3000 rpm at a duty of 1, time constant 0.8 s) with `duty` listed as a
command and `track` fed from the duty the motor reports.

## Running it

```bash
python3 tools/derived-signals/runtime-spike/run.py \
  --runtime build/ci-linux-release/core/anolis-runtime \
  --provider ../anolis-provider-sim/build/ci-linux-release/anolis-provider-sim \
  --out <an empty directory>
```

The spike is in a build that asks for it, `-DANOLIS_ENABLE_DERIVED=ON`,
and in no other (finding 20). The presets `ci-linux-release-derived` and
`ci-windows-release-derived` ask.

The build pins FluxGraph by commit, `f0282af` of its `main`, which has
the version 2 core that this was first built against on a spike branch.
`-DFETCHCONTENT_SOURCE_DIR_FLUXGRAPH=<clone>` builds against a local
clone, which is how every figure here was made: the runs of
the estimator against the commits named with them, those on arrival
times and with `--rearm-at`, `--signal-fault-s` or `--call-latency-ms`
against `10fff0f`, those with `--call-ms`,
`--skew-ms`, `--max-age` or `--graph-line` against the one before it,
`c3ec3a5`, and the other six against the working tree that became that.
The runs under "A restart, a doubted command and a clock that steps"
are against `c794977`, and those of finding 20 against `f0282af`.

## What it did

Each row is one run of `run.py` with the flags given, a step from rest to
1500 rpm, sampled over HTTP every 50 ms. No run had a refused call or a
refused write.

| Flags | Within 5% for good from | Lowest and highest after the disturbance |
| --- | --- | --- |
| none | 3.2 s | n/a |
| `--poll-ms 500` | 3.1 s | n/a |
| `--call-ms 130` | 3.2 s | n/a |
| `--call-ms 250` | 3.4 s | n/a |
| `--stall-s 3 --seconds 25` | 3.1 s | 1500, 1500 rpm |
| `--unreachable-s 3 --at 8` | never again | 0 rpm, and stays there |
| `--unreachable-s 3 --at 8 --rearm-at 14 --seconds 26` | 15.9 s | 31, 1656 rpm (+10%) |
| the same with `--graph motor-pid-follow.yaml` | 17.2 s | 28, 1500 rpm |
| `--graph motor-pid-follow.yaml --signal-fault-s 3 --at 1` | 4.4 s | 645, 1500 rpm |
| `--graph motor-pid-follow.yaml --call-latency-ms 300 --at 0.2` | 16.3 s | 29, 1871 rpm (+25%) |
| `--skew-ms 2000 --seconds 6` | never | the motor does not start |
| `--skew-ms -2000 --seconds 6` | never | the motor does not start |

A stall of 3 s one second into the step, `--stall-s 3 --at 1`, with what
else was given:

| Also | Within 5% for good from | Lowest and highest after it |
| --- | --- | --- |
| nothing | 4.5 s | 1402, 1500 rpm |
| `--graph motor-pid-no-commands.yaml` | 4.8 s | 1389, 1500 rpm |
| `--max-age 5s` | 6.5 s | 1216, 1500 rpm |
| `--max-age 5s --graph-line "act_for: 30"` | 5.4 s | 1389, 1771 rpm (+18%) |
| `--max-age 5s --graph motor-pid-no-commands.yaml` | 5.4 s | 1389, 1893 rpm (+26%) |
| `--max-age 5s --graph-line "max_catch_up: 3"` | 6.7 s | 1165, 1500 rpm |

Every run above took a sample's time from the provider, which is
`--sample-time provider` now. The default is `arrival`: the state cache
tells the spike of each read as it returns, the sample's time is that
moment on the runtime's monotonic clock, and the engine is called on
that clock. With `--graph motor-pid-follow.yaml` and samples on arrival:

| Flags | Within 5% for good from | Lowest and highest after the disturbance |
| --- | --- | --- |
| none more | 3.2 s | n/a |
| `--call-ms 250` | 3.4 s | n/a |
| `--stall-s 3 --at 1` | 4.9 s | 1373, 1500 rpm |
| `--stall-s 3 --at 1 --max-age 5s` | 6.6 s | 1191, 1500 rpm |
| `--unreachable-s 3 --at 8 --rearm-at 14 --seconds 26` | 17.2 s | 28, 1500 rpm |

`motor-estimator.yaml` adds an estimator of the motor's speed: the
simulator's own model, fed the duty the motor reports and the speed it
measures. One run each of 20 s, with the engine before and after its
thirteenth amendment (FluxGraph at `2fda562` and at `ea5ede7`); the
first three take `--without ", late_z: est_late"`, an output the
earlier engine has not got.

| Flags | Estimator not OK, of about 370 samples from 1 s | Measurements refused | Furthest from the speed |
| --- | --- | --- | --- |
| before, none more | 0 | 0 | 94 rpm |
| before, `--call-ms 130` | 66 | 11 | 178 rpm |
| before, `--call-ms 70` | 38 | 4 | 124 rpm |
| after, none more | 0 | 0 | 18 rpm |
| after, `--call-ms 130` | 32 | 7 | 155 rpm |
| after, `--call-ms 70` | 0 | 1 | 63 rpm |

### A restart, a doubted command and a clock that steps

All with `--graph motor-pid-follow.yaml` but where another graph is
named, and samples timed on arrival but where `--sample-time provider`
is given.

`--kill-provider` kills the simulator 8 s in. The runtime restarts it
within 0.3 s, and the new process has its motor at rest.

| Flags | What the runtime did | Speed after it |
| --- | --- | --- |
| `--kill-provider --at 8`, seven runs | FAULT 0.31 to 0.35 s after, on the tree's third failed call | 0 rpm, and stays there |
| `--kill-provider --at 8 --rearm-at 14 --seconds 26`, the run where the restart came before the third call | stayed in AUTO | from rest to 1650 rpm (+10%), within 5% from 10.6 s |
| the same with the default graph, which has no `follow` | FAULT at 8.31 s, AUTO again at 14 s | from rest to 1621 rpm (+8%) at the re-arm |
| `--kill-provider --backoff-ms 50 --at 8 --seconds 16 --not-idle-on-loss`, four runs | stayed in AUTO in each | from rest to 1650 rpm (+10%) in each, within 5% from 10.5 or 10.6 s |
| `--kill-provider --backoff-ms 50 --at 8 --seconds 16`, four runs | stayed in AUTO in each | from rest to 1500 rpm and not past it, within 5% from 11.6 or 11.7 s |

The first three rows were run before `idle` followed the provider's
loss, which is what `--not-idle-on-loss` leaves out; the tables above
it are not run again.

`motor-pid-ride.yaml` takes the speed through a low-pass that rides
through 2 s, so that with the speed reading FAULT for 1.5 s during the
step the controller runs on the last speed it had and its `duty` is
`uncertain`, STALE to the runtime. Each with
`--signal-fault-s 1.5 --at 1 --seconds 12`:

| Also | The duty while STALE | Highest speed | Within 5% for good from |
| --- | --- | --- | --- |
| nothing | not sent | 1701 rpm (+13%) | 4.6 s |
| `--tree tree-stale.xml`, which sends a STALE duty too | sent | 2429 rpm (+62%) | 4.5 s |
| `--graph motor-pid-ride-applied.yaml`: the PID's `applied` wired to the duty the motor reports, `applied_within: 0.02` | not sent | 1499 rpm | 7.7 s |

`--clock-step-ms` and `--sample-step-ms` move the clock the spike
reads, 8 s in, with the loop at 1500 rpm: the first the calls and the
sample times together, as the wall clock of one machine stepping; the
second the sample times alone, as a provider's clock stepping by
itself. Each with `--sample-time provider --at 8`:

| Flags | What the engine did | The loop |
| --- | --- | --- |
| `--clock-step-ms 30000` | one gap of 300 ticks | 1500 rpm throughout |
| `--clock-step-ms -30000` | refused one call; rebased; a gap of no known length; 4 writes thrown away; `duty` not ready for one poll | 1500 rpm throughout |
| `--sample-step-ms -300` | threw 4 writes away | 1500 rpm throughout |
| `--sample-step-ms 2000` | threw every sample away, 242 by the end | `duty` FAULT from 0.84 s after and for good; the motor kept its last duty |
| `--sample-step-ms -2000` | threw 38 away, and then every sample was past its maximum age | the same |

`--hang` stops the engine's thread from calling it, 1 s into the step,
and nothing else: the runtime, the polls and the tree go on. Each with
`--hang --at 1 --seconds 12`:

| Also | The duty read | Calls the tree made | The motor |
| --- | --- | --- | --- |
| `--alive-within 0`, no check | 0.449, `OK`, to the end | 122, all of the one duty | at that duty, 1348 rpm |
| nothing: an output is `FAULT` once no call has run a tick for 3 call intervals | `FAULT` from 0.14 s after | 11, none after | at that duty, 1348 rpm |

A speed that reads STALE for 3 s, 1 s into the step. The simulator
gives no STALE, so `--fault-reads-stale` has the spike take the FAULT it
can be made to give as one. Each with
`--signal-fault-s 3 --at 1 --fault-reads-stale --seconds 12`:

| `--stale-input` | What the graph is given | The duty | Highest speed | Within 5% for good from |
| --- | --- | --- | --- | --- |
| `bad`, the default | no value | `FAULT` from 0.24 s after, for the 3 s | 1500 rpm | 4.2 s |
| `uncertain` | the value, as new and doubtful | `STALE` for the 3 s, and not sent | 1562 rpm (+4%) | 4.4 s |
| `skip` | nothing; the last sample stands until its maximum age | `OK` for 1.1 s more, and sent; then `FAULT` | 2636 rpm (+76%) | 7.1 s |

## What it found

1. **The cost of this placement is small.** About 520 lines in
   `core/derived/`, 50 in the config loader, 14 in
   `Runtime::init_providers` and a target in CMake. The registry, HTTP,
   change events and health needed no change. It bends the invariant that
   a provider is an isolated process, and the handle is not supervised:
   the main loop walks `config_.providers`.
2. **A provider whose clock is not the runtime's stops the loop, and
   says little.** With every sample stamped 2 s behind, each was past the
   input's maximum age of 1 s when it arrived. With every sample 2 s
   ahead, the engine threw each away, as its rule for a sample stamped
   far ahead says. Either way the
   graph's output was `bad` from start to end and the motor never moved.
   Sample times are each provider's own wall clock (#796), so this is
   what a provider on another machine with a clock 2 s out would do. The
   only trace is the engine's count of writes thrown away: 120 in the
   6 s ahead, and 40 behind.
3. **The graph has to be told when nothing acts on it.** With the
   motor's reads failing, the graph's output went `bad`, the tree sent
   nothing, the device-loss latch took the runtime to FAULT and the safe
   state set the duty to 0. That is the runtime doing its job. The graph
   then ran on with the motor at rest, raised `duty` to its limit of 1
   and published it as OK, for nobody; and when an operator re-armed the
   runtime, MANUAL and back to AUTO, the tree sent that 1 at once and the
   speed went to 1656 rpm, 10% over. FluxGraph's proposal was amended
   for it: a controller takes `follow`. `motor-pid-follow.yaml` wires it
   to an input the spike writes, true while the runtime is not in AUTO or
   holds the motor in its device-loss latch. With that the graph's `duty`
   read 0 through the fault, as the motor's did, and from the re-arm the
   speed rose from rest to 1500 rpm and did not pass it.
4. **After a stall the graph can run before the poll does.** The engine's
   thread and the poll threads all wake together. In the runs with
   `--max-age 5s` the first call after the stall read the speed from
   before it, 3 s old and still within its maximum age, and the
   controller started again from that: bumpless against a measurement
   that was wrong by 750 rpm, and a dip to 1165 rpm when the true one
   arrived a tick later. With the maximum age at 1 s the same stall cost
   nothing: the old sample was refused, and the controller waited for a
   new one. A maximum age near the sample interval is what makes a stall
   safe, and a placement that reads the cache has no way to ask for a
   poll first.
5. **Listing the command is what stops the windup.** With samples that
   stay usable through the stall, the graph that lists no command ran its
   controller on all 30 ticks it caught up and overshot by 26%; told to
   act for 30 periods it overshot by 18%; acting for the default of 3 it
   did not overshoot.
6. **The poll interval is not the loop's sample rate.** The call router
   polls a device at once after every call to it, so a loop that commands
   at the tree's 10 Hz reads the motor at 10 Hz whatever
   `polling.interval_ms` says. A graph that only watches would see 2 Hz at
   the default.
7. **A host slower than the graph's period is unremarkable.** Called
   every 130 ms and every 250 ms against a period of 100 ms, the engine
   ran 2 and 3 ticks in a call and the loop settled within 0.3 s of when
   it does called on the period.
8. **Quality had to be mapped both ways, and the mapping decides what the
   tree sends.** In: OK is `good`, STALE `uncertain` (`bad`, since
   finding 19), FAULT `bad`, UNKNOWN
   `not_ready`, and a value the cache does not hold is `bad`. Out: the
   same backwards, with a value of 0 where the engine has none, since the
   protocol cannot say "no value". The tree gates on OK, so an
   `uncertain` command is not sent. That is O1, guessed at. No run made
   a sample STALE: the simulator never reports one, and the runtime
   judges age when a value is read and does not write STALE into the
   cache, so in these runs the engine's own maximum age was the only
   judge of an old sample.
9. **While the graph has no command the motor keeps the last one.**
   With the speed reading FAULT for 3 s during the step, the graph's
   `duty` was `bad`, the tree sent nothing, and the motor ran on at the
   duty it had, 0.48, from 890 to 1420 rpm with nothing controlling it.
   When the speed came back the controller started from that duty and
   the loop settled with no dip. Nothing in the runtime or the graph
   says for how long a command may stand: a provider's watchdog, where
   it has one, is the only bound.
10. **A slow call is dead time twice.** With every call to the motor
    taking 300 ms the speed went 25% over and took 16 s to settle. The
    tree waits for each call, so it commands at 3 Hz and not 10, and the
    provider answers one request at a time, so the poll of the motor
    waits behind the call too.
11. **Samples stamped as their reads return work, and do not cure the
    stall.** With the sample's time taken on the runtime's monotonic
    clock as its read returns, the loop did what it did on the
    provider's times in every run repeated, and a provider's clock is
    no longer read at all: 14 lines in the state cache and about 100 in
    the spike. Every read reaches the engine and not only the latest. It
    did not change finding 4: the first call after a stall still runs
    before any read has returned, on the sample from before it, and
    with a maximum age of 5 s the speed still dipped, to 1191 rpm. What
    makes a stall safe is the maximum age.
    A clock that steps is finding 16.
12. **An estimator that works between samples doubts itself less on a
    host off the period, and not never.** With the engine called 30%
    slower or faster than its period, the estimator of the motor's speed
    was not OK in 66 and 38 of some 370 samples under the rule as it
    was, and in 32 and 0 under the amended one; called on the period it
    was OK throughout under both, and 18 rpm from the speed at most
    where it had been 94. These are single runs. What is left on the
    slow host is not all the estimator's: the simulator steps its motor
    ten times a second, so a speed read between two steps is up to
    100 ms older than the time its read returned, and no time the
    runtime has says so.
13. **The first output is `bad`.** The engine is called before the first
    poll has put the speed in the cache.

14. **A provider that dies is silent to a graph fed on arrival.** While
    the simulator was down the state cache paused its polls, so no read
    returned and the spike was told nothing: the graph went on giving
    `duty` as OK from the last speed it had, for the 0.3 s the restart
    took. Its only notice would have been the input's maximum age, 1 s.
    What stopped the loop was the tree: three calls failed in a row and
    the runtime went to FAULT, in seven runs of eight. The safe-state
    call failed too, the provider being gone.
15. **A restart the runtime rides through resets the plant and not the
    controller.** In the eighth run the restart finished between the
    second failed call and the third, the runtime stayed in AUTO, and
    the new simulator had its motor at rest. The graph still held the
    integral of a loop at 1500 rpm, 0.50, and nothing told it to
    follow: the runtime was acting. The duty went to its limit and the
    speed to 1650 rpm, 10% over. `follow` is driven by what the runtime
    knows, AUTO and the device-loss latch, and neither moved. Which of
    the two happens is a race between the restart's backoff, 200 ms
    here, and three ticks of the tree. With a backoff of 50 ms the
    runtime stayed in AUTO in every run, and went 10% over in every
    one. The spike's `idle` is now true as well while the device's
    provider is down, and from then until a read of the device has
    returned. With that, in four runs of four, the controller followed
    the motor's own report through the restart, started again from the
    duty of 0 the new process reported, and the speed rose to 1500 rpm
    and did not pass it. It is about 40 lines in the spike, and one more
    thing the layer is told by the runtime: whether a provider can be
    reached.
16. **A doubted command is worse sent than not, and not sending it is
    not free.** With `ride_through` on the measurement the controller
    ran for 1.5 s on a speed that was no longer true. Sent, as
    `tree-stale.xml` does, the duty went to its limit and the speed to
    2429 rpm, 62% over. Not sent, the motor kept its last duty, as in
    finding 9, and the controller, which cannot know its command was
    not sent, wound up against a motor that was not listening: 13% over
    when the speed came back. Without `ride_through` (finding 9) the
    `duty` was `bad`, the controller waited, and there was no overshoot.
    Wiring the duty the motor reports to the PID's `applied` took the
    overshoot away and cost the step: with `applied_within: 0.02` the
    loop took 7.7 s to settle where it takes 3.2 s, the reported duty
    being a tick behind the one given. These are single runs. The graph
    needs to know that its command was not sent, and P2 gives it no way
    but the device's own report.
17. **A step of the one clock is taken in stride; a provider's clock
    stepping by itself stops the loop for good.** With the calls and
    the sample times stepped together by 30 s the engine saw a gap of
    300 ticks going forward and, going back, refused one call, was
    rebased, and gave no `duty` for one poll; the speed did not move
    off 1500 rpm in either. With the sample times alone stepped by 2 s,
    either way, every sample from then on was thrown away or too old,
    `duty` was `bad` from 0.84 s after, the tree sent nothing, the
    runtime stayed in AUTO, and the motor ran on at its last duty: what
    finding 2 has from the start, arrived at in the middle of a run. A
    step back of 300 ms, inside the maximum age, cost 4 samples. No
    real clock was stepped: the spike adds the step to the times it
    reads. On arrival times there is nothing to step.

18. **A graph that hangs has to be found out by whoever reads it.**
    With the engine's thread stopped and no check, the spike went on
    answering every read with the last duty, as `OK`: the tree sent it
    122 times, the runtime stayed in AUTO and health read well. A
    provider in a process of its own that hung would fail its reads and
    be latched as lost; in process there is no read to fail. The spike
    now judges, on the reader's thread, how long ago a call last ran a
    tick, and gives every output as `FAULT` past three call intervals:
    the duty read `FAULT` 0.14 s after the hang and the tree made no
    call after it. The engine's thread also catches what the engine
    throws and stops, so that the outputs age out and the runtime goes
    on; no run threw. Either way the motor ran on at its last duty,
    which is finding 9: stopping the command is not bounding it. One
    run each.

19. **A reading its provider calls STALE is no value, and saying
    nothing is the worst of three.** The spike first wrote one as
    `uncertain` with the time its read returned: a value its provider
    had called too old, given to the graph as new. Leaving it out, so
    that the input's maximum age judges, sounds more careful and is
    not: for the second the old sample stayed within its age the
    controller ran on a speed of 557 rpm that was no longer true, its
    duty read `OK` and was sent, and the speed went to 2636 rpm, 76%
    over. Written as `bad`, the duty was `bad` at the next tick, the
    controller waited, and the speed did not pass its setpoint. So
    `bad` is the default. It is the same lesson as finding 4: a maximum
    age is how long a loop runs blind, and 1 s is ten samples of this
    one. One run each, and the STALE is a FAULT renamed: the value
    beside it was the true speed, where a real one would be old.

20. **The runtime need not depend on the engine.** The layer is now
    behind a CMake option, `ANOLIS_ENABLE_DERIVED`, off unless given, as
    `ANOLIS_ENABLE_AUTOMATION` decides whether a build has behaviour
    trees. FluxGraph was named in two places, the layer's directory and
    the build file, so this was the build's to arrange and one block of
    startup to guard.
    - Built without it, the configure log does not hold the word
      `fluxgraph`, and `nm -C` on `anolis-runtime` finds no symbol that
      names it. Built with it, 724 do, and the binary is 640 kB larger
      of 18.5 MB.
    - A build without it refuses a config with a `derived` section as
      the config is loaded, before a provider is started: "this build
      has no derived signals: build with -DANOLIS_ENABLE_DERIVED=ON",
      and exit 1. Automation's absence is a warning and the runtime
      goes on; here that would leave someone believing in a reading
      that is not there.
    - The 894 unit tests pass in both builds, each with warnings as
      errors.
    - The graph file is read by FluxGraph's own YAML loader now, where
      the spike had a reading of its own, the third in this work, and
      each input is looked up once for a handle: a config that names an
      input the graph has not got is refused at startup, where it was a
      count of refused writes.
    - On the build with it, pinned to FluxGraph's `main`, the default
      run is as it was: within 5% of 1500 rpm between 3.11 s and
      3.15 s in three runs, not past its setpoint, 201 calls and 201
      ticks, no write refused.

## Signals that are read: two providers, telemetry, a doubted output

`monitor.py` runs what the RFC's first version is for: no tree, and
nothing acted on. Two simulators, a graph over a noisy voltage of each
(`monitor.yaml`), telemetry on, polled every 500 ms, for 60 s. At 20 s
the first simulator's signal reads `FAULT` for 1.5 s, and at 35 s the
second's for 5 s.

```bash
python3 tools/derived-signals/runtime-spike/monitor.py \
  --runtime build/ci-linux-release/core/anolis-runtime \
  --provider ../anolis-provider-sim/build/ci-linux-release/anolis-provider-sim \
  --out <an empty directory>
```

Telemetry goes to a stand-in for InfluxDB inside the script, which
answers each write and counts its rows. It is not InfluxDB: nothing is
stored, and what is counted is what the runtime sends. One run of each
of the two graphs.

| Derived signal | What it is | Quality over HTTP | Rows in 60 s |
| --- | --- | --- | --- |
| `apart` | `a - b`, of two providers | `OK`; `FAULT` from 20.8 s to 22.3 s and from 35.8 s to 40.8 s | 5 |
| `far` | whether that is over 0.05 | as `apart` | 5 |
| `smooth` | a low-pass of `a`, riding through 2 s | `OK`; `STALE` from 20.8 s to 22.3 s | 117 |
| `quiet` | `smooth` through a deadband of 0.01 | as `smooth` | 3 |
| `mean` | the mean of `a` over 10 s, 80% of it needed | `UNAVAILABLE` until 7.8 s, then `OK` throughout | 103 |

Each simulator's own signal wrote 119 rows. The device of the graph
wrote 469 rows of the run's 1893.

<!-- markdownlint-disable MD029 -- the findings keep one numbering -->
21. **A signal of two providers is one more signal, and is `FAULT`
    while either is.** `apart` came out, went `FAULT` 0.8 s after the
    first simulator's signal did and again after the second's, and came
    back with them. The window's `mean` did not move through the 1.5 s:
    it needs 80% of its samples, and had them. The two simulators give
    the same noise, so `apart` was the same number throughout, and this
    says nothing of two signals that differ.

22. **A derived signal that moves writes as many rows as what it is
    made of, and the layer's own counts write more than its signals.**
    Telemetry writes a row when a value changes at all. A low-pass of a
    noisy signal changes at every tick: 117 rows against the signal's
    119. Through a deadband of 0.01 it wrote 3. So a graph that
    publishes a filtered signal doubles that signal's rows unless it
    ends in a deadband, and what the deadband is has to be chosen for
    each. Of the device's 469 rows, 233 were its signals; the other
    236 were the spike's counts of its own calls and ticks, which
    change at every call. A count like that is not a thing to publish as a signal
    that telemetry follows.

23. **A doubted output is `STALE` to everything that reads it, and one
    signal colours its whole device.** With the signals of `a` alone
    (`--one`), while the low-pass rode through, HTTP gave `smooth` and
    `quiet` as `STALE`, and the health of the graph's device went from
    `OK` to `STALE` with `stale_signal_count` 2 and back. Nothing a
    reader has tells that `STALE` from a provider's, which is the RFC's
    O1. With `apart` in the graph the same 1.5 s made the device
    `FAULT`: the device is as bad as its worst signal, so a graph of
    unrelated quantities reports one health for all of them. And a
    window that needs 8 s of samples leaves its device `STALE` for the
    first 8 s of every start.
<!-- markdownlint-enable MD029 -->

## What is not here

A second device of one provider in a loop, a tree that is not in
AUTO at the start, a graph changed while the runtime runs, a build with
automation off, tests. The other placement, in a provider, is not built.
The workbench has not been looked at: what it shows of a derived signal
that is `STALE` is not known.
