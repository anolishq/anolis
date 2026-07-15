#!/usr/bin/env bats
# Tests for install.sh --rollback (restore .prev binaries).
# Run without root, network, or systemd: --prefix points at a temp dir and
# there is no systemd unit, so the restart branch degrades to a warning.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
RUN_AS_ROOT="${BATS_TEST_DIRNAME}/helpers/run_as_root.sh"

setup() {
    PREFIX_DIR=$(mktemp -d)
}

teardown() {
    rm -rf "${PREFIX_DIR}"
}

@test "--help documents --rollback" {
    run bash "${INSTALL_SH}" --help
    [ "$status" -eq 0 ]
    [[ "${output}" == *"--rollback"* ]]
}

@test "--rollback without root fails" {
    if [ "$EUID" -eq 0 ]; then
        skip "running as root"
    fi
    run bash "${INSTALL_SH}" --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"must be run as root"* ]]
}

@test "--rollback fails clearly when there is no .prev backup" {
    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"nothing to roll back"* ]]
    [[ "${output}" == *".prev/anolis-runtime not found"* ]]
}

@test "--rollback restores binaries from .prev" {
    mkdir -p "${PREFIX_DIR}/bin" "${PREFIX_DIR}/.prev"
    printf 'new-runtime' > "${PREFIX_DIR}/bin/anolis-runtime"
    printf 'new-provider' > "${PREFIX_DIR}/bin/anolis-provider-foo"
    printf 'old-runtime' > "${PREFIX_DIR}/.prev/anolis-runtime"
    printf 'old-provider' > "${PREFIX_DIR}/.prev/anolis-provider-foo"

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"restored binaries from .prev/"* ]]
    [[ "${output}" == *"rollback complete"* ]]

    [ "$(cat "${PREFIX_DIR}/bin/anolis-runtime")" = "old-runtime" ]
    [ "$(cat "${PREFIX_DIR}/bin/anolis-provider-foo")" = "old-provider" ]
    [ -x "${PREFIX_DIR}/bin/anolis-runtime" ]
}

@test "--rollback warns when no systemd unit exists (no restart attempted)" {
    mkdir -p "${PREFIX_DIR}/bin" "${PREFIX_DIR}/.prev"
    printf 'old-runtime' > "${PREFIX_DIR}/.prev/anolis-runtime"

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"restart the runtime manually"* ]]
}

@test "--dry-run --rollback prints intent and changes nothing" {
    mkdir -p "${PREFIX_DIR}/bin" "${PREFIX_DIR}/.prev"
    printf 'new-runtime' > "${PREFIX_DIR}/bin/anolis-runtime"
    printf 'old-runtime' > "${PREFIX_DIR}/.prev/anolis-runtime"

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"would roll back"* ]]
    [ "$(cat "${PREFIX_DIR}/bin/anolis-runtime")" = "new-runtime" ]
}

# ===========================================================================
# phase_backup: same-version re-run must not clobber the rollback point (#184)
# ===========================================================================

_setup_backup_dirs() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +e +u +o pipefail

    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bundle"
    mkdir -p "${PREFIX}/bin" "${PREFIX}/.prev" "${BUNDLE_DIR}/bin"

    # Installed: v2 binaries. .prev: the v1 binaries from the last upgrade —
    # the rollback point that must survive a same-version re-run.
    printf 'runtime-v2\n' > "${PREFIX}/bin/anolis-runtime"
    printf 'bread-v2\n'   > "${PREFIX}/bin/anolis-provider-bread"
    printf 'runtime-v1\n' > "${PREFIX}/.prev/anolis-runtime"
    printf 'bread-v1\n'   > "${PREFIX}/.prev/anolis-provider-bread"
}

@test "phase_backup: identical incoming payload preserves .prev (#184)" {
    _setup_backup_dirs
    # Incoming bundle == installed binaries (idempotent re-run).
    cp "${PREFIX}/bin/anolis-runtime" "${BUNDLE_DIR}/bin/anolis-runtime"
    cp "${PREFIX}/bin/anolis-provider-bread" "${BUNDLE_DIR}/bin/anolis-provider-bread"

    run phase_backup
    [ "$status" -eq 0 ]
    [[ "${output}" == *".prev preserved"* ]]
    # The v1 rollback point is untouched.
    [ "$(cat "${PREFIX}/.prev/anolis-runtime")" = "runtime-v1" ]
    [ "$(cat "${PREFIX}/.prev/anolis-provider-bread")" = "bread-v1" ]
}

@test "phase_backup: differing incoming payload still snapshots to .prev" {
    _setup_backup_dirs
    # Incoming bundle carries a new version.
    printf 'runtime-v3\n' > "${BUNDLE_DIR}/bin/anolis-runtime"
    printf 'bread-v3\n'   > "${BUNDLE_DIR}/bin/anolis-provider-bread"

    run phase_backup
    [ "$status" -eq 0 ]
    [[ "${output}" == *"saved to .prev"* ]]
    # .prev now holds the currently-installed (v2) binaries.
    [ "$(cat "${PREFIX}/.prev/anolis-runtime")" = "runtime-v2" ]
}

@test "phase_backup: new binary in the bundle forces a snapshot" {
    _setup_backup_dirs
    # Same payload for existing files, plus one binary not installed yet.
    cp "${PREFIX}/bin/anolis-runtime" "${BUNDLE_DIR}/bin/anolis-runtime"
    cp "${PREFIX}/bin/anolis-provider-bread" "${BUNDLE_DIR}/bin/anolis-provider-bread"
    printf 'ezo-v1\n' > "${BUNDLE_DIR}/bin/anolis-provider-ezo"

    run phase_backup
    [ "$status" -eq 0 ]
    [[ "${output}" == *"saved to .prev"* ]]
}
