# Inventory fixtures

Saved runtime inventories for `observability/process_dashboard.py`: the body
of `GET /v0/devices` and of each device's capabilities response, in the
shape the generator reads. `test_process_dashboard.py` checks both against
`schemas/http/runtime-http.openapi.v0.yaml`.

They are built by hand from the signals each provider declares, not captured
from a running runtime. `functions` is left empty; the generator does not read
it.

| File | Devices | Declared in |
| --- | --- | --- |
| `inventory-bioreactor.json` | `bread0`: `rlht0`, `dcmt0`, `dcmt1`; `ezo0`: `ph0`, `do0`. Device ids and labels from anolis-projects `projects/bioreactor-v1/config/provider-*.bioreactor.yaml` | anolis-provider-bread `src/devices/common/inventory.cpp` (`build_rlht_capabilities`, `build_dcmt_capabilities`; the label is the signal id); anolis-provider-ezo `src/devices/ph/ph_adapter.cpp`, `src/devices/do/do_adapter.cpp` |
| `inventory-sim.json` | `sim0`: `tempctl0`, `tempctl1`, `motorctl0`, `relayio0`, `analogsensor0`. Device ids from anolis-provider-sim `config/provider-sim.yaml` and `config/multi-tempctl.yaml` | anolis-provider-sim `src/devices/{tempctl,motorctl,relayio,analogsensor}/*_device.cpp` |

`rlht0` includes `estop_count`, which bread declares only on firmware that
reports e-stop counts. A captured bioreactor inventory is to replace the
hand-built one for the evaluation in #477.
