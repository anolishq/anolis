#!/usr/bin/env bats
# Tests for the --with-telemetry-export provisioning surface in tools/install.sh.
#
# These source install.sh (main() guarded) and exercise the pure helpers +
# flag parsing — no root, network, venv, or systemd required. The venv/pip/
# systemctl orchestration in phase_telemetry_export is validated on real
# hardware (anolishq/anolis#138), not here. See anolishq/anolis#137.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
}

@test "parse_args accepts --with-telemetry-export and sets the flag" {
    WITH_TELEMETRY_EXPORT=0
    parse_args --with-telemetry-export
    [ "${WITH_TELEMETRY_EXPORT}" -eq 1 ]
}

@test "the old --with-telemetry flag is gone (renamed, not silently accepted)" {
    run parse_args --with-telemetry
    [ "$status" -ne 0 ] && [[ "${output}" == *"Unknown option"* ]]
}

@test "emit_telemetry_unit renders a valid system unit" {
    run emit_telemetry_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"Description=Anolis Telemetry Export"* ]] &&
        [[ "${output}" == *"User=anolis"* ]] &&
        [[ "${output}" == *"EnvironmentFile=/etc/anolis/telemetry-export.env"* ]] &&
        [[ "${output}" == *"/telemetry-export/venv/bin/anolis-telemetry-export --config"* ]] &&
        [[ "${output}" == *"WantedBy=multi-user.target"* ]] &&
        [[ "${output}" == *"StartLimitBurst="* ]]
}

@test "a whitespace-only token does not satisfy the start-gate regex" {
    # The gate requires a non-whitespace char after '=' so a fat-fingered
    # 'TOKEN= ' does not start a service that would only crash-loop.
    run grep -qE '^ANOLIS_EXPORT_AUTH_TOKEN=[^[:space:]]' <(printf 'ANOLIS_EXPORT_AUTH_TOKEN= \n')
    [ "$status" -ne 0 ]
}

@test "render_telemetry_config writes the service contract (server+influxdb+limits)" {
    local cfg="${BATS_TEST_TMPDIR}/telemetry-export.yaml"
    render_telemetry_config "${cfg}"
    grep -qE '^server:' "${cfg}" &&
        grep -qE '^influxdb:' "${cfg}" &&
        grep -qE '^limits:' "${cfg}" &&
        grep -qE '^  port: 8091' "${cfg}" &&
        grep -qE '^  max_manifest_entries: 10000' "${cfg}"
}

@test "render_telemetry_config writes NO secret values (resolved from env)" {
    local cfg="${BATS_TEST_TMPDIR}/telemetry-export.yaml"
    render_telemetry_config "${cfg}"
    # No auth_token: / token: YAML keys — secrets come from the EnvironmentFile.
    ! grep -qE '^[[:space:]]*auth_token:' "${cfg}" &&
        ! grep -qE '^[[:space:]]*token:' "${cfg}"
}

@test "--with-telemetry-export adds the telemetry phase to the dry-run" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --project ./cfg --with-telemetry-export
    [ "$status" -eq 0 ] && [[ "${output}" == *"telemetry-export service"* ]]
}

@test "without the flag, the telemetry phase is absent from the dry-run" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --project ./cfg
    [ "$status" -eq 0 ] && [[ "${output}" != *"telemetry-export service"* ]]
}

# ===========================================================================
# #223: resolve telemetry-export version (profile pin > env > fallback)
# ===========================================================================

_mk_tex_manifest() {
    # $2 = telemetry_export version to embed; empty => omit the optional block.
    local dir="$1" ver="$2"
    mkdir -p "${dir}"
    if [[ -n "${ver}" ]]; then
        cat > "${dir}/manifest.json" <<JSON
{
  "schema_version": 1,
  "components": {
    "runtime": {"version": "9.9.9"},
    "providers": {"foo": {"version": "1.0.0"}},
    "optional": {"telemetry_export": {"version": "${ver}"}}
  }
}
JSON
    else
        cat > "${dir}/manifest.json" <<JSON
{
  "schema_version": 1,
  "components": {
    "runtime": {"version": "9.9.9"},
    "providers": {"foo": {"version": "1.0.0"}}
  }
}
JSON
    fi
}

@test "#223 resolver: assembled pin (online --project) wins over env" {
    ASSEMBLED_TELEMETRY_EXPORT_PIN="0.9.9"
    TELEMETRY_EXPORT_VERSION_ENV="8.8.8"; TELEMETRY_EXPORT_VERSION="8.8.8"
    BUNDLE_DIR=""  # online path has no bundle
    resolve_telemetry_export_version > "${BATS_TEST_TMPDIR}/out" 2>&1
    [ "${TELEMETRY_EXPORT_VERSION}" = "0.9.9" ]
    grep -q "8.8.8" "${BATS_TEST_TMPDIR}/out"
}

@test "#223 resolver: manifest pin (offline --local) wins over env" {
    ASSEMBLED_TELEMETRY_EXPORT_PIN=""
    _mk_tex_manifest "${BATS_TEST_TMPDIR}/b" "0.9.9"
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/b"
    TELEMETRY_EXPORT_VERSION_ENV="8.8.8"; TELEMETRY_EXPORT_VERSION="8.8.8"
    resolve_telemetry_export_version > "${BATS_TEST_TMPDIR}/out" 2>&1
    [ "${TELEMETRY_EXPORT_VERSION}" = "0.9.9" ]
    grep -q "8.8.8" "${BATS_TEST_TMPDIR}/out"
}

@test "#223 resolver: env wins when there is no pin" {
    ASSEMBLED_TELEMETRY_EXPORT_PIN=""
    _mk_tex_manifest "${BATS_TEST_TMPDIR}/b" ""
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/b"
    TELEMETRY_EXPORT_VERSION_ENV="8.8.8"; TELEMETRY_EXPORT_VERSION="8.8.8"
    resolve_telemetry_export_version >/dev/null 2>&1
    [ "${TELEMETRY_EXPORT_VERSION}" = "8.8.8" ]
}

@test "#223 resolver: fallback when neither pin nor env" {
    ASSEMBLED_TELEMETRY_EXPORT_PIN=""
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/none"  # no manifest
    TELEMETRY_EXPORT_VERSION_ENV=""; TELEMETRY_EXPORT_VERSION="0.1.1"
    resolve_telemetry_export_version >/dev/null 2>&1
    [ "${TELEMETRY_EXPORT_VERSION}" = "0.1.1" ]
}

@test "#223 resolver: a non-version manifest value is ignored (and warns)" {
    ASSEMBLED_TELEMETRY_EXPORT_PIN=""
    _mk_tex_manifest "${BATS_TEST_TMPDIR}/b" "1.2.3; rm -rf /"
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/b"
    TELEMETRY_EXPORT_VERSION_ENV=""; TELEMETRY_EXPORT_VERSION="0.1.1"
    resolve_telemetry_export_version > "${BATS_TEST_TMPDIR}/out" 2>&1
    [ "${TELEMETRY_EXPORT_VERSION}" = "0.1.1" ]
    grep -q "malformed" "${BATS_TEST_TMPDIR}/out"
}

@test "#223 _manifest_telemetry_export_pin: a provider named telemetry_export cannot spoof the pin" {
    mkdir -p "${BATS_TEST_TMPDIR}/b"
    cat > "${BATS_TEST_TMPDIR}/b/manifest.json" <<'JSON'
{
  "schema_version": 1,
  "components": {
    "providers": {"telemetry_export": {"version": "6.6.6"}},
    "optional": {"telemetry_export": {"version": "0.9.9"}}
  }
}
JSON
    run _manifest_telemetry_export_pin "${BATS_TEST_TMPDIR}/b/manifest.json"
    [ "${output}" = "0.9.9" ]
}

@test "#223 _manifest_telemetry_export_pin does not misread provider versions" {
    _mk_tex_manifest "${BATS_TEST_TMPDIR}/b" ""  # providers present, no optional
    run _manifest_telemetry_export_pin "${BATS_TEST_TMPDIR}/b/manifest.json"
    [ -z "${output}" ]
}
