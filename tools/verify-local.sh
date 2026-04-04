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
