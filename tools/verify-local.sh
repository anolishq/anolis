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
for candidate in \
  build/dev-release/core/anolis-runtime \
  build/dev-windows-release/core/Release/anolis-runtime.exe

do
  if [ -f "$candidate" ]; then
    RUNTIME_BIN="$candidate"
    break
  fi
done

PROVIDER_BIN=""
for candidate in \
  ../anolis-provider-sim/build/dev-release/anolis-provider-sim \
  ../anolis-provider-sim/build/dev-windows-release/Release/anolis-provider-sim.exe

do
  if [ -f "$candidate" ]; then
    PROVIDER_BIN="$candidate"
    break
  fi
done

if [ -n "$RUNTIME_BIN" ]; then
  echo "[verify-local] Running runtime config contract checks"
  "$PYTHON_BIN" tools/contracts/validate-runtime-configs.py --runtime-bin "$RUNTIME_BIN"
else
  echo "[verify-local] Skipping runtime config contract checks: runtime binary not found"
fi

echo "[verify-local] Running machine profile contract checks"
"$PYTHON_BIN" tools/contracts/validate-machine-profiles.py

echo "[verify-local] Running telemetry timeseries contract checks"
"$PYTHON_BIN" tools/contracts/validate-telemetry-timeseries.py

echo "[verify-local] Running docs local-link checks"
"$PYTHON_BIN" tools/contracts/validate-doc-links.py

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

if ! command -v ctest >/dev/null 2>&1; then
  echo "[verify-local] Skipping focused C++ tests: ctest not found"
  exit 0
fi

for build_dir in build/dev-release build/dev-windows-release; do
  if [ -f "$build_dir/CTestTestfile.cmake" ]; then
    echo "[verify-local] Running focused C++ tests from $build_dir"
    ctest --test-dir "$build_dir" --output-on-failure -R "ConfigTest|RuntimeOwnershipValidationTest"
    exit 0
  fi
done

echo "[verify-local] Skipping focused C++ tests: no supported build directory found"
