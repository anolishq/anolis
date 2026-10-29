#!/usr/bin/env bats
# Tests for the --with-observability provisioning surface in tools/install.sh.
#
# These source install.sh (main() guarded) and exercise the pure helpers +
# flag parsing — no root, network, apt, or systemd required. The
# influx-setup/token/grafana orchestration in phase_observability is
# validated on real hardware (#162 chunk D acceptance), not here.
#
# NOTE deliberate divergence from the older bats files: setup() keeps errexit
# and pipefail ACTIVE (only nounset is relaxed for bats internals). With
# errexit disabled, non-final assertion failures are silently swallowed and
# the suite goes vacuous — see anolishq/anolis#207.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
}

@test "parse_args accepts --with-observability, sets the flag, and no longer warns" {
    WITH_OBSERVABILITY=0
    run parse_args --with-observability
    [ "$status" -eq 0 ]
    [[ "${output}" != *"not implemented"* ]]
    # run executes in a subshell; re-apply for the state assertion.
    parse_args --with-observability
    [ "${WITH_OBSERVABILITY}" -eq 1 ]
}

@test "emit_grafana_dropin points systemd at the grafana env file, tolerant of absence" {
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    [[ "${output}" == *"[Service]"* ]]
    # '-' prefix: a missing env file must degrade Grafana, not fail the unit.
    [[ "${output}" == *"EnvironmentFile=-/etc/anolis/observability-grafana.env"* ]]
}

@test "emit_grafana_dropin sets Grafana's log level to warn (#248)" {
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    # level=info request logging must not compete with runtime logs in the journal.
    grep -qx 'Environment=GF_LOG_LEVEL=warn' <<<"${output}"
}

@test "659: emit_grafana_dropin keeps Grafana from updating its plugins at startup" {
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    # A background plugin update unregisters the InfluxDB datasource mid-session.
    grep -qx 'Environment=GF_PLUGINS_PREINSTALL_AUTO_UPDATE=false' <<<"${output}"
}

@test "emit_grafana_env renders connection, token, and first-boot admin password" {
    run emit_grafana_env "read-token-123" "grafana-pw-456"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"INFLUX_URL=http://127.0.0.1:8086"* ]]
    [[ "${output}" == *"INFLUX_ORG=anolis"* ]]
    [[ "${output}" == *"INFLUX_BUCKET=anolis"* ]]
    [[ "${output}" == *"INFLUX_TOKEN=read-token-123"* ]]
    [[ "${output}" == *"GF_SECURITY_ADMIN_PASSWORD=grafana-pw-456"* ]]
    [[ "${output}" == *"GF_AUTH_ANONYMOUS_ENABLED=true"* ]]
    [[ "${output}" == *"GF_DASHBOARDS_DEFAULT_HOME_DASHBOARD_PATH=/var/lib/grafana/dashboards/signal-history.json"* ]]
}

@test "emit_grafana_env never contains a hardcoded dev token" {
    run emit_grafana_env "t" "p"
    [ "$status" -eq 0 ]
    [[ "${output}" != *"dev-token"* ]]
}

@test "retention default is 30 days" {
    [ "${OBSERVABILITY_RETENTION}" = "720h" ]
}

@test "retention is env-overridable" {
    # Fresh process: re-sourcing in this shell would abort on the readonly
    # constants install.sh declares.
    run env ANOLIS_INSTALL_SH_NO_MAIN=1 OBSERVABILITY_RETENTION=2160h \
        bash -c "source '${INSTALL_SH}'; printf '%s' \"\${OBSERVABILITY_RETENTION}\""
    [ "$status" -eq 0 ]
    [ "$output" = "2160h" ]
}

@test "phase_observability is a no-op when the flag is off" {
    WITH_OBSERVABILITY=0
    run phase_observability
    [ "$status" -eq 0 ]
    [ -z "${output}" ]
}

# A re-run over an installed stack rewrites Grafana's drop-in, env file and
# datasource. A running grafana-server keeps what it read at start, and
# `enable --now` does nothing to a running unit, so every run restarts it.
# Everything that touches the host or the network is a recording stub.
_stub_obs_host() {
    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"
    local name
    for name in systemctl apt-get curl dpkg sleep _obs_apt_repos _obs_write_influx_dropin _obs_setup_influx _obs_provision_grafana; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    WITH_OBSERVABILITY=1
}

@test "an observability run restarts grafana-server, never only enable --now" {
    _stub_obs_host
    run phase_observability
    [ "$status" -eq 0 ]
    grep -qxF "systemctl enable grafana-server" "${CALLS}"
    grep -qxF "systemctl restart grafana-server" "${CALLS}"
    if grep -qxF "systemctl enable --now grafana-server" "${CALLS}"; then return 1; fi
}

@test "grafana-server is restarted after Grafana is provisioned" {
    _stub_obs_host
    run phase_observability
    [ "$status" -eq 0 ]
    local provisioned restarted
    provisioned=$(grep -nxF "_obs_provision_grafana " "${CALLS}" | cut -d: -f1)
    restarted=$(grep -nxF "systemctl restart grafana-server" "${CALLS}" | cut -d: -f1)
    [ -n "${provisioned}" ] && [ -n "${restarted}" ]
    [ "${restarted}" -gt "${provisioned}" ]
}

# ---------------------------------------------------------------------------
# #351: Grafana and InfluxDB bind loopback unless --with-grafana-network
# ---------------------------------------------------------------------------

@test "351: parse_args accepts --with-grafana-network" {
    GRAFANA_NETWORK=0
    parse_args --with-observability --with-grafana-network
    [ "${GRAFANA_NETWORK}" -eq 1 ]
}

@test "351: Grafana binds 127.0.0.1 by default" {
    GRAFANA_NETWORK=0
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    grep -qx 'Environment=GF_SERVER_HTTP_ADDR=127.0.0.1' <<<"${output}"
    if grep -q 'GF_SERVER_HTTP_ADDR=0.0.0.0' <<<"${output}"; then return 1; fi
}

@test "351: --with-grafana-network binds Grafana to every interface" {
    GRAFANA_NETWORK=1
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    grep -qx 'Environment=GF_SERVER_HTTP_ADDR=0.0.0.0' <<<"${output}"
    if grep -q 'GF_SERVER_HTTP_ADDR=127.0.0.1' <<<"${output}"; then return 1; fi
}

@test "351: InfluxDB binds 127.0.0.1:8086 with or without the flag" {
    local flag
    for flag in 0 1; do
        GRAFANA_NETWORK=${flag}
        run emit_influxdb_dropin
        [ "$status" -eq 0 ]
        [[ "${output}" == *"[Service]"* ]]
        grep -qx 'Environment=INFLUXD_HTTP_BIND_ADDRESS=127.0.0.1:8086' <<<"${output}"
    done
}

@test "351: a Grafana drop-in written with the flag is recognised as network-bound" {
    local dropin="${BATS_TEST_TMPDIR}/anolis.conf"
    GRAFANA_NETWORK=1
    emit_grafana_dropin >"${dropin}"
    _grafana_dropin_is_network "${dropin}"
    GRAFANA_NETWORK=0
    emit_grafana_dropin >"${dropin}"
    if _grafana_dropin_is_network "${dropin}"; then return 1; fi
    if _grafana_dropin_is_network "${BATS_TEST_TMPDIR}/missing.conf"; then return 1; fi
}

@test "351: the InfluxDB drop-in is written before influxdb is restarted" {
    _stub_obs_host
    run phase_observability
    [ "$status" -eq 0 ]
    local written restarted
    written=$(grep -nxF "_obs_write_influx_dropin " "${CALLS}" | cut -d: -f1)
    restarted=$(grep -nxF "systemctl restart influxdb" "${CALLS}" | cut -d: -f1)
    [ -n "${written}" ] && [ -n "${restarted}" ]
    [ "${restarted}" -gt "${written}" ]
    if grep -qxF "systemctl enable --now influxdb" "${CALLS}"; then return 1; fi
}

_summary_stubs() {
    WITH_OBSERVABILITY=1
    WITH_TELEMETRY_EXPORT=0
    ARCH=arm64
    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    _runtime_port() { echo 8080; }
    hostname() { echo pi-test; }
    SUDO_USER=operator
}

@test "351: the default summary prints the ssh tunnel to Grafana, not a network address" {
    _summary_stubs
    _OBS_GRAFANA_BIND_APPLIED=1
    GRAFANA_NETWORK=0
    _OBS_GRAFANA_WAS_NETWORK=0
    run phase_summary
    [[ "${output}" == *"ssh -L 3000:127.0.0.1:3000 operator@pi-test.local"* ]]
    [[ "${output}" != *"http://pi-test.local:3000"* ]]
}

@test "514: after a failed admin password reset the summary does not point at the password" {
    _summary_stubs
    _OBS_GRAFANA_BIND_APPLIED=1
    _OBS_GRAFANA_PW_UNKNOWN=1
    for GRAFANA_NETWORK in 0 1; do
        run phase_summary
        [[ "${output}" != *"(admin password: ${OBSERVABILITY_ENV_FILE})"* ]]
        [[ "${output}" == *"(admin password: unknown, its reset failed; see the warnings above)"* ]]
    done
}

@test "351: with --with-grafana-network the summary prints Grafana's network address" {
    _summary_stubs
    _OBS_GRAFANA_BIND_APPLIED=1
    GRAFANA_NETWORK=1
    run phase_summary
    [[ "${output}" == *"Grafana:   http://pi-test.local:3000"* ]]
    [[ "${output}" != *"ssh -L 3000"* ]]
}

@test "351: a re-run without the flag says Grafana went back to loopback" {
    _summary_stubs
    _OBS_GRAFANA_BIND_APPLIED=1
    GRAFANA_NETWORK=0
    _OBS_GRAFANA_WAS_NETWORK=1
    run phase_summary
    [[ "${output}" == *"no longer on the network"* ]]
    [[ "${output}" == *"ssh -L 3000:127.0.0.1:3000"* ]]
}

# ---------------------------------------------------------------------------
# #349: drop the InfluxDB datasource's debug JSON from the journal
# ---------------------------------------------------------------------------

@test "349: the Grafana drop-in discards debug-level JSON lines from the journal" {
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    local line pattern
    line=$(grep -x 'LogFilterPatterns=~.*' <<<"${output}")
    pattern="${line#LogFilterPatterns=~}"
    # The pattern matches the measured datasource line and nothing else here.
    grep -qE "${pattern}" <<<'{"@level":"debug","@message":"Received a query request","logger":"tsdb.influxdb","numQueries":1}'
    if grep -qE "${pattern}" <<<'{"@level":"error","@message":"influx proxy creation failed","logger":"tsdb.influxdb"}'; then return 1; fi
    if grep -qE "${pattern}" <<<'logger=context level=warn msg="Request Completed"'; then return 1; fi
    # No quotes or backslashes for systemd's unit-file unquoting to change.
    [[ "${pattern}" != *'"'* && "${pattern}" != *'\'* ]]
}

@test "349: the drop-in comment names the datasource logger, not request logging" {
    run emit_grafana_dropin
    [ "$status" -eq 0 ]
    [[ "${output}" != *"logs requests at level=info"* ]]
    [[ "${output}" == *"tsdb.influxdb"* ]]
}

@test "349: systemd older than 253 is reported as unable to filter" {
    systemctl() { [[ "$1" == "--version" ]] && printf 'systemd 252 (252.39-1~deb12u1)\n+PAM +AUDIT\n'; }
    run _warn_if_no_log_filter
    [[ "${output}" == *"systemd 252"* ]]
    systemctl() { [[ "$1" == "--version" ]] && printf 'systemd 257 (257.13-1~deb13u1)\n'; }
    run _warn_if_no_log_filter
    [ -z "${output}" ]
}

@test "351: a failed provisioning run reports Grafana's bind as unchanged" {
    _stub_obs_host
    # Provisioning fails before the drop-in is written or grafana restarted.
    _obs_provision_grafana() { printf '%s\n' "_obs_provision_grafana $*" >>"${CALLS}"; return 1; }
    phase_observability > /dev/null 2>&1
    _summary_stubs
    GRAFANA_NETWORK=0
    run phase_summary
    [[ "${output}" == *"Grafana bind unchanged"* ]]
    [[ "${output}" != *"ssh -L 3000"* ]]
    [[ "${output}" != *"127.0.0.1:3000 on this machine"* ]]
}

@test "364: a failed grafana restart reports Grafana's bind as unchanged" {
    _stub_obs_host
    # The drop-in is written, but grafana-server does not restart with it.
    systemctl() {
        printf '%s\n' "systemctl $*" >>"${CALLS}"
        [[ "$*" != "restart grafana-server" ]]
    }
    phase_observability > /dev/null 2>&1
    grep -qxF "systemctl restart grafana-server" "${CALLS}"
    _summary_stubs
    GRAFANA_NETWORK=0
    run phase_summary
    [[ "${output}" == *"Grafana bind unchanged"* ]]
    [[ "${output}" != *"ssh -L 3000"* ]]
    [[ "${output}" != *"127.0.0.1:3000 on this machine"* ]]
}

@test "351: a run that restarts grafana reports the bind it applied" {
    _stub_obs_host
    phase_observability > /dev/null 2>&1
    _summary_stubs
    GRAFANA_NETWORK=0
    run phase_summary
    [[ "${output}" == *"ssh -L 3000:127.0.0.1:3000"* ]]
    [[ "${output}" != *"bind unchanged"* ]]
}

@test "351: on x86_64 the tunnel names a host placeholder, not localhost" {
    _summary_stubs
    ARCH=x86_64
    GRAFANA_NETWORK=0
    _OBS_GRAFANA_BIND_APPLIED=1
    run phase_summary
    [[ "${output}" == *"ssh -L 3000:127.0.0.1:3000 operator@<host>"* ]]
    [[ "${output}" != *"@localhost"* ]]
}

@test "351: --with-grafana-network without --with-observability warns" {
    WITH_OBSERVABILITY=0
    GRAFANA_NETWORK=0
    run parse_args --with-grafana-network
    [ "$status" -eq 0 ]
    [[ "${output}" == *"--with-grafana-network has no effect without --with-observability"* ]]
    run parse_args --with-observability --with-grafana-network
    [[ "${output}" != *"no effect"* ]]
}

@test "351: uninstall removes the influxdb drop-in with grafana's" {
    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"
    local name
    for name in rm rmdir systemctl mv chown chmod _observability_data_dirs; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    _obs_provisioned() { return 0; }
    PREFIX="${BATS_TEST_TMPDIR}/no-such-prefix"
    run do_uninstall
    [ "$status" -eq 0 ]
    grep -qxF "rm -rf ${GRAFANA_DROPIN_DIR} ${INFLUX_DROPIN_DIR}" "${CALLS}"
}
