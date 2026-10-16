#!/usr/bin/env bats
# Tests for the install.sh hardening fixes (anolishq/anolis#166):
#   B4 — the runtime systemd unit's ExecStart honors --prefix (not hardcoded).
#   B6 — the health/summary port is read from runtime.yaml, not hardcoded 8080.
#   N1 — checksum verification uses `sha256sum -c` and names a bad file.
#
# These source install.sh (main() guarded) and exercise the pure helpers — no
# root, network, or systemd. The live install path is validated on real
# hardware (anolishq/anolis#138).

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
}

# ---------------------------------------------------------------------------
# B4 — runtime unit ExecStart follows ${PREFIX}
# ---------------------------------------------------------------------------

@test "B4: runtime unit ExecStart uses a custom --prefix" {
    PREFIX=/srv/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"ExecStart=/srv/anolis/bin/anolis-runtime --config /srv/anolis/config/runtime.yaml"* ]]
    [[ "${output}" != *"/opt/anolis"* ]]
}

@test "B4: default prefix still renders /opt/anolis (no regression)" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"ExecStart=/opt/anolis/bin/anolis-runtime --config /opt/anolis/config/runtime.yaml"* ]]
}

@test "B4: comment backticks stay literal (unquoted heredoc not command-substituted)" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *'`providers:`'* ]]
    [[ "${output}" == *'`Wants=`'* ]]
}

@test "B4: still one unit — User/Group anolis, no SupplementaryGroups, no per-provider Wants" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [[ "${output}" == *"User=anolis"* ]]
    [[ "${output}" == *"Group=anolis"* ]]
    # Device access is the platform's host prep (the user's own groups), not a
    # hardcoded list in the unit (anolis#318).
    ! grep -qE '^SupplementaryGroups=' <<< "${output}"
    # No active Wants= directive (the only occurrence is the escaped comment).
    ! grep -qE '^Wants=' <<< "${output}"
}

# ---------------------------------------------------------------------------
# B6 — port derived from the installed runtime config
# ---------------------------------------------------------------------------

@test "B6: _runtime_port reads http.port from runtime.yaml" {
    PREFIX="${BATS_TEST_TMPDIR}/p"
    mkdir -p "${PREFIX}/config"
    printf 'runtime:\n  id: x\nhttp:\n  bind: 127.0.0.1\n  port: 9137\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_port
    [ "$status" -eq 0 ]
    [ "${output}" = "9137" ]
}

@test "B6: _runtime_port ignores a port: under a non-http block" {
    PREFIX="${BATS_TEST_TMPDIR}/p"
    mkdir -p "${PREFIX}/config"
    printf 'http:\n  port: 8080\nproviders:\n  port: 5\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_port
    [ "${output}" = "8080" ]
}

@test "B6: _runtime_port falls back to DEFAULT_PORT when config missing" {
    PREFIX="${BATS_TEST_TMPDIR}/none"
    run _runtime_port
    [ "${output}" = "8080" ]
}

@test "B6: _runtime_port falls back when port is unparseable" {
    PREFIX="${BATS_TEST_TMPDIR}/p"
    mkdir -p "${PREFIX}/config"
    printf 'http:\n  bind: 127.0.0.1\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_port
    [ "${output}" = "8080" ]
}

# ---------------------------------------------------------------------------
# N1 — checksum verification via sha256sum -c
# ---------------------------------------------------------------------------

_make_bundle() {
    local b="$1"
    mkdir -p "${b}/bin" "${b}/config"
    printf 'binary\n' > "${b}/bin/anolis-runtime"
    printf 'http:\n  port: 8080\n' > "${b}/config/runtime.yaml"
    ( cd "${b}" && find . -type f -not -name 'checksums.sha256' | sort | xargs sha256sum > checksums.sha256 )
}

@test "N1: phase_verify passes a good bundle" {
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/good"
    _make_bundle "${BUNDLE_DIR}"
    run phase_verify
    [ "$status" -eq 0 ]
    [[ "${output}" == *"all checksums pass"* ]]
}

@test "N1: phase_verify fails and names a tampered file" {
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bad"
    _make_bundle "${BUNDLE_DIR}"
    printf 'TAMPERED\n' > "${BUNDLE_DIR}/bin/anolis-runtime"
    run phase_verify
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bin/anolis-runtime: FAILED"* ]]
    [[ "${output}" == *"Checksum verification failed"* ]]
}

@test "N1: phase_verify skips cleanly when no checksums file" {
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/empty"
    mkdir -p "${BUNDLE_DIR}"
    run phase_verify
    [ "$status" -eq 0 ]
    [[ "${output}" == *"skipping verification"* ]]
}

# ---------------------------------------------------------------------------
# #302 — the run registry's default data dir is relative; the unit must start
# the runtime somewhere the service user can write, and the install must
# notice when the registry did not come up.
# ---------------------------------------------------------------------------

@test "302: runtime unit sets WorkingDirectory to the prefix" {
    PREFIX=/srv/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    grep -qE '^WorkingDirectory=/srv/anolis$' <<< "${output}"
}

@test "302: run-registry probe passes on 200 and fails on 503" {
    # Shadow curl: -w '%{http_code}' is the only output the probe reads.
    curl() { printf '%s' "${FAKE_HTTP_CODE}"; }
    export -f curl

    FAKE_HTTP_CODE=200
    run _run_registry_available "http://localhost:8080"
    [ "$status" -eq 0 ]

    FAKE_HTTP_CODE=503
    run _run_registry_available "http://localhost:8080"
    [ "$status" -ne 0 ]
}

@test "302: run-registry probe fails when curl itself fails" {
    curl() { return 7; }
    export -f curl
    run _run_registry_available "http://localhost:8080"
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# #344: phase_health reports provider liveness from GET /v0/providers/health
# ---------------------------------------------------------------------------

# Two providers, as the runtime serialises them (nlohmann::json sorts keys, so
# a provider's devices precede its lifecycle_state and provider_id).
_providers_body() {
    printf '{"providers":[{"degraded":false,"device_count":1,"devices":[{"device_id":"d0","health":"OK"}],"lifecycle_state":"%s","provider_id":"alpha","state":"%s"},{"degraded":false,"device_count":0,"devices":[],"lifecycle_state":"%s","provider_id":"beta","state":"%s"}],"status":{"code":"OK","message":"ok"}}' "$@"
}

# curl answers by URL: runtime status, the run registry's HTTP code, and the
# provider health body in ${PROVIDERS_BODY_FILE}. sleep is a no-op so the
# health timeout elapses at once.
_stub_health_endpoints() {
    NO_START=0
    PROVIDERS_BODY_FILE="${BATS_TEST_TMPDIR}/providers.json"
    _runtime_port() { echo 8080; }
    sleep() { :; }
    curl() {
        local url="${*: -1}"
        case "${url}" in
            */v0/runtime/status) printf '{"version":"0.1.45"}' ;;
            */v0/runs) printf '200' ;;
            */v0/providers/health) cat "${PROVIDERS_BODY_FILE}" ;;
            *) return 7 ;;
        esac
    }
    export -f curl sleep _runtime_port
}

@test "344: phase_health reports every provider running" {
    _stub_health_endpoints
    _providers_body RUNNING AVAILABLE RECOVERING AVAILABLE > "${PROVIDERS_BODY_FILE}"

    HEALTH_FAILED=0
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers: 2 of 2 running"* ]]
    [[ "${output}" != *"⚠"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

@test "344: phase_health names a provider that is not running, and does not fail the install" {
    _stub_health_endpoints
    _providers_body RUNNING AVAILABLE CIRCUIT_OPEN UNAVAILABLE > "${PROVIDERS_BODY_FILE}"

    HEALTH_FAILED=0
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers: 1 of 2 running"* ]]
    [[ "${output}" == *"provider beta: CIRCUIT_OPEN"* ]]
    [[ "${output}" != *"provider alpha:"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

@test "344: phase_health waits for a provider that comes up during the timeout" {
    _stub_health_endpoints
    _providers_body RUNNING AVAILABLE RESTARTING UNAVAILABLE > "${PROVIDERS_BODY_FILE}"
    # The second sleep brings beta up.
    SLEEPS="${BATS_TEST_TMPDIR}/sleeps"
    sleep() {
        echo x >> "${SLEEPS}"
        if [[ $(wc -l < "${SLEEPS}") -ge 2 ]]; then
            _providers_body RUNNING AVAILABLE RUNNING AVAILABLE > "${PROVIDERS_BODY_FILE}"
        fi
    }

    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers: 2 of 2 running"* ]]
    [[ "${output}" != *"provider beta:"* ]]
}

@test "344: phase_health warns when the provider health endpoint does not answer" {
    _stub_health_endpoints
    rm -f "${PROVIDERS_BODY_FILE}"
    curl() {
        local url="${*: -1}"
        case "${url}" in
            */v0/runtime/status) printf '{"version":"0.1.45"}' ;;
            */v0/runs) printf '200' ;;
            *) return 22 ;;
        esac
    }

    HEALTH_FAILED=0
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"GET /v0/providers/health did not answer"* ]]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

# #363: a device whose provider-declared descriptor_tags hold a provider_id key
# must not shift which state is reported for which provider.
_providers_body_with_tag() {
    printf '{"providers":[{"devices":[{"descriptor_tags":{"provider_id":"tag0"},"device_id":"d0","health":"OK"}],"lifecycle_state":"%s","provider_id":"alpha"},{"devices":[],"lifecycle_state":"%s","provider_id":"beta"}],"status":{"code":"OK","message":"ok"}}' "$@"
}

_assert_each_provider_own_state() {
    _providers_body_with_tag RUNNING CIRCUIT_OPEN > "${PROVIDERS_BODY_FILE}"
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers: 1 of 2 running"* ]]
    [[ "${output}" == *"provider beta: CIRCUIT_OPEN"* ]]
    [[ "${output}" != *"tag0"* ]]
    [[ "${output}" != *"provider alpha:"* ]]
}

# An empty 200 body is no answer, not zero providers.
_assert_empty_body_unknown() {
    : > "${PROVIDERS_BODY_FILE}"
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers unknown"* ]]
    [[ "${output}" != *"of 0 running"* ]]
}

# An entry with no lifecycle_state counts as a provider that is not running;
# an entry that is not an object, or has no provider_id, is skipped.
_assert_missing_state_unknown() {
    printf '{"providers":[{"lifecycle_state":"RUNNING","provider_id":"alpha"},"junk",null,{"lifecycle_state":"RUNNING"},{"provider_id":"beta"}]}' > "${PROVIDERS_BODY_FILE}"
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers: 1 of 2 running"* ]]
    [[ "${output}" == *"provider beta: UNKNOWN"* ]]
    [[ "${output}" != *"null"* ]]
}

# Run <assertion> with jq (the host's PATH), with python3 and no jq, or with
# neither. The narrowed PATH applies in a subshell, so bats' own cleanup keeps
# its PATH.
_with_parser() {  # <jq|python3|none> <assertion>
    local parser="$1" assertion="$2" bin="${BATS_TEST_TMPDIR}/bin-$1" tool
    _stub_health_endpoints
    if [[ "${parser}" == jq ]]; then
        command -v jq >/dev/null || skip "jq not installed"
        "${assertion}"
        return
    fi
    mkdir -p "${bin}"
    local tools=(grep sed head awk cat wc)
    [[ "${parser}" == python3 ]] && tools+=(python3)
    for tool in "${tools[@]}"; do
        ln -s "$(command -v "${tool}")" "${bin}/${tool}"
    done
    (
        PATH="${bin}"
        [ -z "$(command -v jq)" ]
        "${assertion}"
    )
}

@test "363: provider states are read by key, not token order (jq)" {
    _with_parser jq _assert_each_provider_own_state
}

@test "363: provider states are read by key, not token order (python3, no jq)" {
    _with_parser python3 _assert_each_provider_own_state
}

@test "363: an empty providers/health body is reported as unknown (jq)" {
    _with_parser jq _assert_empty_body_unknown
}

@test "363: an empty providers/health body is reported as unknown (python3, no jq)" {
    _with_parser python3 _assert_empty_body_unknown
}

@test "363: a provider with no lifecycle_state is reported as UNKNOWN (jq)" {
    _with_parser jq _assert_missing_state_unknown
}

@test "363: a provider with no lifecycle_state is reported as UNKNOWN (python3, no jq)" {
    _with_parser python3 _assert_missing_state_unknown
}

# #388: an entry with an empty provider_id is skipped like one with none,
# so it neither counts nor shifts the id and state fields.
_assert_empty_id_skipped() {
    printf '{"providers":[{"lifecycle_state":"RUNNING","provider_id":"alpha"},{"lifecycle_state":"CIRCUIT_OPEN","provider_id":""}]}' > "${PROVIDERS_BODY_FILE}"
    phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
    run cat "${BATS_TEST_TMPDIR}/out"
    [[ "${output}" == *"providers: 1 of 1 running"* ]]
    [[ "${output}" != *"CIRCUIT_OPEN"* ]]
}

# #388: a body that is empty or not JSON came from a runtime that answered,
# so it is not reported as one that did not answer.
_assert_unreadable_body() {
    local body
    for body in '' '{"providers":[' 'not json'; do
        printf '%s' "${body}" > "${PROVIDERS_BODY_FILE}"
        phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
        run cat "${BATS_TEST_TMPDIR}/out"
        [[ "${output}" == *"providers unknown (GET /v0/providers/health answered, but the body was unreadable)"* ]] \
            || { echo "body '${body}': ${output}" >&2; return 1; }
        [[ "${output}" != *"did not answer"* ]] || return 1
    done
}

@test "388: a provider with an empty provider_id is skipped (jq)" {
    _with_parser jq _assert_empty_id_skipped
}

@test "388: a provider with an empty provider_id is skipped (python3, no jq)" {
    _with_parser python3 _assert_empty_id_skipped
}

@test "388: an unreadable providers/health body is not reported as no answer (jq)" {
    _with_parser jq _assert_unreadable_body
}

@test "388: an unreadable providers/health body is not reported as no answer (python3, no jq)" {
    _with_parser python3 _assert_unreadable_body
}

@test "363: with neither jq nor python3, providers are not checked and not polled" {
    SLEEPS="${BATS_TEST_TMPDIR}/sleeps"
    _assert_no_parser() {
        sleep() { echo x >> "${SLEEPS}"; }
        phase_health > "${BATS_TEST_TMPDIR}/out" 2>&1
        run cat "${BATS_TEST_TMPDIR}/out"
        [[ "${output}" == *"providers not checked (no JSON parser: install jq or python3)"* ]]
        [[ "${output}" != *"did not answer"* ]]
        [ ! -e "${SLEEPS}" ]
    }
    _with_parser none _assert_no_parser
}
