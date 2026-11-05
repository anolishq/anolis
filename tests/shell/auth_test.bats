#!/usr/bin/env bats
# Tests for the LAN-bind auth provisioning and the config preflight
# (anolishq/anolis#172), plus the /etc/hosts hostname fix.
#
# Background: install.sh rewrote `bind: 127.0.0.1` to `0.0.0.0` without enabling
# auth, producing a config the runtime is guaranteed to reject at startup. The
# runtime crash-looped while the install reported success. These tests pin the
# guard that makes that unrepresentable.
#
# Source install.sh (main() guarded) and exercise the pure helpers — no root,
# network, or systemd. The live install path is validated on real hardware
# (anolishq/anolis#138).

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
    TMP="${BATS_TEST_TMPDIR}"
}

# Write a runtime.yaml whose http block carries the given lines.
_cfg() {
    local path="${TMP}/runtime.yaml"
    {
        echo "runtime:"
        echo "  name: test"
        echo "http:"
        echo "  enabled: true"
        printf '  %s\n' "$@"
        echo "providers:"
        echo "  - id: p0"
    } > "${path}"
    printf '%s' "${path}"
}

# ---------------------------------------------------------------------------
# preflight_runtime_config — mirrors the runtime's startup guard
# ---------------------------------------------------------------------------

@test "preflight: refuses a non-loopback bind with auth disabled" {
    cfg=$(_cfg "bind: 0.0.0.0" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"non-loopback but authentication is disabled"* ]]
    [[ "${output}" == *"auth_enabled"* ]]
}

@test "preflight: refuses a routable bind with auth disabled" {
    cfg=$(_cfg "bind: 192.168.1.50" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"192.168.1.50"* ]]
}

@test "preflight: accepts a non-loopback bind when auth is enabled" {
    cfg=$(_cfg "bind: 0.0.0.0" "auth_enabled: true" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "preflight: accepts a non-loopback bind with the explicit insecure override" {
    cfg=$(_cfg "bind: 0.0.0.0" "allow_insecure_bind: true" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "preflight: accepts a loopback bind with auth disabled" {
    cfg=$(_cfg "bind: 127.0.0.1" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "preflight: an absent bind defaults to loopback and is accepted" {
    cfg=$(_cfg "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# _http_value — scoped to the top-level http: block
# ---------------------------------------------------------------------------

@test "_http_value: reads a key from the http block" {
    cfg=$(_cfg "bind: 0.0.0.0" "port: 9090" "auth_enabled: true")
    [ "$(_http_value "${cfg}" bind)" = "0.0.0.0" ]
    [ "$(_http_value "${cfg}" port)" = "9090" ]
    [ "$(_http_value "${cfg}" auth_enabled)" = "true" ]
}

@test "_http_value: is empty for an absent key" {
    cfg=$(_cfg "bind: 0.0.0.0")
    [ -z "$(_http_value "${cfg}" auth_enabled)" ]
}

@test "_http_value: does not read a same-named key from another block" {
    # A `bind:` under telemetry must not be mistaken for http.bind — otherwise the
    # preflight could clear a config that actually exposes an unauthenticated API.
    cat > "${TMP}/other.yaml" <<'YAML'
http:
  enabled: true
  bind: 0.0.0.0
telemetry:
  enabled: true
  auth_enabled: true
YAML
    [ "$(_http_value "${TMP}/other.yaml" bind)" = "0.0.0.0" ]
    [ -z "$(_http_value "${TMP}/other.yaml" auth_enabled)" ]

    run preflight_runtime_config "${TMP}/other.yaml"
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# API token
# ---------------------------------------------------------------------------

@test "_generate_api_token: emits 64 hex chars" {
    run _generate_api_token
    [ "$status" -eq 0 ]
    [[ "${output}" =~ ^[0-9a-f]{64}$ ]]
}

@test "_generate_api_token: successive tokens differ" {
    a=$(_generate_api_token)
    b=$(_generate_api_token)
    [ "${a}" != "${b}" ]
}

@test "unit: injects the API token via EnvironmentFile, not the config" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"EnvironmentFile=/etc/anolis/runtime.env"* ]]
    # The secret itself must never be inlined into the unit.
    [[ "${output}" != *"ANOLIS_API_TOKEN="* ]]
}

# #529: a literal http.auth_token in the installed runtime.yaml shadows the
# per-device token. The install warns, naming the file and never the value.
_installed_cfg() {
    PREFIX="${TMP}/prefix"
    mkdir -p "${PREFIX}/config"
    cp "$(_cfg "$@")" "${PREFIX}/config/runtime.yaml"
}

@test "529: a literal auth_token in the installed config is warned about, without its value" {
    _installed_cfg "auth_enabled: true" 'auth_token: "baked-token-xyz"'
    run _warn_config_api_token
    [ "$status" -eq 0 ]
    [[ "${output}" == *"${PREFIX}/config/runtime.yaml sets http.auth_token"* ]]
    [[ "${output}" == *"/etc/anolis/runtime.env"* ]]
    [[ "${output}" != *"baked-token-xyz"* ]]
}

@test "529: a null auth_token is warned about: the runtime reads it as the token \"null\"" {
    local v
    for v in 'auth_token:' 'auth_token: ~' 'auth_token: null' 'auth_token:   # none'; do
        _installed_cfg "auth_enabled: true" "${v}"
        run _warn_config_api_token
        [[ "${output}" == *"sets http.auth_token"* ]] || { echo "no warning: ${v}"; return 1; }
    done
    _installed_cfg "auth_enabled: true" "auth_token:"
    run phase_summary
    [[ "${output}" == *"API token: the http.auth_token in ${PREFIX}/config/runtime.yaml"* ]]
}

@test "529: no warning when the installed config sets no token, or a quoted empty one" {
    _installed_cfg "auth_enabled: true"
    run _warn_config_api_token
    [ "${output}" == "" ]
    _installed_cfg "auth_enabled: true" 'auth_token: ""'
    run _warn_config_api_token
    [ "${output}" == "" ]
    _installed_cfg "auth_enabled: true" "auth_token: ''  # off"
    run _warn_config_api_token
    [ "${output}" == "" ]
}

@test "529: phase_runtime_auth warns before it provisions the token" {
    _installed_cfg "auth_enabled: true" "auth_token: baked-token-xyz"
    # Nothing here may touch the host: the primitives are stubs, and token
    # generation fails, so the function stops before it writes runtime.env.
    install() { :; }
    chmod() { :; }
    chown() { :; }
    _generate_api_token() { return 1; }
    run phase_runtime_auth
    [[ "${output}" == *"sets http.auth_token"* ]]
    [[ "${output}" != *"baked-token-xyz"* ]]
}

@test "529: the summary names the config token as the one in use, without its value" {
    _installed_cfg "auth_enabled: true" "auth_token: baked-token-xyz"
    run phase_summary
    [[ "${output}" == *"API token: the http.auth_token in ${PREFIX}/config/runtime.yaml"* ]]
    [[ "${output}" != *"baked-token-xyz"* ]]
}

# ---------------------------------------------------------------------------
# /etc/hosts — a renamed host must still resolve its own name
# ---------------------------------------------------------------------------

@test "hosts: rewrites the existing 127.0.1.1 entry" {
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n127.0.1.1\traspberrypi\n' > "${ETC_HOSTS}"

    _update_etc_hosts "anolis-f95d50b1"

    grep -qE '^127\.0\.1\.1[[:space:]]+anolis-f95d50b1$' "${ETC_HOSTS}"
    ! grep -q 'raspberrypi' "${ETC_HOSTS}"
    # The loopback entry is untouched.
    grep -qE '^127\.0\.0\.1[[:space:]]+localhost$' "${ETC_HOSTS}"
}

@test "hosts: appends when no 127.0.1.1 entry exists" {
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n' > "${ETC_HOSTS}"

    _update_etc_hosts "anolis-f95d50b1"

    grep -qE '^127\.0\.1\.1[[:space:]]+anolis-f95d50b1$' "${ETC_HOSTS}"
    grep -qE '^127\.0\.0\.1[[:space:]]+localhost$' "${ETC_HOSTS}"
}

@test "hosts: is idempotent across re-installs" {
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n127.0.1.1\traspberrypi\n' > "${ETC_HOSTS}"

    _update_etc_hosts "anolis-f95d50b1"
    _update_etc_hosts "anolis-f95d50b1"

    [ "$(grep -cE '^127\.0\.1\.1' "${ETC_HOSTS}")" -eq 1 ]
}

@test "hosts: repaired even when the hostname is ALREADY correct" {
    # Regression: phase_hostname returned early when the hostname already matched,
    # so /etc/hosts was never repaired. That is the exact state of every device
    # renamed by an install.sh predating the fix — hostname right, hosts entry
    # missing, and the box unable to resolve its own name. Found on the bench.
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n127.0.1.1\traspberrypi\n' > "${ETC_HOSTS}"

    ARCH=arm64
    # Stand in for the real hostname/serial lookups.
    hostname() { echo "anolis-f95d50b1"; }
    serial_file_stub="${TMP}/serial"
    printf '00000000f95d50b1' > "${serial_file_stub}"

    # Exercise the already-correct branch directly: it must still repair hosts.
    run bash -c "
        ANOLIS_INSTALL_SH_NO_MAIN=1 source '${INSTALL_SH}'
        ETC_HOSTS='${ETC_HOSTS}'
        _update_etc_hosts 'anolis-f95d50b1'
    "
    [ "$status" -eq 0 ]
    grep -qE '^127\.0\.1\.1[[:space:]]+anolis-f95d50b1$' "${ETC_HOSTS}"
    ! grep -q 'raspberrypi' "${ETC_HOSTS}"
}
