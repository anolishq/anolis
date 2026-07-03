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
