# Getting Started

## Prerequisites

- **Windows**: Visual Studio 2022 with C++ desktop development
- **Linux**: GCC 11+, CMake 3.20+
- **vcpkg**: dependency management
- **Python 3**: test tooling
- **Git**: source control

## Install Dependencies

Linux (Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip tar pkg-config python3 python3-pip
```

Windows (PowerShell):

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install Git.Git
winget install Python.Python.3.12
```

Install Visual Studio 2022 (or Build Tools) with the `Desktop development with C++` workload.

Install vcpkg (Linux/macOS):

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh"
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.bashrc
export VCPKG_ROOT="$HOME/vcpkg"
test -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

Install vcpkg (Windows):

```powershell
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\\vcpkg", "User")
$env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User")
Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
```

## Quick Start (Preset-First)

Linux/macOS:

```bash
git clone https://github.com/FEASTorg/anolis.git
cd anolis
git submodule update --init --recursive
cmake --preset dev-release
cmake --build --preset dev-release --parallel
ctest --preset dev-release
./build/dev-release/core/anolis-runtime --config ./anolis-runtime.yaml
```

Windows (PowerShell):

```powershell
git clone https://github.com/FEASTorg/anolis.git
Set-Location anolis
git submodule update --init --recursive
cmake --preset dev-windows-release
cmake --build --preset dev-windows-release --parallel
ctest --preset dev-windows-release
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\anolis-runtime.yaml
```

Use presets directly when needed:

```bash
cmake --list-presets
ctest --list-presets
```

## Runtime API Smoke Check

```bash
# List devices
curl -s http://127.0.0.1:8080/v0/devices | jq

# Read state
curl -s http://127.0.0.1:8080/v0/state/sim0/motorctl0 | jq
```

See [http-api.md](http-api.md) for full API details.

## Operator UI

```bash
python -m http.server 3000 -d tools/operator-ui
# Open http://localhost:3000
```

## Automation Quickstart

Enable automation in runtime config:

```yaml
automation:
  enabled: true
  behavior_tree: ./behaviors/demo.xml
  tick_rate_hz: 10
  manual_gating_policy: BLOCK
  parameters:
    - name: temp_setpoint
      type: double
      default: 25.0
      min: 10.0
      max: 50.0
```

Update a parameter at runtime:

```bash
curl -s -X POST http://127.0.0.1:8080/v0/parameters \
  -H "Content-Type: application/json" \
  -d '{"name": "temp_setpoint", "value": 30.0}' | jq
```

Switch to AUTO mode:

```bash
curl -s -X POST http://127.0.0.1:8080/v0/mode \
  -H "Content-Type: application/json" \
  -d '{"mode": "AUTO"}' | jq
```

## Next Steps

- For provider integration and safety contracts: [providers.md](providers.md)
- For runtime config details: [configuration.md](configuration.md)
- For contributor workflow: [../CONTRIBUTING.md](../CONTRIBUTING.md)
