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
clone, which is how every figure here was made, on that branch as it
stood on 2026-11-15.

## What it did

Each row is one run of `run.py` with the flags given, a step from rest to
1500 rpm, sampled over HTTP every 50 ms.

| Flags | Within 5% at | Highest | After the disturbance | Ticks in one call, most |
| --- | --- | --- | --- | --- |
| none | 3.2 s | 1500 rpm | n/a | 1 |
| `--poll-ms 500` | 3.1 s | 1500 rpm | n/a | 1 |
| `--stall-s 3 --seconds 25` | 3.1 s | 1500 rpm | 1500 rpm throughout | 30 |
| `--stall-s 3 --at 1` | 4.5 s | 1500 rpm | lowest 1402 rpm | 30 |
| `--stall-s 3 --at 1 --graph motor-pid-no-commands.yaml` | 4.8 s | 1500 rpm | lowest 1389 rpm | 30 |
| `--unreachable-s 3 --at 8` | 3.1 s | 1500 rpm | 0 rpm, and stays there | 1 |

No run had a gap, a refused call or a refused write.

## What it found

1. **The cost of this placement is small.** 522 lines in `core/derived/`,
   about 50 in the config loader, 14 in `Runtime::init_providers` and a
   target in CMake. The registry, HTTP, change events and health needed no
   change. It bends the invariant that a provider is an isolated process,
   and the handle is not supervised: the main loop walks
   `config_.providers`.
2. **The poll interval is not the loop's sample rate.** The call router
   polls a device at once after every call to it, so a loop that commands
   at the tree's 10 Hz reads the motor at 10 Hz whatever
   `polling.interval_ms` says. A graph that only watches would see 2 Hz at
   the default.
3. **The graph cannot see the runtime's mode.** With the motor's reads
   failing, the graph's output went `bad`, the tree sent nothing, the
   device-loss latch took the runtime to FAULT and the safe state set the
   duty to 0. That is the runtime doing its job. The graph then ran on
   with the motor at rest, raised `duty` to its limit of 1 and published
   it as OK, for nobody. Its integral did not wind up, since the PID stops
   it at a limit, and `track` was not used, since the PID only tracks when
   it starts again after an input it could not use. A layer that is told
   nothing of AUTO, FAULT or the latch gives a command that is `good`
   while nothing acts on it.
4. **Quality had to be mapped both ways, and the mapping decides what the
   tree sends.** In: OK is `good`, STALE `uncertain`, FAULT `bad`, UNKNOWN
   `not_ready`, and a value the cache does not hold is `bad`. Out: the
   same backwards, with a value of 0 where the engine has none, since the
   protocol cannot say "no value". The tree gates on OK, so an
   `uncertain` command is not sent. That is O1, guessed at, and no run
   here tested the guess.
5. **The stall runs did not tell the two graphs apart.** A 3 s stall made
   one call run 30 ticks. With the speed's `max_age` at 1 s the samples
   were too old for most of those ticks, the PID gave no value on them
   and kept its integral, with the command listed or not. The difference
   between 1402 and 1389 rpm is within what one run varies by.
6. **Not tested: the clock.** The calls and the sample times are both the
   wall clock, the simulator's and the runtime's on one machine (O7). A
   step of it would reach the engine, which refuses a call earlier than
   the last; the spike then rebases. No run stepped the clock.
7. **The first output is `bad`.** The engine is called before the first
   poll has put the speed in the cache.

## What is not here

A provider restart, a second device, telemetry, a tree that is not in
AUTO at the start, a graph changed while the runtime runs, a build with
automation off, tests. The other placement, in a provider, is not built.
