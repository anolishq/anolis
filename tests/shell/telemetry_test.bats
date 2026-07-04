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
    set +e +u +o pipefail
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
