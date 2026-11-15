# The engine in the runtime: a spike

Throwaway. It answers part of O10 of the
[derived-signals RFC](https://github.com/anolishq/anolis/pull/790): what it
costs to host the engine in the runtime's own process, and what a real
host does to a graph. It is not a design and is not for merging.

## What it is

`core/derived/` wraps the version 2 core of FluxGraph
([proposal](https://github.com/anolishq/fluxgraph/pull/182),
[core](https://github.com/anolishq/fluxgraph/pull/185)) behind
`IProviderHandle`, in process. A thread of its own writes the graph's
inputs from the state cache and calls the engine once a period. The
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

The build pins FluxGraph's spike branch by commit.
`-DFETCHCONTENT_SOURCE_DIR_FLUXGRAPH=<clone>` builds against a local
clone, which is how every figure here was made: the runs with
`--rearm-at`, `--signal-fault-s` or `--call-latency-ms` against the
commit pinned, those with `--call-ms`,
`--skew-ms`, `--max-age` or `--graph-line` against the one before it,
`c3ec3a5`, and the other six against the working tree that became that.

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
   tree sends.** In: OK is `good`, STALE `uncertain`, FAULT `bad`, UNKNOWN
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
11. **Not tested: a clock that steps.** The calls and the sample times
   are both the wall clock (O7). A step of it would reach the engine,
   which refuses a call earlier than the last; the spike then rebases.
   `--skew-ms` moves the sample times and not the clock.
12. **The first output is `bad`.** The engine is called before the first
    poll has put the speed in the cache.

## What is not here

A provider restart, a second device, telemetry, a tree that is not in
AUTO at the start, a graph changed while the runtime runs, a build with
automation off, tests. The other placement, in a provider, is not built.
