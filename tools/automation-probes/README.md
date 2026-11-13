# Automation authoring probes

Evidence behind the issues under #813. Nothing here is built or run by CI.

## `bt_nodes_test.probes.patch`

Throwaway tests appended to `tests/unit/bt_nodes_test.cpp`, written against
`main` at c3a7fd7 with BehaviorTree.CPP 4.9.0. Apply and build:

```bash
git apply tools/automation-probes/bt_nodes_test.probes.patch
cmake --build --preset ci-linux-release --parallel --target anolis_unit_tests
```

Several tests need a directory to write to and skip nothing on their own, so
run them by filter, not as part of the suite:

| Filter | Environment | Shows |
| --- | --- | --- |
| `BTNodesTest.ProbeDumpNodeModel` | `ANOLIS_PROBE_OUT` | the node model `writeTreeNodesModelXML` gives (#817) |
| `BTNodesTest.ProbeScriptOnTypedEntries` | | a `Script` reading values the Anolis nodes write |
| `BTNodesTest.ProbeGrootDefaultSaveSemantics` | | empty literals on typed ports fail at load (#818) |
| `BTNodesTest.ProbeVocab*` | `ANOLIS_VOCAB_DIR`, `ANOLIS_VOCAB_ORIG` | the bioreactor rewrites against the original (anolishq/anolis-projects#163) |
| `ReproRuntime.A6*` | `ANOLIS_REPRO_OUT` | `CallDevice` with unreadable args (#806) |
| `ReproRuntime.A5*` | `ANOLIS_REPRO_OUT` | ignored condition attributes, undeclared parameters (#805, #808) |
| `ReproRuntime.C4*` | `ANOLIS_REPRO_OUT` | a failed change-gated write (#812) |
| `ReproRuntime.C6*` | `ANOLIS_REPRO_OUT` | re-entering AUTO (#811) |
| `ReproRuntime.C7*` | `ANOLIS_REPRO_OUT` | Anolis nodes in a SubTree (#807) |
| `BTNodesTest.Repro*` | | `PeriodicPulseWindow` stale outputs (#810), `BuildArgsJson` value sources (#818) |
| `ReproRuntime.ReadSignalOnStringSignal` | | `ReadSignal` on a string signal (#816) |

`ReproRuntime.*` uses the real nodes, `BTRuntime`, `CallRouter` and
`ModeManager`; the only stand-in is a recording provider. `ProbeVocab*` uses a
recording stand-in for `CallDevice`. `ProbeLoadGrootSavedFiles` and
`ProbeVocab*` read files that are not in this branch.

When a fix lands, take its test from here and make it a real one.

## `groot2/`

A way to look at Groot2 without a desktop session: one launch on a private
Xvfb display, scripted with xdotool, screenshots with ImageMagick's `import`.
It needs `Xvfb`, `xdotool` and `import`, and a shell that may open Unix
sockets.

```bash
cp -r tools/automation-probes/groot2 /some/scratch/groot2 && cd /some/scratch/groot2
/path/to/Groot2.AppImage --appimage-extract      # leaves ./squashfs-root
bash shot.sh demo actions-view.sh --file /path/to/tree-with-model.xml
ls shots/
```

`shot.sh <name> <actions|-> [groot2 args]` starts Groot2 with its settings
redirected under `./xdg`, waits `STARTUP_WAIT` seconds (default 8), sources
the actions file, and always takes `<name>-final.png`. An actions file may
call `snap <label>`, `xdotool` and `sleep`. `actions-view.sh` dismisses
dialogs, captures the tree as drawn, zoomed out and zoomed in, and saves with
Ctrl+S when `DO_SAVE=1`.

Checked with Groot2 1.10.0 on a 1600x1000 display. Coordinates in an actions
file are for that size.
