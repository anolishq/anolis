# Derived signals RFC: review of the first revision

Reviewed: 2026-11-11

Reviewed: the first revision of [the derived-signals RFC](derived-signals.md)
and its evidence in `tools/derived-signals/`, in two passes made without
reference to how either was written. One re-derived the RFC's claims from
the source of the runtime, the protocol and the reference machine's project.
The other re-ran the evidence, wrote new traces against `heater_guard.py`,
and broke the block engine on purpose to see what the check would miss.

## Verdict

Not fit to proceed as it stood. Most claims about the existing system held.
Three things did not:

- The first proposed decision rested on a claim the code contradicts.
- The problem statement left out the two cheapest alternatives.
- The engine check was presented as stronger than it is.

The second revision of the RFC answers each finding below, or carries it as
an open decision.

## Findings and what was done

### The RFC

| # | Finding | Evidence | Outcome |
| --- | --- | --- | --- |
| 1 | P1 said a value in the state cache is read unchanged by HTTP state and the operator panel. It is not: `GET /v0/state` lists devices from the registry, and units are in the registry. | `core/http/handlers/state_handlers.cpp`; `core/state/state_cache.hpp` | P1 rewritten. The registry entry is now an open item under O8. |
| 2 | The case against building a layer was not made. Most of the needs read one device and could be computed in its provider or firmware, where a trip can also act. The tree's library already has scripting. | `tools/derived-signals/signals.yaml`; anolis-projects `bioreactor-v1/docs/runbook.md`, "Heater"; [safety.md](../safety.md), "The runtime does enter FAULT autonomously" | New open decision O0, worked first. |
| 3 | O6 asked whether safety settings may name a derived signal. For the stop chain the answer is already no: a status that reads in force or unknown holds the latch and sends no safe-state calls, so a derived trip there would leave the heater on. | `core/control/safe_state.cpp`, `SafeStateController::hold` | O6 now states the prohibition. |
| 4 | P2 hid an actor. If the layer calls nothing and the tree does not run in `MANUAL`, something new must invoke the stop, which adds a cause of autonomous `FAULT` to a list the safety doc keeps closed. | [safety.md](../safety.md) | Stated under P2 and O6. |
| 5 | P4 did not say what "good" is. The cache keeps a value with quality `OK` when its provider goes down, and `ReadSignal` gives the tree a number whatever its quality. | `core/state/state_cache.cpp`; `core/automation/bt_nodes.cpp` | Four shapes of "not good" listed under O1. |
| 6 | P3's stated reason was wrong. Periodic evaluation also sees inputs of mixed age. | The RFC's own O7 | Reason replaced: a fixed number of evaluations per period. |
| 7 | P5 does not match how the runtime reads config, which is parsed by hand and warns on an unknown key. | `core/runtime/config.cpp` | Stated under P5; O9 now comes before P5 is accepted. |
| 8 | The spike reads operator parameters, which exist only with automation enabled. | `tools/derived-signals/signals.yaml` | Added to O4. |
| 9 | The tree-node claim was overstated: `CheckBool` compares, and two nodes remember a previous value. | `core/automation/bt_nodes.cpp` | Problem statement reworded. |
| 10 | The quality claim missed that both published quality enumerations are closed, and that `CheckQuality` does not recognise `UNAVAILABLE`. | `schemas/http/runtime-http.openapi.v0.yaml`; `core/automation/bt_nodes.cpp` | Added to O1. |
| 11 | Not asked: whether a derived number that changes on every evaluation floods events and telemetry; how a provider id is reserved. | `core/state/state_cache.cpp` | Added to O8. |

### The evidence

| # | Finding | Evidence | Outcome |
| --- | --- | --- | --- |
| 12 | "Never trips" was an artefact of a trace that ended too soon. The bad reading delays the trip by 902.5 s. | A longer trace | Traces lengthened; RFC and outputs corrected. |
| 13 | Under the `hold` policy the engine and the reference disagreed: the engine could trip while its input was bad and the reference could not. | A trace with the pad reading bad from 1900 s to 1910 s | Reference changed to match; trace added to the check. |
| 14 | The reference's no-good-reading clock started at 0, not at the first tick. | A trace that starts at 500 s | Fixed. |
| 15 | The check caught 4 of 37 further ways of breaking the engine. Every recorded sample that night is good, bad and not ready were compared as the same thing, and both sides were one person's code on one grid. | Mutation run on a copy | README and O3 corrected. Rebuilding the check is the first work under O2 and O3. |
| 16 | A fallback that gave the wrong value still passed. | Same run | Fixed: it now fails the run. |
| 17 | Six of seventeen published signals were never compared. | `engine-results.txt` | Stated; not yet fixed. |
| 18 | "Within one point", "before each calibration call", "predicted", "changed 111 times" were each overstated. | Recomputed from the recordings | Evidence section reworded with the figures. |
| 19 | The style comparison was not even-handed, and the file that was made to run is half formulas. It does not carry P5 by itself. | The three style files | P5 and the README say so. |
| 20 | Engine defects: malformed files that load and then fail while ticking; a not-a-number reading with quality `OK`; `median` reporting through bad input; a fallback replacing both outputs of a two-output block; a mixed time base. | Probes on a copy | Listed in the README as known defects. Not fixed: the rules they need are O1 and O2. |

## What held

- The claims about polling, timestamps, the protocol, startup order and
  FluxGraph.
- The reference detectors are a faithful port of `heater_guard.py` up to
  the first trip: no difference over 4,000 random traces.
- The committed outputs reproduce exactly.
- The formula evaluator refuses attribute access, subscripts, powers,
  chained comparisons and unknown calls.

## Recommendation carried forward

Work O0 before any other decision. If the heater's trip belongs in its
firmware or provider, what is left are aids to the operator, and the
layer's safety scope reduces to one prohibition.
