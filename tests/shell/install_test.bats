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
