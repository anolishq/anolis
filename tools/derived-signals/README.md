# Derived signals: paper exercise (prototype)

Evidence for [the derived-signals RFC](../../docs/rfcs/derived-signals.md),
not shipped in any release. Nothing here is read by the runtime, a provider
or the workbench.

The exercise takes nine quantities the bioreactor reference rig needs, writes
each as plain code, runs that code against the rig's recorded data, then
writes the same nine in three declaration styles and compares them.

| File | What it is |
| --- | --- |
| `reference.py` | The nine cases as plain Python, stdlib only. The behaviour the styles must reproduce. |
| `results.txt` | Its output on the recordings described below. |
| `style-a-expressions.yaml` | One expression per derived signal. |
| `style-b-chains.yaml` | One source signal and a chain of filters. |
| `style-c-blocks.yaml` | A graph of typed blocks. |
| `lint.py` | Loads the three files, compares them, and breaks each on purpose. Needs PyYAML. |
| `lint-results.txt` | Its output. |

## Ground rules

Held fixed for all three styles, so the comparison is about the shape of the
declaration and nothing else. Each is an assumption of the exercise.

1. A derived value is a signal, `derived/<group>/<signal>`, with a unit and a
   quality, readable wherever a device signal is.
2. Everything is evaluated once per period (2.5 s, the rig's poll interval),
   in dependency order, from the latest cached value of each input.
3. An input is a value, a quality and an age. A value that is not good is not
   used as a number.
4. A derived signal has three states: a good value, not ready yet (a window
   still filling, a filter not started), and bad input.
5. The layer produces signals. It calls nothing.

## The nine cases

Each comes from something the rig does today by hand, in a script, or not at
all. Paths are in
[anolis-projects](https://github.com/anolishq/anolis-projects)
`projects/bioreactor-v1`. "Rig record" is the rig's bench notes for the date
given, which are not published.

| # | Quantity | Today | Source |
| --- | --- | --- | --- |
| 1 | Pad temperature with single-sample glitches removed | Nothing. A 1.8 °C reading arrived with quality OK between readings of 29–31 °C | Rig record, 2026-11-08 |
| 2 | Liquid temperature corrected for the thermocouple's bias; pad minus liquid | The operator subtracts about 1.2 °C by hand | `docs/runbook.md`, "Reading the liquid temperature" |
| 3 | Steady liquid temperature for a pad setpoint; pad setpoint for a liquid target | The operator works it out from `pad ≈ target + k × (target − room)` | `docs/runbook.md`, "Choosing the pad setpoint" |
| 4 | Heater trip: over-temperature, no good reading for 120 s, pad below liquid for 30 min | `scripts/heater_guard.py`, outside the runtime | Its docstring: "the runtime has no signal thresholds" |
| 5 | Is the pH reading steady | Judged by eye before calibrating | `docs/runbook.md`, "Before you start" |
| 6 | Relay duty over a window; heater power | A script over exported telemetry, after the run | Rig record, 2026-11-08 |
| 7 | Probe compensation temperature from the liquid temperature | An operator parameter, default 25 | `behaviors/bioreactor_stir_dual_dosing.xml`, the last block |
| 8 | Liquid temperature estimate with an uncertainty, from the model in 3 and the reading in 2 | Nothing | Stress case, not a need. It tests whether a style has room for an estimator |
| 9 | Heater energy since the run started | Nothing | Stress case. It tests state that should outlive a restart |

## Running it

Part 1 needs only `heater_guard.py` from a checkout of anolis-projects:

```bash
python3 tools/derived-signals/reference.py \
  --guard ../anolis-projects/projects/bioreactor-v1/scripts/heater_guard.py
```

It feeds five synthetic traces to the real `Guard` class and to the case 4
detectors, and prints where each trips.

Part 2 replays every case over recordings from the rig, which are not
published:

- `--archive`: a directory of gzipped CSV files, one per 30-minute window,
  named for the window's start in UTC (`20261109T030000Z.csv.gz`). Each is an
  [anolis-telemetry-export](https://github.com/anolishq/anolis-telemetry-export)
  `signals:query` export with `resolution: raw_event`, and has the columns
  `timestamp`, `runtime_name`, `provider_id`, `device_id`, `signal_id`,
  `value`, `value_type`, `quality`. The run reads 2026-10-30 to 2026-11-02
  and 2026-11-06 to 2026-11-09.
- `--room-csv`: the room temperature for the overnight run, one row a minute,
  with columns `time` (local, EST) and `temp_c`. The room sensor was not yet
  a device on the rig that night.

`results.txt` is the output of that run. `python3 tools/derived-signals/lint.py`
needs no data.

## How each case went in each style

| Case | A expressions | B chains | C blocks |
| --- | --- | --- | --- |
| 1 glitch | One call | One filter. Natural | One block |
| 2 bias, gap | One line each. Natural | Offset is a filter; the gap needs the second form | One block each |
| 3 model | One line each. Natural | Second form, whose formula is style A | Four arithmetic blocks, or one `expr` block |
| 4 detectors | Compact, but the bad-input policy is an extra argument that is easy to leave out | Thresholds natural; the quality detector and the two-input test need the second form; two helper signals | Seven blocks; the policy is a named setting |
| 5 steady | One line, two window calls over the same data | The chain forks: two helper signals, then the second form | One window block with two outputs, one `expr` |
| 6 duty, power | One line each | Natural | Two blocks |
| 7 compensation | One line; the fallback needs an operator (`??`) | Natural: mean, deadband, fallback. The best fit of any case to any style | Two blocks |
| 8 estimator | Does not fit: one expression, two outputs, shared state | Does not fit: needs a third form, which is a block | Fits unchanged |
| 9 energy | Persistence and reset sit outside the expression | Natural | Two blocks |

Style B used its plain chain for 12 of 21 nodes, a two-input form for 7 and a
block form for 2. Style C used 17 block types across 26 blocks, 10 of them
unpublished intermediates. The counts are in `lint-results.txt`.

## What this does not show

- One machine, one night and one calibration session. Nothing here exercised
  inputs with skewed timestamps, a provider restart or a runtime restart.
- One person wrote all three styles and the reference, in that order. The
  line counts depend on formatting choices.
- No style was implemented, and `lint.py` is not a parser.
- The filter in case 8 uses illustrative tuning. The 35.4 W in cases 6 and 9
  is 12 V × 2.95 A from a single current reading, not metered.
- The two thresholds in case 5 were chosen before the run and not tuned
  afterwards.
