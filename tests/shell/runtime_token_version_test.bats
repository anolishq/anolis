#!/usr/bin/env bats
# A null http.auth_token depends on the runtime that reads it: releases after
# 0.11.0 treat it as unset, so ANOLIS_API_TOKEN applies, and releases up to
# 0.11.0 use the string "null". install.sh can act on either (an older pin or
# bundle, --rollback, --allow-downgrade), so its warning, summary and health
# probes follow the installed binary's version.
#
# anolis-runtime is a stub that reports a version, and curl is a stub that
# answers only for ACCEPTED_TOKEN. RUNTIME_ENV_FILE, SYSTEMD_DIR and PREFIX
# point into the test's temp dir. Nothing here touches the host or the network.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
ENV_TOKEN="per-device-token-not-a-secret"
NULL_VALUES=('' '~' 'null' 'Null' 'NULL' '# none' 'null  # none' '!!null null')

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    local t="${BATS_TEST_TMPDIR}"
    source <(sed -e "s|^readonly RUNTIME_ENV_FILE=.*|readonly RUNTIME_ENV_FILE=\"${t}/runtime.env\"|" \
        -e "s|^readonly SYSTEMD_DIR=.*|readonly SYSTEMD_DIR=\"${t}/systemd\"|" "${INSTALL_SH}")
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    PREFIX="${t}/prefix"
    mkdir -p "${PREFIX}/config" "${PREFIX}/bin" "${t}/systemd"
    printf 'ANOLIS_API_TOKEN=%s\n' "${ENV_TOKEN}" > "${RUNTIME_ENV_FILE}"

    NO_START=0
    HEALTH_FAILED=0
    ACCEPTED_TOKEN="${ENV_TOKEN}"
    sleep() { :; }
    # A runtime that answers only a request carrying ACCEPTED_TOKEN on stdin.
    curl() {
        local a line="" url="${*: -1}"
        for a in "$@"; do
            if [[ "${a}" == "@-" ]]; then
                IFS= read -r line
            fi
        done
        [[ "${line##* }" == "${ACCEPTED_TOKEN}" ]] || return 22
        case "${url}" in
            */v0/runtime/status) printf '{"version":"0.1.47"}' ;;
            */v0/runs) printf '200' ;;
            */v0/providers/health) printf '{"providers":[{"lifecycle_state":"RUNNING","provider_id":"alpha"}]}' ;;
            *) return 7 ;;
        esac
    }
    export -f curl sleep
}

_runtime_reports() {
    printf '#!/bin/sh\necho "anolis-runtime %s"\n' "$1" > "${PREFIX}/bin/anolis-runtime"
    chmod +x "${PREFIX}/bin/anolis-runtime"
}

# Write an installed runtime.yaml with <text> as the rest of the http block.
_token_cfg() {
    printf 'http:\n  enabled: true\n  port: 8080\n  auth_enabled: true\n%b\nproviders:\n  - id: p0\n' "$1" \
        > "${PREFIX}/config/runtime.yaml"
}

_assert_token_in_config_is_used() {  # <label>
    run _warn_config_api_token
    [[ "${output}" == *"sets http.auth_token"* ]] || { echo "no warning: $1"; return 1; }
}

_assert_null_is_unset() {  # <label>
    run _warn_config_api_token
    [ "${output}" == "" ] || { echo "warned: $1"; return 1; }
    [ "$(_runtime_api_token)" = "${ENV_TOKEN}" ] || { echo "token: $1"; return 1; }
}

# --- a runtime that uses the string ------------------------------------------

@test "older runtime: every null spelling is warned about and is the token in use" {
    local version v
    for version in 0.10.2 0.11.0; do
        _runtime_reports "${version}"
        for v in "${NULL_VALUES[@]}"; do
            _token_cfg "  auth_token: ${v}"
            _assert_token_in_config_is_used "${version} [${v}]"
            [ "$(_runtime_api_token)" = "null" ] || { echo "token: ${version} [${v}]"; return 1; }
        done
    done
}

@test "older runtime: versions compare by number, not as text" {
    local version
    for version in 0.9.99 0.10.12 0.2.0; do
        _runtime_reports "${version}"
        _token_cfg "  auth_token: null"
        _assert_token_in_config_is_used "${version}"
    done
}

@test "a runtime version that cannot be read counts as older" {
    _token_cfg "  auth_token: null"
    rm -f "${PREFIX}/bin/anolis-runtime"
    _assert_token_in_config_is_used "no binary"
    printf '#!/bin/sh\necho "anolis-runtime"\n' > "${PREFIX}/bin/anolis-runtime"
    chmod +x "${PREFIX}/bin/anolis-runtime"
    _assert_token_in_config_is_used "no version in the output"
    printf '#!/bin/sh\nexit 1\n' > "${PREFIX}/bin/anolis-runtime"
    _assert_token_in_config_is_used "a binary that fails"
}

@test "older runtime: phase_runtime_auth warns on an upgrade that keeps a null token" {
    _runtime_reports 0.10.2
    _token_cfg "  auth_token: null"
    # Nothing here may touch the host: the primitives are stubs, and token
    # generation fails, so the function stops before it writes runtime.env.
    install() { :; }
    chmod() { :; }
    chown() { :; }
    _generate_api_token() { return 1; }
    run phase_runtime_auth
    [[ "${output}" == *"sets http.auth_token"* ]]
}

@test "older runtime: the summary names the config, not runtime.env" {
    _runtime_reports 0.10.2
    _token_cfg "  auth_token: null"
    run phase_summary
    [[ "${output}" == *"API token: the http.auth_token in"* ]]
    [[ "${output}" != *"API token: ${RUNTIME_ENV_FILE}"* ]]
}

@test "older runtime: phase_health finds the runtime with the token it uses" {
    _runtime_reports 0.10.2
    _token_cfg "  auth_token: null"
    ACCEPTED_TOKEN="null"
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"health: runtime responding"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

@test "older runtime: a rollback with a null token warns and finds the runtime" {
    _runtime_reports 0.10.2
    _token_cfg "  auth_token: null"
    ACCEPTED_TOKEN="null"
    mkdir -p "${PREFIX}/.prev"
    cp "${PREFIX}/bin/anolis-runtime" "${PREFIX}/.prev/anolis-runtime"
    touch "${SYSTEMD_DIR}/anolis-runtime.service"
    systemctl() { :; }
    chown() { :; }
    replace_binaries() { :; }
    do_rollback > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"sets http.auth_token"* ]]
    [[ "${output}" == *"health: runtime responding"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

@test "older runtime: the install preflight does not refuse a null token" {
    _token_cfg "  auth_token: null"
    run preflight_runtime_config "${PREFIX}/config/runtime.yaml"
    [ "$status" -eq 0 ]
}

# --- a runtime that treats null as unset --------------------------------------

@test "runtime after 0.11.0: every null spelling is silent and runtime.env is used" {
    local version v
    for version in 0.11.1 0.12.0 1.0.0; do
        _runtime_reports "${version}"
        for v in "${NULL_VALUES[@]}"; do
            _token_cfg "  auth_token: ${v}"
            _assert_null_is_unset "${version} [${v}]"
        done
    done
}

@test "runtime after 0.11.0: phase_health finds the runtime with runtime.env's token" {
    _runtime_reports 0.11.1
    _token_cfg "  auth_token: null"
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"health: runtime responding"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

# --- what counts as null, on any runtime ---------------------------------------

@test "a value on the line after the key is a token, on any runtime" {
    local version v
    for version in 0.10.2 0.11.1; do
        _runtime_reports "${version}"
        for v in '  auth_token:\n    shared-literal-token' \
            '  auth_token:\n    "shared-literal-token"' \
            '  auth_token: # a comment\n    shared-literal-token' \
            '  auth_token:\n    # a comment\n\n    shared-literal-token' \
            '  auth_token:\r\n    shared-literal-token\r'; do
            _token_cfg "${v}"
            _assert_token_in_config_is_used "${version} [${v}]"
        done
    done
}

@test "the line after the key decides only when it is the key's value" {
    _runtime_reports 0.11.1
    local v
    for v in '  auth_token:\n    ~' '  auth_token:\n    null  # none' \
        '  auth_token:\n  bind: 127.0.0.1' '  auth_token:\n\n  # a comment\n  bind: 127.0.0.1'; do
        _token_cfg "${v}"
        _assert_null_is_unset "[${v}]"
    done
    _token_cfg '  auth_token:\n    ""'
    _assert_null_is_unset "a quoted empty string on the next line"
}

@test "only the !!null tag itself marks a null" {
    _runtime_reports 0.11.1
    _token_cfg '  auth_token: !!nullable shared-literal-token'
    _assert_token_in_config_is_used "a longer tag"
}

@test "a token in a key the block reader accepts is still used by the probes" {
    local v
    for v in '  auth_token : shared-literal-token' '  "auth_token": shared-literal-token'; do
        _token_cfg "${v}"
        [ "$(_runtime_api_token)" = "shared-literal-token" ] || { echo "token: ${v}"; return 1; }
    done
}
