#!/usr/bin/env bats
# `install.sh --uninstall` removes the secrets it and its companions keep under
# /etc/anolis, which lie outside the prefix: the runtime's runtime.env, the
# telemetry and observability env files, and the workbench service's copy of
# the runtime token.
#
# Every path constant under /etc/anolis and SYSTEMD_DIR is pointed into this
# test's temp dir before install.sh is sourced. rm, rmdir, systemctl, mv,
# chown and chmod are stubs: each call is recorded, and an rm is carried out
# only when every operand lies inside the fake /etc. The prefix does not
# exist, so nothing else is removed.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    ETC="${BATS_TEST_TMPDIR}/etc/anolis"
    mkdir -p "${ETC}" "${BATS_TEST_TMPDIR}/systemd"
    # shellcheck disable=SC1090
    source <(sed -E \
        -e "s|^readonly ([A-Z_]+)=\"/etc/anolis/|readonly \\1=\"${ETC}/|" \
        -e "s|^readonly SYSTEMD_DIR=.*|readonly SYSTEMD_DIR=\"${BATS_TEST_TMPDIR}/systemd\"|" \
        "${INSTALL_SH}")
    set +u
    # The redirect must have reached every constant this test relies on.
    [[ "${RUNTIME_ENV_FILE}" == "${ETC}/runtime.env" ]]
    [[ "${WORKBENCH_TOKEN_FILE}" == "${ETC}/workbench-runtime-token" ]]

    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"
    rm() {
        printf 'rm %s\n' "$*" >>"${CALLS}"
        local a
        for a in "$@"; do
            [[ "${a}" == -* ]] && continue
            [[ "${a}" == "${ETC}"/?* && "${a}" != *..* ]] || return 0
        done
        command rm "$@"
    }
    local name
    for name in rmdir systemctl mv chown chmod; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    _obs_provisioned() { return 1; }
    PREFIX="${BATS_TEST_TMPDIR}/no-such-prefix"
}

teardown() {
    unset -f rm rmdir systemctl mv chown chmod
}

@test "uninstall removes the workbench's runtime token copy with the other secrets" {
    printf 'fake-test-token-not-a-secret\n' >"${WORKBENCH_TOKEN_FILE}"
    printf 'ANOLIS_API_TOKEN=fake-test-token-not-a-secret\n' >"${RUNTIME_ENV_FILE}"
    run do_uninstall
    [ "$status" -eq 0 ]
    [ ! -e "${WORKBENCH_TOKEN_FILE}" ]
    [ ! -e "${RUNTIME_ENV_FILE}" ]
    [[ "${output}" == *"removed ${WORKBENCH_TOKEN_FILE}"* ]]
    [[ "${output}" != *"fake-test-token-not-a-secret"* ]]
}

@test "uninstall is fine when the workbench token copy is absent" {
    printf 'ANOLIS_API_TOKEN=fake-test-token-not-a-secret\n' >"${RUNTIME_ENV_FILE}"
    run do_uninstall
    [ "$status" -eq 0 ]
    [ ! -e "${RUNTIME_ENV_FILE}" ]
    [[ "${output}" != *"workbench-runtime-token"* ]]
    ! grep -qF "workbench-runtime-token" "${CALLS}" || return 1
}
