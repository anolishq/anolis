# Process dashboard generator (prototype)

A prototype for anolishq/anolis#477 and #353, not shipped in any release:
packaging is decided after the bench evaluation.

`process_dashboard.py` turns a runtime's saved inventory into a Grafana
dashboard for that runtime, with no per-machine code. It is a prototype
(anolishq/anolis#477) for one question: whether the metadata providers
already declare is enough for a useful first process view. Nothing installs
or provisions its output.

## Use

Save the inventory, one capabilities response per device:

```bash
api=http://127.0.0.1:8080
curl -s "$api/v0/devices" > devices.json
jq -r '.devices[] | "\(.provider_id)/\(.device_id)"' devices.json | while read -r id; do
  curl -s "$api/v0/devices/$id/capabilities"
done | jq -s --slurpfile devices devices.json '{devices: $devices[0], capabilities: .}' > inventory.json
```

With `http.auth_enabled: true` each request needs the API token (see
`docs/deploy/install-sh.md`). Then generate, naming the runtime as its
telemetry does (`runtime_name` in the runtime config; the HTTP API does not
report it):

```bash
python3 tools/process-dashboard/process_dashboard.py inventory.json --runtime-name bioreactor -o process.json
```

Import `process.json` in Grafana (Dashboards → New → Import). The output is
deterministic: the same inventory gives the same file, whatever order the
devices are listed in, so it can be kept in version control and diffed.

## What it draws

- A row per device, titled with its display name, `provider_id/device_id` and
  type.
- A panel per signal, never shared:
  - `double`, `int64`, `uint64`: a line drawn in steps, the declared unit
    shown verbatim on the axis and on each value;
  - `bool` and `string`: a state timeline. Booleans get neutral colours,
    because whether `true` is good depends on the signal;
  - `bytes`, and any type the generator does not know: a text panel saying it
    is not plotted.
- A quality timeline per device, one line per signal.
- Mode and parameter changes as annotations.

Each panel's description carries the signal's description, its full identity,
its value type and its unit, or "no unit declared".

Queries pin all four identity tags exactly, so a signal id repeated on
another device, provider or runtime never leaks in. Values are drawn as the
bundled Signal History draws them (see `observability/README.md`, "How the
panels read change-driven data"): the last value in each window, held through windows with
no change, seeded from up to 30 days before the range while the runtime
reports health.

Panel ids derive from identity, so adding a device does not renumber the
panels that were there. The dashboard uid derives from the runtime name.

## What the metadata can and cannot tell it

Evaluated against the bioreactor (bread RLHT and DCMT, EZO pH and DO) and the
simulator (tempctl, motorctl, relayio, analogsensor), from the signals those
providers declare (`tests/observability/fixtures/`). A captured bioreactor
inventory is still to be checked against this.

**Inferable, and used:**

- Which signals belong to one device, and that device's name and type.
- Value type, which decides the panel type.
- The unit, as text. That is enough to label an axis honestly. It is not
  enough to compare or convert: units are free text (`C`, `%`, `mg/L`, `pH`),
  with no vocabulary behind them.

**Not inferable:**

- **Which signals belong on one chart.** A setpoint and its measurement are
  the obvious pair, and nothing declares it. Each provider names them
  differently: bread RLHT has `setpoint1_c` beside `t1_c`, bread DCMT has
  `motor1_target` beside `motor1_value`, and the simulator has a `setpoint`
  signal set by `set_setpoint`. Matching names would be provider-specific
  rules under another name. A shared unit says nothing either: dissolved
  oxygen in mg/L and in % are one quantity in two forms, while two `%`
  signals on different devices are unrelated.
- **What a signal means in the machine.** Whether a temperature is the
  culture or a heater is the machine's knowledge, not the provider's. Device
  display names from the provider config ("Reactor Temperature Node") carry
  some of it; signals carry none.
- **Ranges and thresholds.** Some are in description prose ("0-10V", "PWM duty
  0..1"); none are structured on signals. Function arguments have `min` and
  `max`, signals do not. None are invented.

**Gaps in what providers declare**, cheaper to fix at the source than with new
metadata:

- bread declares no labels: each signal's label is its id (`t1_c`), so that is
  what the panels show.
- The EZO labels include the unit ("Dissolved Oxygen (mg/L)").
- The simulator's motor duty has no unit; its range is only in the
  description.

**Contract notes:**

- `runtime_name` is not available over HTTP, so it is an argument.
- The runtime reports a value type it cannot name as `unknown`
  (`core/http/json.cpp`), which the OpenAPI `ValueType` enum does not list
  (anolishq/anolis#499). The generator shows such a signal as not plotted.
- A `bytes` value is written to telemetry as a string of the raw bytes
  (anolishq/anolis#500).
- The runtime already holds a per-signal freshness bound and poll rate that
  HTTP does not report. Each signal's provider-declared `stale_after_ms` and
  `poll_hint_hz` are kept in the device registry
  (`core/registry/device_registry.hpp`), and bread declares 5000 ms and
  1.0 Hz on every signal. `encode_signal_spec` (`core/http/json.cpp`) puts
  only the id, value type, label, unit and description on HTTP, so the saved
  inventory, and the generator, never see them (anolishq/anolis#498).

**Is more metadata justified?** Not new metadata, by this evaluation. The
useful panel-level improvements are provider fixes above. Pairing is the one
thing that needs information nobody declares; the next experiment, if pairing
is wanted, is an optional human-written file naming pairs for one machine, not
a provider or contract change.

Metadata that already exists is another matter. Providers already declare a
freshness bound for each signal and the runtime keeps it; reporting
`stale_after_ms` on the signal spec over HTTP is the one field between it and
a generated dashboard that marks staleness per signal. That is a contract
change, so it is out of scope here; anolishq/anolis#498 tracks it.

## Limitations

- **A runtime that stops inside the range reads as steady and OK until the
  stop leaves the range.** Nothing is written when a runtime stops, and both
  the value panels and the device's quality timeline hold each series' last
  row through windows with no change (`createEmpty: true`, then
  `fill(usePrevious: true)`). So after a stop, with or without a restart, the
  last value and the last quality, usually OK, are drawn to the end of the
  range, and a gap before a restart is not marked. The liveness gate drops
  the seed only once the stop is before the range. The bundled Signal History
  and Device Health's Quality Distribution Over Time draw a stop the same way
  (anolishq/anolis#497).
- After a quality-only row (a non-finite double), the value line holds the last
  finite value. The device's quality timeline shows the change.
- `uint64` values above 2^53 lose precision: Grafana plots numbers as 64-bit
  floats.
- A signal whose last change is more than 30 days before the range is not
  seeded.
- No unit conversion, no scaling, and no layout beyond two panels a row.
- Rendering in a browser is not tested; `tests/observability/` runs the
  queries and checks the model.
