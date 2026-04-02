# Mixed-Bus Validation and Scenario Pack

This directory is a validation/scenario pack for `bread0` + `ezo0`.
It is not the canonical application profile.

Canonical bioreactor runtime profile:

1. `config/bioreactor/anolis-runtime.bioreactor.manual.yaml`
2. `config/bioreactor/README.md`

## Runtime Profiles

1. `anolis-runtime.mixed.win.mock.yaml`
   - Windows mock baseline.
   - Uses `provider-bread.mock.yaml` + `provider-ezo.mock.yaml`.
   - HTTP port `18080`.
   - Expected inventory: 6 devices (`rlht0`, `dcmt0`, `dcmt1`, `ph0`, `do0`, `ec0`).

2. `anolis-runtime.mixed.win.mock.conflict.yaml`
   - Windows mock ownership-conflict negative test.
   - Uses `provider-bread.conflict.mock.yaml` + `provider-ezo.conflict.mock.yaml`.
   - Intentionally duplicates ownership of `(bus_path, i2c_address)` at
     `mock://mixed-bus-conflict`, `0x63`.
   - Expected result: runtime startup fails fast with duplicate ownership error.

3. `anolis-runtime.mixed.yaml`
   - Linux hardware validation/support profile.
   - Uses `provider-bread.yaml` + `provider-ezo.yaml`.
   - Address map: RLHT `0x0A`, DCMT `0x14`, DCMT `0x15`, EZO pH `0x63`, EZO DO `0x61`.
   - Polling interval `2500ms`.
   - HTTP port `8080`.
   - Expected inventory: 5 devices (`rlht0`, `dcmt0`, `dcmt1`, `ph0`, `do0`).

## Provider Configs

1. `provider-bread.yaml` (Linux hardware support profile)
2. `provider-ezo.yaml` (Linux hardware support profile)
3. `provider-bread.mock.yaml` (Windows mock baseline)
4. `provider-ezo.mock.yaml` (Windows mock baseline)
5. `provider-bread.conflict.mock.yaml` (Windows mock negative test)
6. `provider-ezo.conflict.mock.yaml` (Windows mock negative test)

## Build Prerequisites (Preset-Based)

Build all three repos before validation so runtime command paths exist.

### Linux/macOS

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

### Windows (PowerShell)

```powershell
Set-Location D:\repos_feast\anolis
cmake --preset dev-windows-release
cmake --build --preset dev-windows-release

Set-Location D:\repos_feast\anolis-provider-bread
cmake --preset dev-windows-release
cmake --build --preset dev-windows-release

Set-Location D:\repos_feast\anolis-provider-ezo
cmake --preset dev-windows-release
cmake --build --preset dev-windows-release
```

## A) Windows Mock Baseline Validation

Start runtime:

```powershell
Set-Location D:\repos_feast\anolis
Get-NetTCPConnection -LocalPort 18080 -ErrorAction SilentlyContinue
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\config\mixed-bus-providers\anolis-runtime.mixed.win.mock.yaml
```

Validate endpoints:

```powershell
$base = 'http://127.0.0.1:18080'
Invoke-RestMethod "$base/v0/runtime/status" | ConvertTo-Json -Depth 8
Invoke-RestMethod "$base/v0/providers/health" | ConvertTo-Json -Depth 8
Invoke-RestMethod "$base/v0/devices" | ConvertTo-Json -Depth 8
Invoke-RestMethod "$base/v0/state" | ConvertTo-Json -Depth 8
```

Expected:

1. Runtime stays up.
2. `bread0` and `ezo0` are present.
3. Inventory includes 6 devices.

## B) Windows Mock Ownership-Conflict Negative Test

Run conflict profile:

```powershell
Set-Location D:\repos_feast\anolis
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\config\mixed-bus-providers\anolis-runtime.mixed.win.mock.conflict.yaml
$LASTEXITCODE
```

Expected:

1. Runtime startup fails before steady state.
2. Error includes:
   - `I2C ownership validation failed`
   - `duplicate ownership for bus='mock://mixed-bus-conflict' addr='0x63'`
   - conflicting owners `bread0/rlht_conflict` and `ezo0/ph_conflict`
3. Exit code is non-zero.

## C) Linux Hardware Support Validation

Start runtime:

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config ./config/mixed-bus-providers/anolis-runtime.mixed.yaml
```

Capture validation artifacts:

```bash
cd /path/to/anolis
./config/mixed-bus-providers/check_mixed_bus_http.sh \
  --base-url http://127.0.0.1:8080 \
  --expect-providers bread0,ezo0 \
  --min-device-count 5 \
  --capture-dir artifacts/mixed-bus-validation/mixed
```

Expected:

1. Runtime and both providers are `AVAILABLE`.
2. Inventory includes 5 devices.
3. Script exits `0` and writes artifacts.
