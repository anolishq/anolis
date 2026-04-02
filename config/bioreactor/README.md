# Bioreactor Manual Validation Profile

This directory is the canonical application runtime profile for the current
bioreactor lab stack.

Scope:

1. Manual/open-loop validation only.
2. No automation.
3. No telemetry sink.

Device map:

1. `bread0`:
   - `rlht0` at `0x0A`
   - `dcmt0` at `0x14`
   - `dcmt1` at `0x15`
2. `ezo0`:
   - `ph0` at `0x63`
   - `do0` at `0x61`

## Files

1. `anolis-runtime.bioreactor.manual.yaml`
2. `provider-bread.bioreactor.yaml`
3. `provider-ezo.bioreactor.yaml`

## Build Prerequisites

Build all three repos before validation so runtime config executable paths exist.

```bash
cd /path/to/anolis
cmake --preset dev-release
cmake --build --preset dev-release

cd /path/to/anolis-provider-bread
cmake --preset dev-linux-hardware-release
cmake --build --preset dev-linux-hardware-release

cd /path/to/anolis-provider-ezo
cmake --preset dev-linux-hardware-release
cmake --build --preset dev-linux-hardware-release
```

## Linux Hardware Baseline

Start runtime:

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config ./config/bioreactor/anolis-runtime.bioreactor.manual.yaml
```

Capture baseline artifacts:

```bash
cd /path/to/anolis
./config/mixed-bus-providers/check_mixed_bus_http.sh \
  --base-url http://127.0.0.1:8080 \
  --expect-providers bread0,ezo0 \
  --min-device-count 5 \
  --capture-dir artifacts/mixed-bus-validation/bioreactor
```

Acceptance:

1. `bread0` and `ezo0` are `AVAILABLE`.
2. Runtime reports 5 devices.
3. Health is stable/OK for `rlht0`, `dcmt0`, `dcmt1`, `ph0`, and `do0`.
4. Artifacts exist in `artifacts/mixed-bus-validation/bioreactor`.

## Operator UI Manual Workflow

Start UI server:

```bash
cd /path/to/anolis/tools/operator-ui
./serve.sh
```

Open `http://127.0.0.1:3000` and validate:

1. All 5 devices are visible.
2. Per-device state updates continuously.
3. Function calls can be issued manually.
4. Automation panel remains graceful while automation is disabled.

## Notes

1. Bosch sensor checks remain outside provider mixed-bus scope.
2. Ownership-conflict negative testing lives in `config/mixed-bus-providers/`.
