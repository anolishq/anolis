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
    for name in systemctl apt-get curl dpkg sleep _obs_apt_repos _obs_setup_influx _obs_provision_grafana; do
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
