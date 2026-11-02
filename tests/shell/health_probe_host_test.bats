#!/usr/bin/env bats
# The health probes carry the API token, so they must reach the address the
# runtime itself listens on, which no other local user can bind while the
# runtime holds it. `http://localhost` resolves to ::1 first on Debian, and a
# runtime bound to an IPv4 address leaves [::1]:<port> free for anyone to
# listen on and collect the token (#534).
#
# curl is a stub that records its argv; PREFIX and RUNTIME_ENV_FILE point into
# the test's temp dir. Nothing here touches /etc/anolis or the network.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source <(sed "s|^readonly RUNTIME_ENV_FILE=.*|readonly RUNTIME_ENV_FILE=\"${BATS_TEST_TMPDIR}/runtime.env\"|" "${INSTALL_SH}")
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    mkdir -p "${PREFIX}/config"
    printf 'ANOLIS_API_TOKEN=fake-test-token-not-a-secret\n' > "${RUNTIME_ENV_FILE}"

    NO_START=0
    CALLS="${BATS_TEST_TMPDIR}/calls"
    : > "${CALLS}"
    sleep() { :; }
    curl() {
        printf 'ARGV: %s\n' "$*" >> "${CALLS}"
        local a line
        for a in "$@"; do
            [[ "${a}" == "@-" ]] && while IFS= read -r line; do :; done
        done
        case "${*: -1}" in
            */v0/runtime/status) printf '{"version":"0.1.47"}' ;;
            */v0/runs) printf '200' ;;
            */v0/providers/health) printf '{"providers":[{"lifecycle_state":"RUNNING","provider_id":"alpha"}]}' ;;
            *) return 7 ;;
        esac
    }
    export -f curl sleep
}

# Run phase_health against a runtime.yaml whose http block has `bind_line`
# (empty for no bind key), and print every URL the probes called.
_probe_urls() {
    local bind_line="$1"
    {
        printf 'http:\n  enabled: true\n  port: 8080\n  auth_enabled: true\n'
        [[ -n "${bind_line}" ]] && printf '  %s\n' "${bind_line}"
    } > "${PREFIX}/config/runtime.yaml"
    HEALTH_FAILED=0
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    [ "${HEALTH_FAILED}" -eq 0 ] || { cat "${BATS_TEST_TMPDIR}/out" >&2; return 1; }
    grep '^ARGV: ' "${CALLS}" | awk '{ print $NF }'
}

# Every probe went to `base`, and there were status, runs and providers calls.
_assert_all_probes_at() {
    local base="$1" urls="$2" url
    [ -n "${urls}" ]
    while IFS= read -r url; do
        [[ "${url}" == "${base}/v0/"* ]] || { echo "probe went to ${url}, not ${base}" >&2; return 1; }
    done <<< "${urls}"
    grep -q '/v0/runtime/status$' <<< "${urls}"
    grep -q '/v0/runs$' <<< "${urls}"
    grep -q '/v0/providers/health$' <<< "${urls}"
}

@test "health probe: no bind key probes 127.0.0.1, never localhost" {
    urls=$(_probe_urls "")
    _assert_all_probes_at "http://127.0.0.1:8080" "${urls}"
    ! grep -q localhost <<< "${urls}" || return 1
}

@test "health probe: bind 127.0.0.1 probes 127.0.0.1" {
    urls=$(_probe_urls "bind: 127.0.0.1")
    _assert_all_probes_at "http://127.0.0.1:8080" "${urls}"
}

@test "health probe: bind 0.0.0.0 (a rendered bundle) probes 127.0.0.1" {
    urls=$(_probe_urls "bind: 0.0.0.0")
    _assert_all_probes_at "http://127.0.0.1:8080" "${urls}"
}

@test "health probe: another 127/8 address is probed as itself" {
    urls=$(_probe_urls "bind: 127.0.0.2")
    _assert_all_probes_at "http://127.0.0.2:8080" "${urls}"
}

@test "health probe: bind ::1 probes [::1]" {
    urls=$(_probe_urls 'bind: "::1"')
    _assert_all_probes_at "http://[::1]:8080" "${urls}"
}

@test "health probe: bind :: probes [::1]" {
    urls=$(_probe_urls "bind: '::'")
    _assert_all_probes_at "http://[::1]:8080" "${urls}"
}

@test "health probe: bind localhost probes [::1] when IPv6 is up" {
    _ipv6_available() { return 0; }
    urls=$(_probe_urls "bind: localhost")
    _assert_all_probes_at "http://[::1]:8080" "${urls}"
}

@test "health probe: bind localhost probes 127.0.0.1 when IPv6 is down" {
    _ipv6_available() { return 1; }
    urls=$(_probe_urls "bind: localhost")
    _assert_all_probes_at "http://127.0.0.1:8080" "${urls}"
}

@test "health probe: a specific address is probed as itself" {
    urls=$(_probe_urls "bind: 192.168.4.20")
    _assert_all_probes_at "http://192.168.4.20:8080" "${urls}"
    urls=$(: > "${CALLS}"; _probe_urls "bind: fd00::20")
    _assert_all_probes_at "http://[fd00::20]:8080" "${urls}"
}

@test "health probe: no runtime call in install.sh is built on localhost" {
    ! grep -nE '_runtime_curl[^#]*localhost|"http://localhost:\$\{port\}' "${INSTALL_SH}" || return 1
}
