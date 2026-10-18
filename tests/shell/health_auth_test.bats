#!/usr/bin/env bats
# The install's health checks call the runtime over loopback, which needs the
# API token when http.auth_enabled is true. They read it from runtime.env (or
# http.auth_token in runtime.yaml, which the runtime prefers) and pass it to
# curl on stdin, so it never appears in curl's argv or in the install output.
#
# curl is a stub that records its argv and stdin; RUNTIME_ENV_FILE and PREFIX
# point into the test's temp dir. Nothing here touches /etc/anolis or the
# network.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
FAKE_TOKEN="fake-test-token-not-a-secret"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    # RUNTIME_ENV_FILE is readonly in install.sh; point it into the test dir.
    source <(sed "s|^readonly RUNTIME_ENV_FILE=.*|readonly RUNTIME_ENV_FILE=\"${BATS_TEST_TMPDIR}/runtime.env\"|" "${INSTALL_SH}")
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
    [[ "${RUNTIME_ENV_FILE}" == "${BATS_TEST_TMPDIR}/runtime.env" ]]

    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    mkdir -p "${PREFIX}/config"
    printf 'http:\n  enabled: true\n  port: 8080\n  auth_enabled: true\n' > "${PREFIX}/config/runtime.yaml"
    printf 'ANOLIS_API_TOKEN=%s\n' "${FAKE_TOKEN}" > "${RUNTIME_ENV_FILE}"

    NO_START=0
    CALLS="${BATS_TEST_TMPDIR}/calls"
    : > "${CALLS}"
    sleep() { :; }
    # Records "ARGV: <args>" and "STDIN: <line>" per call. A call with -H @-
    # reads its headers from stdin; any other call gets no stdin recorded.
    curl() {
        printf 'ARGV: %s\n' "$*" >> "${CALLS}"
        local a
        for a in "$@"; do
            if [[ "${a}" == "@-" ]]; then
                local line
                while IFS= read -r line; do printf 'STDIN: %s\n' "${line}" >> "${CALLS}"; done
            fi
        done
        local url="${*: -1}"
        case "${url}" in
            */v0/runtime/status) printf '{"version":"0.1.47"}' ;;
            */v0/runs) printf '200' ;;
            */v0/providers/health) printf '{"providers":[{"lifecycle_state":"RUNNING","provider_id":"alpha"}]}' ;;
            *) return 7 ;;
        esac
    }
    export -f curl sleep
}

# Every recorded runtime call carried the token on stdin, never in argv.
_assert_every_call_carries_token() {
    local argv stdin
    argv=$(grep -c '^ARGV: ' "${CALLS}")
    [ "${argv}" -ge 1 ]
    stdin=$(grep -cxF "STDIN: Authorization: Bearer ${FAKE_TOKEN}" "${CALLS}")
    [ "${stdin}" -eq "${argv}" ] || { cat "${CALLS}" >&2; return 1; }
    ! grep '^ARGV: ' "${CALLS}" | grep -qF "${FAKE_TOKEN}" || return 1
    [ "$(grep '^ARGV: ' "${CALLS}" | grep -c -- '-H @-')" -eq "${argv}" ]
}

@test "health: every runtime call in phase_health carries the token header" {
    HEALTH_FAILED=0
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"health: runtime responding (v0.1.47)"* ]]
    [[ "${output}" == *"health: run registry available"* ]]
    [[ "${output}" == *"providers: 1 of 1 running"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
    # status (x2), runs, providers/health
    grep -q '/v0/runtime/status$' "${CALLS}"
    grep -q '/v0/runs$' "${CALLS}"
    grep -q '/v0/providers/health$' "${CALLS}"
    _assert_every_call_carries_token
}

@test "health: phase_health never prints the token" {
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    ! grep -qF "${FAKE_TOKEN}" "${BATS_TEST_TMPDIR}/out" || return 1
}

@test "health: the run-registry probe carries the token header" {
    run _run_registry_available "http://localhost:8080"
    [ "$status" -eq 0 ]
    _assert_every_call_carries_token
}

@test "health: the provider-liveness probe carries the token header" {
    run _provider_states "http://localhost:8080"
    [ "$status" -eq 0 ]
    [[ "${output}" == "alpha RUNNING" ]]
    _assert_every_call_carries_token
}

@test "health: http.auth_token in runtime.yaml wins over runtime.env, as in the runtime" {
    printf 'http:\n  port: 8080\n  auth_enabled: true\n  auth_token: "yaml-fake-token"\n' > "${PREFIX}/config/runtime.yaml"
    run _run_registry_available "http://localhost:8080"
    [ "$status" -eq 0 ]
    grep -qxF "STDIN: Authorization: Bearer yaml-fake-token" "${CALLS}"
    ! grep -qF "${FAKE_TOKEN}" "${CALLS}" || return 1
}

@test "health: with no token anywhere, the probes run without a header" {
    rm -f "${RUNTIME_ENV_FILE}"
    run _run_registry_available "http://localhost:8080"
    [ "$status" -eq 0 ]
    ! grep -q -- '-H @-' "${CALLS}" || return 1
    ! grep -q '^STDIN: ' "${CALLS}" || return 1
}

# runtime.env is read the way systemd's EnvironmentFile reads it, so the probe
# sends the token the runtime was given.
_assert_probe_sends() {  # <expected token>
    run _run_registry_available "http://localhost:8080"
    [ "$status" -eq 0 ]
    grep -qxF "STDIN: Authorization: Bearer $1" "${CALLS}" || { cat "${CALLS}" >&2; return 1; }
    [ "$(grep -c '^STDIN: ' "${CALLS}")" -eq 1 ]
    ! grep '^ARGV: ' "${CALLS}" | grep -qF "$1" || return 1
}

@test "health: a duplicated ANOLIS_API_TOKEN in runtime.env is read last-wins, as systemd does" {
    printf 'ANOLIS_API_TOKEN=fake-old-token\nINFLUXDB_TOKEN=x\nANOLIS_API_TOKEN=fake-new-token\n' > "${RUNTIME_ENV_FILE}"
    _assert_probe_sends fake-new-token
}

@test "health: a double-quoted ANOLIS_API_TOKEN in runtime.env loses its quotes" {
    printf 'ANOLIS_API_TOKEN="fake-quoted-token"\n' > "${RUNTIME_ENV_FILE}"
    _assert_probe_sends fake-quoted-token
}

@test "health: a CRLF runtime.env does not put a CR into the header" {
    printf 'ANOLIS_API_TOKEN=fake-crlf-token\r\n' > "${RUNTIME_ENV_FILE}"
    _assert_probe_sends fake-crlf-token
}
