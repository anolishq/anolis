#!/usr/bin/env bats
# Smoke tests for tools/install.sh
# These run in CI without root, network access, or hardware.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

# ===========================================================================
# Help & usage
# ===========================================================================

@test "--help prints usage and exits 0" {
    run bash "${INSTALL_SH}" --help
    [ "$status" -eq 0 ]
    [[ "${output}" == *"Usage: install.sh"* ]]
    [[ "${output}" == *"--project"* ]]
    [[ "${output}" == *"--local"* ]]
    [[ "${output}" == *"--stage"* ]]
}

@test "-h is an alias for --help" {
    run bash "${INSTALL_SH}" -h
    [ "$status" -eq 0 ]
    [[ "${output}" == *"Usage: install.sh"* ]]
}

# ===========================================================================
# Argument parsing — error cases
# ===========================================================================

@test "unknown option fails with message" {
    run bash "${INSTALL_SH}" --bogus
    [ "$status" -ne 0 ]
    [[ "${output}" == *"Unknown option: --bogus"* ]]
}

@test "non-root execution fails" {
    if [ "$EUID" -eq 0 ]; then
        skip "running as root"
    fi
    run bash "${INSTALL_SH}" --project ./cfg
    [ "$status" -ne 0 ]
    [[ "${output}" == *"must be run as root"* ]]
}

# ===========================================================================
# Dry-run mode (no root needed — we fake EUID)
# ===========================================================================

@test "--dry-run --project prints the online assemble + install phases" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --project ./cfg
    [ "$status" -eq 0 ]
    [[ "${output}" == *"DRY RUN"* ]]
    [[ "${output}" == *"detect architecture"* ]]
    [[ "${output}" == *"assemble bundle from config"* ]]
    [[ "${output}" == *"install binaries"* ]]
    [[ "${output}" == *"install systemd units"* ]]
}

@test "--dry-run --local prints phases for local bundle" {
    local tmpdir
    tmpdir=$(mktemp -d)
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --local "${tmpdir}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"DRY RUN"* ]]
    [[ "${output}" == *"local bundle"* ]]
    rmdir "${tmpdir}"
}

@test "--dry-run --uninstall prints uninstall message" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --uninstall
    [ "$status" -eq 0 ]
    [[ "${output}" == *"dry-run"* ]]
    [[ "${output}" == *"uninstall"* ]]
}

# ===========================================================================
# Argument combinations
# ===========================================================================

@test "--prefix changes install prefix in dry-run output" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --project ./cfg --prefix /usr/local/anolis
    [ "$status" -eq 0 ]
    [[ "${output}" == *"/usr/local/anolis"* ]]
}

@test "--no-start omits start and health from dry-run" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --project ./cfg --no-start
    [ "$status" -eq 0 ]
    [[ "${output}" != *"start services"* ]]
    [[ "${output}" != *"health check"* ]]
}

# ===========================================================================
# Validation: require --project or --local
# ===========================================================================

@test "install without --project or --local fails" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"--project"* ]] || [[ "${output}" == *"--local"* ]]
}

# ===========================================================================
# replace_binaries — installing over a RUNNING executable (#181)
# ===========================================================================
# Linux refuses to write the text of an executing ELF (ETXTBSY), so a plain
# `cp` over the binaries of a live service fails — which broke idempotent
# re-runs, upgrades, and --rollback on the #138 bench. replace_binaries must
# rename over the destination instead.

_setup_running_binary() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    SRC="${BATS_TEST_TMPDIR}/bundle-bin"
    DEST="${BATS_TEST_TMPDIR}/prefix-bin"
    mkdir -p "${SRC}" "${DEST}"

    # Simulate the running service: the destination binary is a real ELF
    # that is currently executing.
    cp /bin/sleep "${DEST}/anolis-runtime"
    "${DEST}/anolis-runtime" 30 &
    RUNNING_PID=$!

    # The replacement the bundle ships.
    printf 'new-binary-content\n' > "${SRC}/anolis-runtime"
}

_teardown_running_binary() {
    kill "${RUNNING_PID}" 2>/dev/null || true
    wait "${RUNNING_PID}" 2>/dev/null || true
}

@test "replace_binaries: plain cp over a running ELF fails (simulation is real)" {
    _setup_running_binary

    run cp "${SRC}/anolis-runtime" "${DEST}/anolis-runtime"

    _teardown_running_binary
    [ "$status" -ne 0 ]
    [[ "${output}" == *"Text file busy"* ]]
}

@test "replace_binaries: replaces a running ELF and leaves no temp litter" {
    _setup_running_binary

    run replace_binaries "${SRC}" "${DEST}"
    replaced=$(cat "${DEST}/anolis-runtime")
    leftovers=$(find "${DEST}" -name '.*.tmp.*' | wc -l)
    kill -0 "${RUNNING_PID}"  # old inode still executing until the restart

    _teardown_running_binary
    [ "$status" -eq 0 ]
    [ "${replaced}" = "new-binary-content" ]
    [ "${leftovers}" -eq 0 ]
}

@test "replace_binaries: copies every regular file and skips subdirectories" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    src="${BATS_TEST_TMPDIR}/multi-src"
    dest="${BATS_TEST_TMPDIR}/multi-dest"
    mkdir -p "${src}/subdir" "${dest}"
    printf 'a\n' > "${src}/anolis-runtime"
    printf 'b\n' > "${src}/anolis-provider-bread"

    run replace_binaries "${src}" "${dest}"
    [ "$status" -eq 0 ]
    [ "$(cat "${dest}/anolis-runtime")" = "a" ]
    [ "$(cat "${dest}/anolis-provider-bread")" = "b" ]
    [ ! -e "${dest}/subdir" ]
}

@test "replace_binaries: fails cleanly when the destination is unwritable" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    src="${BATS_TEST_TMPDIR}/ro-src"
    dest="${BATS_TEST_TMPDIR}/ro-dest"
    mkdir -p "${src}" "${dest}"
    printf 'x\n' > "${src}/anolis-runtime"
    chmod 555 "${dest}"

    run replace_binaries "${src}" "${dest}"
    chmod 755 "${dest}"
    [ "$status" -ne 0 ]
    [ "$(find "${dest}" -type f | wc -l)" -eq 0 ]
}

# ===========================================================================
# _stage_fetch failure messaging (#138 matrix row 13)
# ===========================================================================

@test "_stage_fetch failure names the release page, GITHUB_TOKEN, and --local" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    run _stage_fetch "anolishq/definitely-not-a-repo" "v9.9.9" "nope.tar.gz" "${BATS_TEST_TMPDIR}"

    [ "$status" -ne 0 ]
    [[ "${output}" == *"failed to download nope.tar.gz"* ]]
    [[ "${output}" == *"releases/tag/v9.9.9"* ]]
    [[ "${output}" == *"GITHUB_TOKEN"* ]]
    [[ "${output}" == *"--local"* ]]
}

# ===========================================================================
# #222: --local refuses a --prefix that differs from the staged bundle
# ===========================================================================

# Write a bundle dir with a manifest.json in the writer's indent-2 shape.
# $2 empty => omit the "prefix" field (legacy pre-#222 bundle).
_mk_bundle() {
    local dir="$1" prefix="$2"
    mkdir -p "${dir}"
    if [[ -n "${prefix}" ]]; then
        cat > "${dir}/manifest.json" <<JSON
{
  "schema_version": 1,
  "profile": "p",
  "version": "9.9.9",
  "arch": "x86_64",
  "prefix": "${prefix}",
  "components": {
    "runtime": {"version": "9.9.9"}
  }
}
JSON
    else
        cat > "${dir}/manifest.json" <<JSON
{
  "schema_version": 1,
  "profile": "p",
  "version": "9.9.9",
  "arch": "x86_64",
  "components": {
    "runtime": {"version": "9.9.9"}
  }
}
JSON
    fi
}

@test "#222 parse_args --prefix sets PREFIX_EXPLICIT" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    PREFIX_EXPLICIT=0
    parse_args --prefix /x
    [ "${PREFIX_EXPLICIT}" -eq 1 ]
    [ "${PREFIX}" = "/x" ]
    # A parse with no --prefix leaves it unset.
    PREFIX_EXPLICIT=0
    parse_args --no-start
    [ "${PREFIX_EXPLICIT}" -eq 0 ]
}

@test "#222 --local refuses an explicit --prefix that differs from the staged prefix" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    _mk_bundle "${BATS_TEST_TMPDIR}/b" "/opt/anolis"
    PROJECT_DIR=""; LOCAL_PATH="${BATS_TEST_TMPDIR}/b"
    PREFIX="/usr/local/anolis"; PREFIX_EXPLICIT=1
    run phase_prepare_bundle
    [ "$status" -ne 0 ]
    [[ "${output}" == *"staged for prefix /opt/anolis"* ]]
    [[ "${output}" == *"Re-stage"* ]]
}

@test "#222 --local with a matching explicit --prefix proceeds" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    _mk_bundle "${BATS_TEST_TMPDIR}/b" "/opt/anolis"
    PROJECT_DIR=""; LOCAL_PATH="${BATS_TEST_TMPDIR}/b"
    PREFIX="/opt/anolis"; PREFIX_EXPLICIT=1
    run phase_prepare_bundle
    [ "$status" -eq 0 ]
}

@test "#222 --local with no --prefix adopts the staged prefix" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    _mk_bundle "${BATS_TEST_TMPDIR}/b" "/usr/local/anolis"
    PROJECT_DIR=""; LOCAL_PATH="${BATS_TEST_TMPDIR}/b"
    PREFIX="/opt/anolis"; PREFIX_EXPLICIT=0
    # Call directly so the PREFIX adoption is observable in this shell.
    phase_prepare_bundle
    [ "${PREFIX}" = "/usr/local/anolis" ]
}

@test "#222 --local legacy bundle without a prefix field proceeds (warns if explicit)" {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    _mk_bundle "${BATS_TEST_TMPDIR}/b" ""
    PROJECT_DIR=""; LOCAL_PATH="${BATS_TEST_TMPDIR}/b"
    PREFIX="/usr/local/anolis"; PREFIX_EXPLICIT=1
    run phase_prepare_bundle
    [ "$status" -eq 0 ]
    [[ "${output}" == *"predates prefix recording"* ]]
}
