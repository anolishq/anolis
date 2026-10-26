#!/usr/bin/env bats
# An uninstall keeps the InfluxDB instance and its data, so it keeps the
# credentials for that instance too, and a later --with-observability install
# reuses them (anolishq/anolis#469). Before this, the uninstall deleted
# observability.env and observability-grafana.env, and the reinstall found
# InfluxDB already set up, skipped token creation, and left Grafana and the
# runtime with no token.
#
# Every path constant under /etc/anolis and SYSTEMD_DIR is pointed into this
# test's temp dir before install.sh is sourced. Commands that would change the
# host are recorded instead; rm is carried out only when every operand lies
# inside the fake /etc, and install drops -o/-g so it runs unprivileged.
# InfluxDB is a stub that reports itself already set up.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

WRITE_TOKEN="fake-write-token-not-a-secret"
READ_TOKEN="fake-read-token-not-a-secret"
GRAFANA_PW="fake-grafana-pw-not-a-secret"

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
    [[ "${OBSERVABILITY_ENV_FILE}" == "${ETC}/observability.env" ]]
    [[ "${GRAFANA_ENV_FILE}" == "${ETC}/observability-grafana.env" ]]
    [[ "${RUNTIME_ENV_FILE}" == "${ETC}/runtime.env" ]]
    [[ "${TELEMETRY_ENV_FILE}" == "${ETC}/telemetry-export.env" ]]

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
    install() {
        printf 'install %s\n' "$*" >>"${CALLS}"
        local args=()
        while (($#)); do
            case "$1" in
                -o | -g) shift 2; continue ;;
                -m) args+=("$1" "$2"); shift 2; continue ;;
                -*) args+=("$1") ;;
                /dev/null | "${BATS_TEST_TMPDIR}"/?*) args+=("$1") ;;
                *) return 0 ;; # outside the fake tree: recorded, not run
            esac
            shift
        done
        command install "${args[@]}"
    }
    # /health answers; /api/v2/setup says onboarding already happened.
    curl() {
        case "$*" in
            */api/v2/setup*) printf '{"allowed": false}\n' ;;
            *) return 0 ;;
        esac
    }
    local name
    for name in rmdir systemctl mv chown chmod sleep influx _observability_data_dirs; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    _obs_provisioned() { return 0; }
    PREFIX="${BATS_TEST_TMPDIR}/no-such-prefix"
    _OBS_GRAFANA_PW="fake-new-grafana-pw-not-a-secret"
}

teardown() {
    unset -f rm install curl rmdir systemctl mv chown chmod sleep influx
}

# What a fresh --with-observability install leaves in /etc/anolis.
_fresh_install_files() {
    {
        echo "INFLUXDB_ADMIN_USERNAME=admin"
        echo "INFLUXDB_ADMIN_PASSWORD=fake-admin-pw-not-a-secret"
        echo "INFLUXDB_ADMIN_TOKEN=fake-admin-token-not-a-secret"
        echo "INFLUXDB_WRITE_TOKEN=${WRITE_TOKEN}"
        echo "INFLUXDB_READ_TOKEN=${READ_TOKEN}"
        echo "GRAFANA_ADMIN_PASSWORD=${GRAFANA_PW}"
    } >"${OBSERVABILITY_ENV_FILE}"
    emit_grafana_env "${READ_TOKEN}" "${GRAFANA_PW}" >"${GRAFANA_ENV_FILE}"
    printf 'ANOLIS_API_TOKEN=fake-api-token-not-a-secret\nINFLUXDB_TOKEN=%s\n' "${WRITE_TOKEN}" >"${RUNTIME_ENV_FILE}"
}

@test "469: uninstall keeps the credentials of the InfluxDB instance it keeps" {
    _fresh_install_files
    run do_uninstall
    [ "$status" -eq 0 ]
    [ -f "${OBSERVABILITY_ENV_FILE}" ]
    [ -f "${GRAFANA_ENV_FILE}" ]
    [ ! -e "${RUNTIME_ENV_FILE}" ]
    [[ "${output}" == *"Kept: the InfluxDB credentials in ${OBSERVABILITY_ENV_FILE} ${GRAFANA_ENV_FILE}"* ]]
    [[ "${output}" == *"rm -f ${OBSERVABILITY_ENV_FILE} ${GRAFANA_ENV_FILE}"* ]]
    [[ "${output}" != *"not-a-secret"* ]]
}

@test "469: without observability provisioned, uninstall still removes the files" {
    _fresh_install_files
    _obs_provisioned() { return 1; }
    run do_uninstall
    [ "$status" -eq 0 ]
    [ ! -e "${OBSERVABILITY_ENV_FILE}" ]
    [ ! -e "${GRAFANA_ENV_FILE}" ]
}

@test "469: uninstall then install wires the kept tokens back in" {
    _fresh_install_files
    run do_uninstall
    [ "$status" -eq 0 ]
    printf 'ANOLIS_EXPORT_INFLUX_TOKEN=\n' >"${TELEMETRY_ENV_FILE}"
    run phase_runtime_auth
    [ "$status" -eq 0 ]
    command rm -f "${GRAFANA_ENV_FILE}" # restored too, if only it went missing

    run _obs_setup_influx
    [ "$status" -eq 0 ]
    grep -qxF "INFLUXDB_TOKEN=${WRITE_TOKEN}" "${RUNTIME_ENV_FILE}"
    grep -qE '^ANOLIS_API_TOKEN=[0-9a-f]{64}$' "${RUNTIME_ENV_FILE}"
    grep -qxF "INFLUX_TOKEN=${READ_TOKEN}" "${GRAFANA_ENV_FILE}"
    grep -qxF "GF_SECURITY_ADMIN_PASSWORD=${GRAFANA_PW}" "${GRAFANA_ENV_FILE}"
    [ "$(grep -c '^GRAFANA_ADMIN_PASSWORD=' "${OBSERVABILITY_ENV_FILE}")" -eq 1 ]
    grep -qxF "ANOLIS_EXPORT_INFLUX_TOKEN=${READ_TOKEN}" "${TELEMETRY_ENV_FILE}"
    [ "$(stat -c %a "${GRAFANA_ENV_FILE}")" = "600" ]
    if grep -q '^influx ' "${CALLS}"; then return 1; fi
    [[ "${output}" != *"not-a-secret"* ]]
}

@test "469: a reinstall leaves a runtime token the operator set alone" {
    _fresh_install_files
    printf 'ANOLIS_API_TOKEN=x\nINFLUXDB_TOKEN=operator-rotated\n' >"${RUNTIME_ENV_FILE}"
    run _obs_setup_influx
    [ "$status" -eq 0 ]
    grep -qxF "INFLUXDB_TOKEN=operator-rotated" "${RUNTIME_ENV_FILE}"
    [ "$(grep -c '^INFLUXDB_TOKEN=' "${RUNTIME_ENV_FILE}")" -eq 1 ]
}

@test "469: a reinstall fills a blank runtime token" {
    _fresh_install_files
    printf 'ANOLIS_API_TOKEN=x\nINFLUXDB_TOKEN=\n' >"${RUNTIME_ENV_FILE}"
    run _obs_setup_influx
    [ "$status" -eq 0 ]
    grep -qxF "INFLUXDB_TOKEN=${WRITE_TOKEN}" "${RUNTIME_ENV_FILE}"
    [ "$(grep -c '^INFLUXDB_TOKEN=' "${RUNTIME_ENV_FILE}")" -eq 1 ]
}

@test "469: an instance set up without any of its credentials is refused, with a recovery step" {
    printf 'ANOLIS_API_TOKEN=x\n' >"${RUNTIME_ENV_FILE}"
    run _obs_setup_influx
    [ "$status" -ne 0 ]
    [ ! -e "${GRAFANA_ENV_FILE}" ]
    [[ "${output}" == *"credentials"* ]]
    [[ "${output}" == *"${OBSERVABILITY_ENV_FILE}"* ]]
    [[ "${output}" == *"INFLUXDB_WRITE_TOKEN"* && "${output}" == *"INFLUXDB_READ_TOKEN"* ]]
    [[ "${output}" == *"influx auth create"* ]]
    # The commands read root's influx CLI config, so each one runs as root.
    [[ "${output}" == *'id=$(sudo -H influx bucket list '* ]]
    [[ "${output}" == *"sudo -H influx auth create --org anolis --write-bucket"* ]]
    [[ "${output}" == *"sudo -H influx auth create --org anolis --read-bucket"* ]]
}

@test "469: an operator-run instance with its own Grafana env file is left alone" {
    printf 'INFLUX_TOKEN=operator-made\n' >"${GRAFANA_ENV_FILE}"
    printf 'ANOLIS_API_TOKEN=x\n' >"${RUNTIME_ENV_FILE}"
    run _obs_setup_influx
    [ "$status" -eq 0 ]
    grep -qxF "INFLUX_TOKEN=operator-made" "${GRAFANA_ENV_FILE}"
}

@test "469: recovering with only the two tokens records the new Grafana password" {
    # The refusal's recovery step: the operator writes back just the tokens.
    # Written by hand, so with whatever mode the operator's umask gave it.
    printf 'INFLUXDB_WRITE_TOKEN=%s\nINFLUXDB_READ_TOKEN=%s\n' "${WRITE_TOKEN}" "${READ_TOKEN}" >"${OBSERVABILITY_ENV_FILE}"
    command chmod 644 "${OBSERVABILITY_ENV_FILE}"
    printf 'ANOLIS_API_TOKEN=x\n' >"${RUNTIME_ENV_FILE}"
    # Record how many password lines the file held when each chmod ran.
    chmod() {
        printf 'chmod %s (passwords: %s)\n' "$*" "$(grep -c '^GRAFANA_ADMIN_PASSWORD=' "${OBSERVABILITY_ENV_FILE}")" >>"${CALLS}"
    }
    run _obs_setup_influx
    [ "$status" -eq 0 ]
    grep -qxF "GF_SECURITY_ADMIN_PASSWORD=${_OBS_GRAFANA_PW}" "${GRAFANA_ENV_FILE}"
    [ "$(grep -c '^GRAFANA_ADMIN_PASSWORD=' "${OBSERVABILITY_ENV_FILE}")" -eq 1 ]
    grep -qxF "GRAFANA_ADMIN_PASSWORD=${_OBS_GRAFANA_PW}" "${OBSERVABILITY_ENV_FILE}"
    grep -qxF "INFLUXDB_WRITE_TOKEN=${WRITE_TOKEN}" "${OBSERVABILITY_ENV_FILE}"
    # Locked down before the password went in.
    grep -qxF "chmod 600 ${OBSERVABILITY_ENV_FILE} (passwords: 0)" "${CALLS}"
    grep -qxF "chown root:root ${OBSERVABILITY_ENV_FILE}" "${CALLS}"
    [[ "${output}" != *"not-a-secret"* ]]
}
