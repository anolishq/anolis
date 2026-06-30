# Task runner for the anolis runtime — a façade over the commands CI runs
# (.github/workflows/ci.yml). CMake stays the build system; `just` only gives
# contributors the same discoverable `just check && just test` flow as the rest
# of the org (per the global task-runner convention).
#
# anolis is dual-lane: C++ (CMake presets + vcpkg) and Python (uv). Top-level
# recipes span both lanes; `*-cpp` / `*-py` variants narrow to one.
#
# `fmt`/`fmt-check`/`lint` call `clang-format`/`clang-tidy` on PATH. The org pins
# those to a SHA-verified static LLVM 18.1.8 build installed in CI by the
# `setup-clang-tools` action; install the same pinned binary on dev boxes so
# dev, editor, and CI run byte-identical bits. Do NOT use the distro/apt
# clang-format here (it drifts: Debian 18.1.8 vs Ubuntu 18.1.3).

# Primary CMake preset — matches the CI gate so `just check`/`just test`
# reproduce CI. Override for fast local iteration, e.g. `just preset=dev-release test`.
preset := "ci-linux-release"

# C++ sources tracked by git (excludes generated build/ output).
cpp_files := "git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx'"

# List available recipes.
default:
    @just --list

# Configure C++ (vcpkg deps resolve during CMake configure) and sync Python deps.
setup:
    cmake --preset {{preset}}
    uv sync --locked --extra dev

# Format both lanes in place (pinned clang-format 18.1.8; ruff).
fmt:
    {{cpp_files}} | xargs clang-format -i
    uv run ruff format .

# Verify formatting without modifying files (CI gate).
fmt-check:
    {{cpp_files}} | xargs clang-format --dry-run --Werror
    uv run ruff format --check .

# Static analysis over both lanes (clang-tidy needs `just setup` first).
lint:
    run-clang-tidy -p build/{{preset}} $({{cpp_files}})
    uv run ruff check . --output-format=github
    uv run mypy .

# CI-equivalent: formatting + lint across both lanes.
check: fmt-check lint

# C++-only formatting + lint.
check-cpp:
    {{cpp_files}} | xargs clang-format --dry-run --Werror
    run-clang-tidy -p build/{{preset}} $({{cpp_files}})

# Python-only formatting + lint + type-check.
check-py:
    uv run ruff format --check .
    uv run ruff check . --output-format=github
    uv run mypy .

# Build + test both lanes.
test: test-cpp test-py

# Build and run the C++ test suite.
test-cpp:
    cmake --build --preset {{preset}} --parallel
    ctest --preset {{preset}} --output-on-failure

# Run the Python test suite against the runtime built by `test-cpp`. Pinning
# --runtime is required: the harness auto-resolver (tests/conftest.py) prefers a
# `build/dev-release` tree if one exists, which may be stale and lack newer
# routes — so an unpinned run can test the wrong binary. The explicit test paths
# (matching pyproject testpaths) are needed so pytest discovers tests/conftest.py
# and registers --runtime before parsing it.
test-py:
    uv run pytest tests/integration tests/scenarios --runtime build/{{preset}}/core/anolis-runtime
