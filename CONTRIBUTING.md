# Contributing to Anolis

Thank you for your interest in contributing to Anolis!

## Getting Started

### Prerequisites

- **Windows**: Visual Studio 2022 with C++ desktop development workload
- **Linux**: GCC 11+, CMake 3.20+
- **Git**: For version control

### Setup

1. Clone the repository:

   ```bash
   git clone https://github.com/FEASTorg/anolis.git
   cd anolis
   ```

2. Initialize protocol submodule:

   ```bash
   git submodule update --init --recursive
   ```

3. Configure and build:

   ```bash
   # Linux/macOS
   cmake --preset dev-release
   cmake --build --preset dev-release --parallel

   # Windows (PowerShell)
   cmake --preset dev-windows-release
   cmake --build --preset dev-windows-release --parallel
   ```

4. Run the tests:

   ```bash
   # Linux/macOS
   ctest --preset dev-release

   # Windows
   ctest --preset dev-windows-release
   ```

## Development Workflow

### Building

```bash
# Linux/macOS
cmake --preset dev-release
cmake --build --preset dev-release --parallel

# Windows
cmake --preset dev-windows-release
cmake --build --preset dev-windows-release --parallel

# Clean build
# Linux/macOS:
rm -rf build/dev-release
cmake --preset dev-release
cmake --build --preset dev-release --parallel
# Windows (PowerShell):
#   Remove-Item -Recurse -Force .\build\dev-windows-release
#   cmake --preset dev-windows-release
#   cmake --build --preset dev-windows-release --parallel
```

### Running

```bash
# Linux/macOS
./build/dev-release/core/anolis-runtime --config ./config/anolis-runtime.yaml

# Windows
.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\config\anolis-runtime.yaml
```

### Testing

```bash
# Focused local drift check (composer + key runtime tests)
bash tools/verify-local.sh

# Run all tests (includes unit + integration)
ctest --preset dev-release                           # Linux/macOS
ctest --preset dev-windows-release                   # Windows

# Or run pytest suites directly
python -m pytest tests/integration/test_integration.py -m "not stress and not slow"
python -m pytest tests/scenarios/test_scenarios.py -m "not stress and not slow"
```

## Code Guidelines

### C++ Style

- C++17 standard
- Use `std::filesystem` for path handling
- Platform-specific code must be guarded with `#ifdef _WIN32` / `#else`
- Prefer RAII for resource management
- Use `std::unique_ptr` / `std::shared_ptr` appropriately

### Commits

- Write clear, concise commit messages
- Reference issue numbers when applicable
- Keep commits focused on a single change

### Pull Requests

1. Create a branch from `main`
2. Make your changes
3. Ensure all tests pass locally
4. Push and create a PR
5. Wait for CI to pass
6. Request review

## CI/CD

Both `anolis` and `anolis-provider-sim` use GitHub Actions for CI. Tests run on:

- Ubuntu 24.04 (GCC)
- Windows Server 2022 (MSVC 2022)

### Test Requirements

#### Regression Rule

Any bug fixed must include a test that would have caught it.

#### Test Structure

- Unit tests: C++ with GoogleTest in `tests/unit/`
- Integration tests: Python in `tests/integration/`
- Validation scenarios: Python in `tests/scenarios/`
- Tests must clean up processes after completion
- Tests must exit with proper exit codes (0 = pass, non-zero = fail)

### Safety Requirements for New Features

Anolis enforces a **safe-by-default** design. Contributions must maintain safety properties:

#### Test Configuration Defaults

- ✅ **Runtime mode not configurable**: Runtime always starts in IDLE mode (safe default)
- ✅ **Use HTTP API for mode changes**: Tests/production must use `POST /v0/mode` to transition modes
- ❌ **Don't set mode in YAML**: YAML `runtime.mode` field is rejected by config validation

**Mode transitions via HTTP API**:

```bash
# Start in IDLE (enforced at startup)
# Transition to MANUAL
curl -X POST http://localhost:8080/v0/mode -d '{"mode": "MANUAL"}'

# Transition to AUTO (requires automation enabled)
curl -X POST http://localhost:8080/v0/mode -d '{"mode": "AUTO"}'
```

#### Safety Test Requirements

New features that affect operational safety MUST include tests covering:

1. **IDLE Mode Behavior**
   - Control operations blocked in IDLE
   - Read-only operations allowed in IDLE
   - Proper error codes returned (FAILED_PRECONDITION)

2. **Mode Transition Enforcement**
   - Valid transitions succeed
   - Invalid transitions rejected (e.g., FAULT → AUTO)
   - Automation stops when exiting AUTO mode

3. **Fault Handling**
   - Error conditions transition to FAULT mode
   - FAULT mode blocks control operations
   - Recovery path requires MANUAL mode

**Test patterns**:

```cpp
// C++ Unit Test Pattern: IDLE blocking
TEST_F(FeatureTest, IdleModeBlocks ControlOperations) {
    mode_manager->set_mode(RuntimeMode::IDLE, err);

    auto result = feature->execute_operation(req);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, Status_Code_FAILED_PRECONDITION);
   EXPECT_THAT(result.error_message, HasSubstr("IDLE"));
}
```

```python
# Python Integration Test Pattern: Mode enforcement
def test_feature_respects_idle_mode(self):
    set_mode(self.base_url, "IDLE")

    resp = requests.post(f"{self.base_url}/v0/feature/action")

    assert resp.status_code == 400 or resp.status_code == 409
    # Or check JSON body for FAILED_PRECONDITION status
```

#### Mode Enforcement Testing Patterns

When adding features that interact with devices:

1. **CallRouter integration**: Use CallRouter for device calls (don't bypass)
2. **Mode checks**: Verify mode restrictions before operations
3. **Error propagation**: Return appropriate error codes (FAILED_PRECONDITION for mode blocks)

**Example CallRouter usage**:

```cpp
// ✅ Correct: Uses CallRouter (mode checks automatic)
control::CallRequest req;
req.device_handle = "provider/device";
req.function_name = "actuate";
auto result = call_router->execute_call(req, provider_registry);

// ❌ Wrong: Direct provider call (bypasses mode checks)
provider->call(device_id, function_id, args);
```

#### Parameter vs Device State Distinction

Maintain clear separation between automation parameters and device state:

| Concept                   | Storage                            | Authority         | Mutability             |
| ------------------------- | ---------------------------------- | ----------------- | ---------------------- |
| **Automation Parameters** | Runtime config / BT blackboard     | Anolis core       | Operator-configurable  |
| **Device State**          | StateCache (polled from providers) | Provider/hardware | Hardware-authoritative |

**Guidelines**:

- ✅ **Parameters**: Automation setpoints, thresholds, timeouts (BT-visible, operator-tunable)
- ✅ **Device State**: Sensor readings, actuator positions, hardware status (StateCache only)
- ❌ **Never**: Store device state in automation parameters
- ❌ **Never**: Use BT blackboard for hardware state

**Example distinction**:

```cpp
// ✅ Parameter (operator-configurable, automation input)
automation_params_["target_temperature"] = 60.0;

// ✅ Device state (hardware-authoritative, read from StateCache)
auto temp_signal = state_cache_.get_signal("provider/device", "temperature");

// ❌ Wrong: Don't store hardware state in parameters
automation_params_["current_temperature"] = temp_signal.get_value();  // NO!
```

## Code Quality

### C++ Code Quality

#### Static Analysis (clang-tidy)

**Primary Check**: clang-tidy runs in CI on **Linux** builds. MSVC/Visual Studio integration is unreliable (mixed diagnostics, incomplete coverage).
So we do **not** enforce clang-tidy on Windows.

**Local runs (recommended: Linux/WSL)**:

- Configure with `-DENABLE_CLANG_TIDY=ON` (example: `cmake --preset dev-debug -DENABLE_CLANG_TIDY=ON`).
- Or use a preset that sets `ENABLE_CLANG_TIDY=ON`.
- You need `clang-tidy` installed (`sudo apt install clang-tidy`).

**Windows note**: You can install clang-tidy with the "C++ Clang tools" workload or LLVM, but results under MSVC are incomplete.
Prefer running analysis from Linux/WSL instead.

**To apply fixes**: Use Linux/WSL with `compile_commands.json`; MSVC generators do not support apply-fixes reliably.

#### Formatting (clang-format)

We use `clang-format` to enforce C++ style. **CI uses clang-format 19** - ensure you use the same version locally to avoid format mismatches.

**Installation:**

- **Linux**: Install from LLVM apt repository:

  ```bash
  wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
  sudo add-apt-repository "deb http://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-19 main"
  sudo apt-get update
  sudo apt-get install clang-format-19
  sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-19 100
  ```

- **Windows**: Download from [LLVM releases](https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.5)
- **Verify**: `clang-format --version` should show version 19.x

```bash
# Apply formatting
clang-format -i core/runtime/src/main.cpp

# Recursively (PowerShell)
Get-ChildItem -Path core,tests -Recurse -Include *.cpp,*.hpp | ForEach-Object { clang-format -i $_.FullName }

# Recursively (Bash)
find core tests \( -name "*.cpp" -o -name "*.hpp" \) -print0 | xargs -0 clang-format -i
```

### Python Code Quality

We use `ruff` for both linting and formatting Python scripts.

#### Python setup

```bash
pip install -r requirements.txt
```

#### Generating/updating the lock file (UTF-8)

On Windows PowerShell, redirection (`>`) defaults to UTF-16; regenerate the lock file with UTF-8 to avoid CI decode errors.

```sh
# Create fresh virtual environment
python -m venv .venv

# Activate virtual environment
# PowerShell
.\.venv\Scripts\Activate.ps1
# Bash
source .venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# PowerShell (Windows PowerShell 5.x and Windows PowerShell 7+ — safe & explicit)
python -m pip freeze | Out-File -Encoding utf8 requirements-lock.txt

# PowerShell 7+ (set UTF-8 as default redirection encoding, then normal > works)
$PSDefaultParameterValues['Out-File:Encoding'] = 'utf8'
python -m pip freeze > requirements-lock.txt

# Git Bash / WSL (UTF-8 by default)
python -m pip freeze > requirements-lock.txt

# Quick verify (fails non-zero if not UTF-8)
python -c "open('requirements-lock.txt','rb').read().decode('utf-8')"
```

#### Linting & Formatting

```bash
# Fix auto-fixable lint issues
ruff check --fix .

# Apply formatting
ruff format .
```

### Sanitizers

#### AddressSanitizer (ASAN)

To run with AddressSanitizer (ASAN) on Linux/macOS:

```bash
cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address"
cmake --build build_asan
ctest --test-dir build_asan
```

#### ThreadSanitizer (TSAN) - Race Detection

ThreadSanitizer detects data races at runtime with 5-20x performance overhead.
Use the dedicated `ci-tsan` preset to keep TSAN cache/toolchain isolated.

**Building with TSAN:**

```bash
# Linux/WSL only (uses x64-linux-tsan vcpkg triplet for consistent instrumentation)
cmake --preset ci-tsan
cmake --build --preset ci-tsan --parallel

# Clean rebuild recommended when switching to/from TSAN
rm -rf build/ci-tsan
cmake --preset ci-tsan
cmake --build --preset ci-tsan --parallel
```

**Running Tests with TSAN:**

```bash
# Run all tests (unit + integration)
ctest --preset ci-tsan

# TSAN is NOT supported on Windows MSVC - use WSL or Linux
```

**Expected Results:**

- **Unit Tests:** 178/178 pass (1 HTTP test disabled due to cpp-httplib false positives)
- **Integration Tests:** 5/6 pass (supervision test skipped - timing-sensitive)

**Race Reports:**

If races detected, reports written to:

```bash
~/anolis/tsan-report.*

# Check for reports
ls -la ~/anolis/tsan-report.* 2>/dev/null
# No files = No races detected ✅
```

**CI/CD Integration:**

TSAN detection is automatic via environment variables (`TSAN_OPTIONS`, `LD_PRELOAD`).
Tests skip timing-sensitive checks gracefully under TSAN without special configuration.

**Limitations:**

- Windows: TSAN only works with GCC/Clang on Linux/macOS
- Performance: 5-20x slowdown (6.2x unit tests, 20x process spawning)
- Memory: 5-10x increased usage
- Timing tests: Must skip or relax timeouts due to overhead

## Project Structure

```sh
anolis/
├── core/               # Runtime kernel
│   ├── control/        # Call router
│   ├── http/           # HTTP server
│   ├── provider/       # Provider host
│   ├── registry/       # Device registry
│   ├── runtime/        # Config and bootstrap
│   ├── src/            # Main entry point
│   └── state/          # State cache
├── docs/               # Documentation
├── sdk/                # Language SDKs
├── spec/               # Protocol specifications
└── tools/              # Developer tools
    └── operator-ui/    # Web-based operator interface
```

---

## Cross-Platform Notes

- **Linux is the reference platform** (deployment target)
- **Windows is a supported dev platform**
- Test on both platforms before submitting changes that touch:
  - Process spawning (`provider_process.cpp`)
  - File paths
  - Signal handling
  - Socket operations

---

## Common Pitfalls & Solutions

### 1. Windows Macro Conflicts

**Problem**: Windows headers define macros like `min`, `max`, `GetTickCount` that conflict with C++ standard library and protobuf.

**Symptoms**:

```text
error C2039: 'GetTickCount': is not a member of 'google::protobuf::util::TimeUtil'
error C2589: '(': illegal token on right side of '::'
```

**Solution**: In any header that uses `std::min`, `std::max`, or protobuf time utilities:

```cpp
#pragma once

// MUST be before any Windows headers
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <algorithm>
#include <google/protobuf/util/time_util.h>

// Use parentheses to prevent macro expansion
double clamped = (std::max)(lo, (std::min)(hi, v));
auto ts = (google::protobuf::util::TimeUtil::GetCurrentTime)();
```

### 2. C++ Incomplete Type Errors

**Problem**: Forward declarations cause "incomplete type" errors when used in containers.

**Symptoms**:

```text
error: invalid use of incomplete type 'struct SignalSpec'
```

**Solution**: Define structs before they're used in container types:

```cpp
// WRONG - forward declaration used before definition
struct SignalSpec;
struct DeviceCapabilitySet {
    std::unordered_map<std::string, SignalSpec> signals;  // Error!
};
struct SignalSpec { ... };

// RIGHT - define before use
struct SignalSpec { ... };
struct DeviceCapabilitySet {
    std::unordered_map<std::string, SignalSpec> signals;  // OK
};
```

### 3. vcpkg Package Sharing Between Projects

**Problem**: Re-running dependency resolution across projects increases build time.

**Solution**: Use pinned vcpkg baseline + cached vcpkg root (preset-first) instead of manual cross-repo `VCPKG_INSTALLED_DIR` wiring:

```bash
# Anolis
cmake --preset dev-release
cmake --build --preset dev-release

# Provider-sim (separate repo, separate build tree)
cmake --preset ci-linux-release
cmake --build --preset ci-linux-release
```

In CI, use reusable setup actions and pinned commit/baseline policy (`docs/dependencies.md`).

### 4. Linux Process Cleanup (pkill)

**Problem**: `pkill -f name` matches the full command line, including paths like `/home/user/anolis/...`.
This can accidentally kill unrelated processes.

**Symptoms**:

- Test script kills itself (exit code 143 = SIGTERM)
- Python processes terminated unexpectedly

**Solution**: Use `-x` for exact binary name match:

```python
# WRONG - matches paths containing "anolis"
subprocess.run(["pkill", "-f", "anolis-runtime"])

# RIGHT - matches only processes named exactly "anolis-runtime"
subprocess.run(["pkill", "-x", "anolis-runtime"])
```

### 5. Bash Exit Code Capture

**Problem**: `cmd || VAR=$?` doesn't capture the command's exit code.

**Symptoms**: Exit code is always 0 or wrong value.

**Explanation**: The `||` means "run if previous failed", but `$?` then captures the assignment's exit code (always 0), not the command's.

**Solution**:

```bash
# WRONG
python test.py || TEST_EXIT=$?

# RIGHT
set +e  # Don't exit on error
python test.py
TEST_EXIT=$?
if [ $TEST_EXIT -ne 0 ]; then ...
exit $TEST_EXIT
```

### 6. Python subprocess OOM on CI

**Problem**: `subprocess.run(capture_output=True)` buffers all output in memory, causing OOM when tests produce lots of output.

**Symptoms**: Exit code 137 (SIGKILL) on Linux CI, "Killed" in logs.

**Solution**: Stream output to log files:

```python
# WRONG - buffers everything in memory
result = subprocess.run(cmd, capture_output=True)

# RIGHT - stream to file
with open("test.log", "w") as log_file:
    result = subprocess.run(cmd, stdout=log_file, stderr=subprocess.STDOUT)
```

### 7. GitHub Actions Permissions

**Problem**: Some diagnostic commands require elevated permissions.

**Symptoms**: `dmesg: read kernel buffer failed: Operation not permitted`

**Solution**: Silence permission errors, these are nice-to-have diagnostics:

```bash
dmesg 2>/dev/null | grep -i "oom\|killed" || true
```

---

## Signal Handling Safety

The Anolis runtime uses POSIX signal handlers for graceful shutdown (SIGINT/SIGTERM).
Signal handlers have **strict safety constraints** that must be followed.

### Async-Signal-Safe Requirements

According to POSIX standards, signal handlers may **only** call async-signal-safe functions.
Most C/C++ standard library functions are **NOT** safe, including:

- ❌ `malloc/free`, `new/delete` (heap allocation)
- ❌ `std::mutex::lock()`, `std::lock_guard` (mutexes)
- ❌ `LOG_INFO()`, `std::cout`, `printf()` (I/O operations)
- ❌ `std::function` callback invocation (may allocate)
- ✅ `std::atomic<T>::load/store()` (safe)
- ✅ Writing to `volatile sig_atomic_t` (safe, but atomic preferred)

**Violation Consequences**: Deadlocks, crashes, undefined behavior (especially under ThreadSanitizer/ASAN).

### Implementation Pattern

The runtime uses an **atomic flag polling pattern**:

```cpp
// signal_handler.hpp
class SignalHandler {
public:
    static void install();
    static bool is_shutdown_requested();
private:
    static void handle_signal(int signal);
    static std::atomic<bool> shutdown_requested_;
};

// signal_handler.cpp
std::atomic<bool> SignalHandler::shutdown_requested_{false};

void SignalHandler::handle_signal(int signal) {
    // ONLY atomic operations in signal context
    shutdown_requested_.store(true);
    // NO callbacks, NO logging, NO mutexes
}

bool SignalHandler::is_shutdown_requested() {
    return shutdown_requested_.load();
}

// Main loop polls the flag
while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (SignalHandler::is_shutdown_requested()) {
        LOG_INFO("Signal received, stopping...");  // Safe: not in signal context
        running_ = false;
        break;
    }
}
```

### Testing Signal Handling

Signal handling is validated by the pytest integration suite:

```bash
python -m pytest tests/integration/test_integration.py -k signal_handling
```

This test verifies:

- Clean shutdown on SIGINT (Ctrl+C)
- Clean shutdown on SIGTERM (kill)
- No hangs or deadlocks
- Shutdown completes within timeout

Run with ThreadSanitizer when available to detect data races:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=thread
cmake --build build
python -m pytest tests/integration/test_integration.py -k signal_handling --runtime=build/dev-release/core/anolis-runtime --provider=/path/to/anolis-provider-sim
```

---

## Concurrency and Threading

The Anolis runtime is multithreaded and uses mutex-based synchronization to protect shared state.
To prevent deadlocks, **all code must follow the documented lock hierarchy**.

### Lock Hierarchy

Locks must be acquired in this order (never acquire a lock "earlier" in the list while holding a lock "later" in the list):

1. **ModeManager** (`automation/mode_manager.hpp`)
2. **StateCache** (`state/state_cache.hpp`)
3. **CallRouter** (`control/call_router.hpp`)
4. **EventEmitter** (`events/event_emitter.hpp`)
5. **SubscriberQueue** (`events/subscriber_queue.hpp`)

**Examples:**

✅ **Safe**: Lock ModeManager, then StateCache

```cpp
std::lock_guard<std::mutex> mode_lock(mode_manager_->get_mutex());
std::lock_guard<std::mutex> cache_lock(state_cache_->get_mutex());
```

❌ **Deadlock Risk**: Lock StateCache, then ModeManager (reversed order)

```cpp
std::lock_guard<std::mutex> cache_lock(state_cache_->get_mutex());  // Lock #2
std::lock_guard<std::mutex> mode_lock(mode_manager_->get_mutex());  // Lock #1 - WRONG!
```

### Deadlock Prevention Guidelines

1. **Know the hierarchy**: Before adding lock acquisition, check where it fits in the hierarchy
2. **Never reverse**: Never acquire a "higher" lock while holding a "lower" lock
3. **Minimize lock scope**: Release locks as soon as possible
4. **Avoid nested locking**: Prefer the snapshot-and-release pattern (see below)
5. **Use read-write locks**: For read-heavy workloads, use `std::shared_mutex` with `std::shared_lock` for reads

### Snapshot-and-Release Pattern

When you need to perform work while holding a lock, collect the necessary data and release the lock before doing expensive operations:

```cpp
// ❌ BAD: Long operation under lock
void update_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    // ... update state ...
    event_emitter_->emit(event);  // Nested lock + expensive operation!
}

// ✅ GOOD: Snapshot data, release lock, then emit
void update_state() {
    std::vector<PendingEvent> pending;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ... update state ...
        pending.push_back(event);  // Just collect data
    }  // Lock released

    for (const auto& evt : pending) {
        event_emitter_->emit(evt);  // Emit outside lock
    }
}
```

**Rationale**: This pattern:

- Minimizes lock hold time (better concurrency)
- Prevents nested locking (safer)
- Avoids holding locks during I/O or expensive operations

### Testing for Concurrency Issues

The project uses **ThreadSanitizer (TSAN)** to detect data races and lock inversions.
See the [Sanitizers](#sanitizers) section for build and run instructions.

**Platform Support:**

- ✅ **Linux/macOS**: Full TSAN support via native toolchain
- ⚠️ **Windows**: TSAN not supported by MSVC/clang-cl - use WSL or Linux for validation
- ✅ **CI**: TSAN validation runs automatically via `.github/workflows/extended.yml` on push/PR to `main`

**TSAN builds** use a custom vcpkg triplet (`x64-linux-tsan`) that ensures **all dependencies**
(protobuf, abseil, etc.) are instrumented with `-fsanitize=thread`.
This prevents false positives from uninstrumented libraries.

**When adding new synchronization**:

1. Add unit tests for concurrent access (see `tests/unit/*_concurrency_test.cpp`)
2. Run with TSAN locally before pushing concurrency-related changes (CI also runs TSAN, but local runs shorten feedback):

   **Linux/macOS:**

   ```bash
   rm -rf build/ci-tsan
   cmake --preset ci-tsan
   cmake --build --preset ci-tsan --parallel
   ctest --preset ci-tsan
   ```

   **Windows (via WSL):**

   ```powershell
   # From Windows PowerShell - launches WSL
   wsl bash -lc "cd /mnt/d/repos_feast/anolis && rm -rf build/ci-tsan && cmake --preset ci-tsan && cmake --build --preset ci-tsan --parallel && ctest --preset ci-tsan"
   ```

   **Manual CMake (Linux/macOS only):**

   ```bash
   cmake --preset ci-tsan
   cmake --build --preset ci-tsan --parallel
   ctest --preset ci-tsan
   ```

3. Verify no race reports in output (TSAN reports start with "WARNING: ThreadSanitizer:")

**TSAN Detection in Tests:**

Timing-sensitive tests detect TSAN via environment variables and skip gracefully:

```python
import os

# Check if running under TSAN or timing tests should be skipped
skip_timing = os.environ.get("ANOLIS_SKIP_TIMING_TESTS") == "1"
tsan_env = os.environ.get("ANOLIS_TSAN") == "1" or bool(os.environ.get("TSAN_OPTIONS"))

if skip_timing or tsan_env:
    print("SKIPPING: Timing-sensitive test")
    if skip_timing:
        print("Reason: ANOLIS_SKIP_TIMING_TESTS=1")
    else:
        print("Reason: TSAN detected (TSAN_OPTIONS/ANOLIS_TSAN)")
    return 0  # Skip gracefully
```

**Environment Variables:**

- `ANOLIS_TSAN=1`: Set in TSAN CI/local TSAN invocations to signal timing-sensitive tests
- `ANOLIS_SKIP_TIMING_TESTS=1`: Manual override to skip timing-sensitive tests
- `TSAN_OPTIONS`: TSAN runtime configuration (presence indicates TSAN environment)

**Why skip timing tests?** TSAN's 5-20x overhead breaks timing assumptions (e.g., provider spawn >1s vs <50ms normally).
Functional correctness is validated without TSAN; race detection is covered by unit tests under TSAN.

---

## Test Patterns

### Integration Test Best Practices

#### ✅ DO: Use HTTP API for Assertions

Tests should verify runtime behavior through the HTTP API, not by parsing log output.

```python
# ✅ GOOD: Assert using HTTP API
from tests.support.api_helpers import assert_provider_available, assert_device_count

runtime = RuntimeFixture(
    runtime_path="build/dev-release/core/anolis-runtime",
    provider_path="/path/to/anolis-provider-sim"
)
runtime.start()

assert_provider_available("sim", timeout=5)
assert_device_count(4, timeout=5)

# ❌ BAD: Parse logs for assertions
runtime.wait_for_log("Provider started")  # Fragile! Breaks if log message changes
```

**Why?** API-based assertions:

- Test behavior, not implementation details
- Don't break when log messages are refactored
- Verify the contract users actually depend on

**When to use logs**: For debugging context (e.g., print last 10 ERROR lines on failure), not for pass/fail decisions.

#### ✅ DO: Use Process Group Cleanup

Tests must clean up only their own processes, not globally by name.

```python
# ✅ GOOD: Use RuntimeFixture (process-group scoped cleanup)
from tests.support.runtime_fixture import RuntimeFixture

runtime = RuntimeFixture(runtime_path="...", provider_path="...")
try:
    runtime.start()
    # ... test logic ...
finally:
    runtime.cleanup()  # Kills only this test's process group

# ❌ BAD: Global process killing
import subprocess
subprocess.run(["pkill", "-x", "anolis-runtime"])  # Kills ALL anolis-runtime processes!
```

**Why?** Process-group cleanup:

- Prevents tests from interfering with each other
- Prevents killing developer's manual runtime instances
- Works correctly with parallel test execution

**How it works**:

- Linux: `os.setsid()` creates new session, `os.killpg()` kills group
- Windows: `CREATE_NEW_PROCESS_GROUP` + `CTRL_BREAK_EVENT`

#### Test Structure Template

```python
from tests.support.runtime_fixture import RuntimeFixture
from tests.support.api_helpers import assert_http_available, assert_provider_available

def test_my_feature():
    """Test description here."""
    runtime = RuntimeFixture(
        runtime_path="build/dev-release/core/anolis-runtime",
        provider_path="/path/to/anolis-provider-sim"
    )

    try:
        runtime.start()

        # Wait for runtime ready
        assert_http_available(timeout=5)
        assert_provider_available("sim", timeout=5)

        # Your test logic here
        resp = requests.get("http://localhost:8080/v0/devices")
        assert resp.status_code == 200

    finally:
        runtime.cleanup()  # Always cleanup, even on exception

if __name__ == "__main__":
    test_my_feature()
    print("✓ test_my_feature")
```

### Running Tests

```bash
# Run all tests
python -m pytest tests/integration/test_integration.py -m "not stress and not slow"

# Run scenario suite
python -m pytest tests/scenarios/test_scenarios.py -m "not stress and not slow"

# Run with custom paths
python -m pytest tests/integration/test_integration.py \
  --runtime=build/dev-release/core/anolis-runtime \
  --provider=/path/to/anolis-provider-sim
```

---

## Getting Help

- Check the [documentation](docs/README.md)
- Review existing issues
- Ask in discussions

## License

By contributing, you agree that your contributions will be licensed under the project's license.
