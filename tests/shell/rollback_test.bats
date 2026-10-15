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
# Naming the rollback target from the metadata phase_backup records (#315)
# ===========================================================================

_setup_prev_with_meta() {
    mkdir -p "${PREFIX_DIR}/bin" "${PREFIX_DIR}/.prev/meta"
    printf 'new-runtime' > "${PREFIX_DIR}/bin/anolis-runtime"
    printf 'old-runtime' > "${PREFIX_DIR}/.prev/anolis-runtime"
    printf 'anolis-runtime 0.1.39\n' > "${PREFIX_DIR}/.prev/meta/runtime-version"
    printf '{}\n' > "${PREFIX_DIR}/.prev/meta/manifest.json"
}

@test "--rollback names the version before restoring it (#315)" {
    _setup_prev_with_meta

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"rollback target: anolis-runtime 0.1.39"* ]]
    [[ "${output}" == *"${PREFIX_DIR}/.prev/meta/manifest.json"* ]]
    # Named BEFORE the restore, not after.
    [[ "${output}" == *"rollback target: anolis-runtime 0.1.39"*"restored binaries from .prev/"* ]]
    [ "$(cat "${PREFIX_DIR}/bin/anolis-runtime")" = "old-runtime" ]
    # The metadata is not a binary: it must not be restored into bin/.
    [ ! -e "${PREFIX_DIR}/bin/meta" ]
    [ ! -e "${PREFIX_DIR}/bin/runtime-version" ]
}

@test "--dry-run --rollback names the version and restores nothing (#315)" {
    _setup_prev_with_meta

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"rollback target: anolis-runtime 0.1.39"* ]]
    [[ "${output}" == *"would roll back"* ]]
    [ "$(cat "${PREFIX_DIR}/bin/anolis-runtime")" = "new-runtime" ]
}

@test "--rollback of a backup without metadata says the version is unknown (#315)" {
    mkdir -p "${PREFIX_DIR}/bin" "${PREFIX_DIR}/.prev"
    printf 'old-runtime' > "${PREFIX_DIR}/.prev/anolis-runtime"

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --rollback --prefix "${PREFIX_DIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"rollback target: version unknown"* ]]
    [ "$(cat "${PREFIX_DIR}/bin/anolis-runtime")" = "old-runtime" ]
}

# ===========================================================================
# phase_backup: same-version re-run must not clobber the rollback point (#184)
# ===========================================================================

_setup_backup_dirs() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

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

@test "phase_backup records the saved runtime version and manifest in .prev/meta (#315)" {
    _setup_backup_dirs
    printf '#!/bin/sh\necho "anolis-runtime 0.1.39"\n' > "${PREFIX}/bin/anolis-runtime"
    chmod +x "${PREFIX}/bin/anolis-runtime"
    printf '{"version": "0.1.39"}\n' > "${PREFIX}/manifest.json"
    printf 'runtime-v3\n' > "${BUNDLE_DIR}/bin/anolis-runtime"

    run phase_backup
    [ "$status" -eq 0 ]
    [ "$(cat "${PREFIX}/.prev/meta/runtime-version")" = "anolis-runtime 0.1.39" ]
    cmp -s "${PREFIX}/manifest.json" "${PREFIX}/.prev/meta/manifest.json"
}

@test "phase_backup drops stale metadata when the version cannot be read (#315)" {
    _setup_backup_dirs
    # .prev/meta describes an older backup; the runtime being saved now is not
    # executable, so its version is unknown. A stale name would be a wrong one.
    mkdir -p "${PREFIX}/.prev/meta"
    printf 'anolis-runtime 0.1.1\n' > "${PREFIX}/.prev/meta/runtime-version"
    printf '{}\n' > "${PREFIX}/.prev/meta/manifest.json"
    printf 'runtime-v3\n' > "${BUNDLE_DIR}/bin/anolis-runtime"

    run phase_backup
    [ "$status" -eq 0 ]
    [[ "${output}" == *"could not read the runtime version"* ]]
    [ ! -e "${PREFIX}/.prev/meta/runtime-version" ]
    [ ! -e "${PREFIX}/.prev/meta/manifest.json" ]
}

# ===========================================================================
# --rollback restores the manifest of the install it rolls back to (#345)
# ===========================================================================

# Source install.sh and point it at a prefix under BATS_TEST_TMPDIR. chown and
# systemctl are recording stubs, so nothing outside the temp prefix is touched.
_setup_install_cycle() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    STUB_LOG="${BATS_TEST_TMPDIR}/stub.log"
    chown() { echo "chown $*" >> "${STUB_LOG}"; }
    systemctl() { echo "systemctl $*" >> "${STUB_LOG}"; }
}

# Install a bundle whose runtime reports <version> and whose manifest names it.
_install_version() {
    local version="$1"
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bundle-${version}"
    mkdir -p "${BUNDLE_DIR}/bin"
    printf '#!/bin/sh\necho "anolis-runtime %s"\n' "${version}" > "${BUNDLE_DIR}/bin/anolis-runtime"
    chmod +x "${BUNDLE_DIR}/bin/anolis-runtime"
    printf '{\n  "version": "%s"\n}\n' "${version}" > "${BUNDLE_DIR}/manifest.json"

    phase_directories
    phase_backup
    phase_install_binaries
    phase_manifest
}

@test "--rollback restores the manifest of the rolled-back install (#345)" {
    _setup_install_cycle
    _install_version 0.1.43
    _install_version 0.1.44
    grep -q '"version": "0.1.44"' "${PREFIX}/manifest.json"

    run do_rollback
    [ "$status" -eq 0 ]
    [ "$("${PREFIX}/bin/anolis-runtime")" = "anolis-runtime 0.1.43" ]
    cmp -s "${BATS_TEST_TMPDIR}/bundle-0.1.43/manifest.json" "${PREFIX}/manifest.json"
    [[ "${output}" == *"restored manifest.json from .prev/"* ]]
    grep -qx "chown ${ANOLIS_USER}:${ANOLIS_USER} ${PREFIX}/manifest.json" "${STUB_LOG}"

    # The next upgrade's backup then records a manifest that agrees with the
    # runtime version it saves.
    _install_version 0.1.44
    [ "$(cat "${PREFIX}/.prev/meta/runtime-version")" = "anolis-runtime 0.1.43" ]
    grep -q '"version": "0.1.43"' "${PREFIX}/.prev/meta/manifest.json"
}

@test "--rollback of a backup with no saved manifest warns that manifest.json is stale (#345)" {
    _setup_install_cycle
    _install_version 0.1.43
    _install_version 0.1.44
    # A backup taken before .prev/meta existed (#334) has no saved manifest.
    rm -rf "${PREFIX}/.prev/meta"

    run do_rollback
    [ "$status" -eq 0 ]
    [ "$("${PREFIX}/bin/anolis-runtime")" = "anolis-runtime 0.1.43" ]
    [[ "${output}" == *"manifest.json still describes the newer install"* ]]
    grep -q '"version": "0.1.44"' "${PREFIX}/manifest.json"
}
