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
[doc("Build + test both lanes (`just test /path/to/anolis-provider-sim` names the sim).")]
test provider="": test-cpp (test-py provider)

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
#
# The suite also needs an anolis-provider-sim binary. `provider` names one
# (`just test-py /path/to/anolis-provider-sim`);
# without it the harness looks beside this clone. The recipe checks it resolves
# before pytest starts, and never fetches it or skips the suite without it.
[doc("Run the Python suite (`just test-py /path/to/anolis-provider-sim` names the sim).")]
test-py provider="":
    #!/usr/bin/env bash
    set -euo pipefail
    provider={{quote(provider)}}
    # A relative path is the caller's, not the justfile directory's.
    if [[ -n "${provider}" && "${provider}" != /* ]]; then
        provider={{quote(invocation_directory())}}/"${provider}"
    fi
    # Exit 3 means the harness's own resolver found no sim; any other failure
    # is reported as itself.
    rc=0
    sim=$(uv run python -c 'import sys
    from tests.conftest import _resolve_provider
    try:
        print(_resolve_provider(sys.argv[1] or None))
    except RuntimeError:
        sys.exit(3)' "${provider}") || rc=$?
    if [[ ${rc} -ne 0 && ${rc} -ne 3 ]]; then
        exit "${rc}"
    fi
    if [[ ${rc} -eq 3 ]]; then
        ref=$(awk '/provider_sim:/{f=1; next} f && /ref:/{print $2; exit}' .ci/dependency-pins.yml)
        echo "error: no anolis-provider-sim binary found${provider:+ at ${provider}}." >&2
        echo "The suite needs anolis-provider-sim ${ref}, the version pinned in .ci/dependency-pins.yml." >&2
        echo "Download it from https://github.com/anolishq/anolis-provider-sim/releases/tag/${ref}," >&2
        echo "or build it beside this clone, where the suite finds it without an argument:" >&2
        echo "  git clone --branch ${ref} https://github.com/anolishq/anolis-provider-sim ../anolis-provider-sim" >&2
        echo "  cd ../anolis-provider-sim && cmake --preset ci-linux-release && cmake --build --preset ci-linux-release" >&2
        echo "Then run: just test-py /path/to/anolis-provider-sim" >&2
        exit 1
    fi
    uv run pytest tests/integration tests/scenarios --runtime build/{{preset}}/core/anolis-runtime --provider "${sim}"

# The dashboard query suite needs Docker, so `test` does not include it. It
# starts a throwaway InfluxDB + Grafana (the dev stack's image pins, made
# collision-free by tests/observability/compose.test.yml), runs pytest in the
# stack's `tester` service, and always tears the stack down. Arguments go to
# pytest as given and replace the default path, e.g.
# `just test-observability tests/observability -k "liveness and not quiet"`.
[doc("Run the Grafana dashboard queries against fixture telemetry (needs Docker).")]
[positional-arguments]
test-observability *args:
    #!/usr/bin/env bash
    set -euo pipefail
    compose=(docker compose -p anolis-obs-test --env-file tests/observability/compose.test.env
        -f observability/docker-compose.observability.yml -f tests/observability/compose.test.yml)
    trap '"${compose[@]}" down -v >/dev/null 2>&1' EXIT
    "${compose[@]}" up -d --wait --quiet-pull
    "${compose[@]}" run --rm tester "$@"
