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
    [[ "${output}" == *"--profile"* ]]
    [[ "${output}" == *"--local"* ]]
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
    # Skip if somehow running as root in CI
    if [ "$EUID" -eq 0 ]; then
        skip "running as root"
    fi
    run bash "${INSTALL_SH}" --profile test
    [ "$status" -ne 0 ]
    [[ "${output}" == *"must be run as root"* ]]
}

# ===========================================================================
# Dry-run mode (no root needed — we fake EUID)
# ===========================================================================

@test "--dry-run --profile prints all phases without acting" {
    run bash -c 'EUID=0; source "'"${INSTALL_SH%/*}"'/../tools/install.sh" 2>/dev/null; EUID=0 DRY_RUN=1 PROFILE=bioreactor-v1 main --dry-run --profile bioreactor-v1'
    # The above is fragile; use the wrapper approach instead:
    run env BATS_INSTALL_FAKE_ROOT=1 bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --profile bioreactor-v1
    [ "$status" -eq 0 ]
    [[ "${output}" == *"DRY RUN"* ]]
    [[ "${output}" == *"detect architecture"* ]]
    [[ "${output}" == *"install binaries"* ]]
    [[ "${output}" == *"install systemd units"* ]]
}

@test "--dry-run --local prints phases for local bundle" {
    local tmpdir
    tmpdir=$(mktemp -d)
    run env BATS_INSTALL_FAKE_ROOT=1 bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --local "${tmpdir}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"DRY RUN"* ]]
    rmdir "${tmpdir}"
}

@test "--dry-run --uninstall prints uninstall message" {
    run env BATS_INSTALL_FAKE_ROOT=1 bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --uninstall
    [ "$status" -eq 0 ]
    [[ "${output}" == *"dry-run"* ]]
    [[ "${output}" == *"uninstall"* ]]
}

# ===========================================================================
# Argument combinations
# ===========================================================================

@test "--prefix changes install prefix in dry-run output" {
    run env BATS_INSTALL_FAKE_ROOT=1 bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --profile test --prefix /usr/local/anolis
    [ "$status" -eq 0 ]
    [[ "${output}" == *"/usr/local/anolis"* ]]
}

@test "--no-start omits start and health from dry-run" {
    run env BATS_INSTALL_FAKE_ROOT=1 bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --profile test --no-start
    [ "$status" -eq 0 ]
    [[ "${output}" != *"start services"* ]]
    [[ "${output}" != *"health check"* ]]
}

# ===========================================================================
# Validation: require --profile or --local
# ===========================================================================

@test "online mode without --profile fails" {
    run env BATS_INSTALL_FAKE_ROOT=1 bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}"
    # It should fail at phase_resolve because no profile and no local
    [ "$status" -ne 0 ]
    [[ "${output}" == *"--profile"* ]] || [[ "${output}" == *"--local"* ]]
}
