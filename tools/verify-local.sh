#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1; then
  PYTHON_BIN="python"
else
  echo "[verify-local] error: python3 or python is required" >&2
  exit 1
fi

RUNTIME_BIN=""
if [ -f build/dev-release/core/anolis-runtime ]; then
  RUNTIME_BIN="build/dev-release/core/anolis-runtime"
elif [ -f build/dev-windows-release/core/Release/anolis-runtime.exe ]; then
  RUNTIME_BIN="build/dev-windows-release/core/Release/anolis-runtime.exe"
fi

PROVIDER_BIN=""
if [ -f ../anolis-provider-sim/build/dev-release/anolis-provider-sim ]; then
  PROVIDER_BIN="../anolis-provider-sim/build/dev-release/anolis-provider-sim"
elif [ -f ../anolis-provider-sim/build/dev-windows-release/Release/anolis-provider-sim.exe ]; then
  PROVIDER_BIN="../anolis-provider-sim/build/dev-windows-release/Release/anolis-provider-sim.exe"
fi

if [ -n "$RUNTIME_BIN" ]; then
  echo "[verify-local] Running runtime config contract checks"
  "$PYTHON_BIN" tools/contracts/validate-runtime-configs.py --runtime-bin "$RUNTIME_BIN"
else
  echo "[verify-local] Skipping runtime config contract checks: runtime binary not found"
fi

echo "[verify-local] Running machine profile contract checks"
"$PYTHON_BIN" tools/contracts/validate-machine-profiles.py

echo "[verify-local] Running runtime HTTP OpenAPI structural checks"
"$PYTHON_BIN" tools/contracts/validate-runtime-http-openapi.py

echo "[verify-local] Running runtime HTTP OpenAPI example checks"
"$PYTHON_BIN" tools/contracts/validate-runtime-http-examples.py

if [ -n "$RUNTIME_BIN" ] && [ -n "$PROVIDER_BIN" ]; then
  echo "[verify-local] Running runtime HTTP conformance smoke checks"
  "$PYTHON_BIN" tools/contracts/validate-runtime-http-conformance.py \
    --runtime-bin "$RUNTIME_BIN" \
    --provider-bin "$PROVIDER_BIN"
else
  echo "[verify-local] Skipping runtime HTTP conformance smoke checks: runtime or provider-sim binary not found"
fi

echo "[verify-local] Running System Composer test suite"
"$PYTHON_BIN" -m pytest tools/system-composer/tests -q

if ! command -v ctest >/dev/null 2>&1; then
  echo "[verify-local] Skipping focused C++ tests: ctest not found"
  exit 0
fi

if [ -f build/dev-release/CTestTestfile.cmake ]; then
  echo "[verify-local] Running focused C++ tests from build/dev-release"
  ctest --test-dir build/dev-release --output-on-failure -R "ConfigTest|RuntimeOwnershipValidationTest"
  exit 0
fi

if [ -f build/dev-windows-release/CTestTestfile.cmake ]; then
  echo "[verify-local] Running focused C++ tests from build/dev-windows-release"
  ctest --test-dir build/dev-windows-release --output-on-failure -R "ConfigTest|RuntimeOwnershipValidationTest"
  exit 0
fi

echo "[verify-local] Skipping focused C++ tests: no supported build directory found"
