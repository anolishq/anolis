#!/usr/bin/env bats
# Tests for the uninstall's data-purge guidance (anolishq/anolis#247).
#
# The printed instruction named /var/lib/influxdb2. The Debian influxdb2 package
# stores under /var/lib/influxdb, so an operator following it verbatim believed
# they had destroyed the recorded telemetry and left all of it on disk — a
# data-handling instruction failing in the direction that under-deletes while
# reporting success.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u

    FAKE="$(mktemp -d)"
    export ANOLIS_DATA_ROOT_PREFIX="${FAKE}"
}

teardown() {
    [[ -n "${FAKE:-}" ]] && rm -rf "${FAKE}"
}

@test "#247: reports the Debian influxdb data dir that actually exists" {
    mkdir -p "${FAKE}/var/lib/influxdb"
    run _observability_data_dirs
    [ "$status" -eq 0 ]
    [[ "${output}" == *"/var/lib/influxdb"* ]]
    # Must not invent the path that does not exist on this host.
    [[ "${output}" != *"/var/lib/influxdb2"* ]]
}

@test "#247: reports every data dir present, and only those" {
    mkdir -p "${FAKE}/var/lib/influxdb" "${FAKE}/var/lib/grafana"
    run _observability_data_dirs
    [ "$status" -eq 0 ]
    [[ "${output}" == *"/var/lib/influxdb"* ]]
    [[ "${output}" == *"/var/lib/grafana"* ]]
    [[ "${output}" != *".influxdbv2"* ]]
}

@test "#247: still handles the other spelling if a layout ever uses it" {
    mkdir -p "${FAKE}/var/lib/influxdb2"
    run _observability_data_dirs
    [ "$status" -eq 0 ]
    [[ "${output}" == *"/var/lib/influxdb2"* ]]
}

@test "#247: a host with no data dirs reports nothing rather than guessing" {
    run _observability_data_dirs
    [ "$status" -eq 0 ]
    [ -z "${output}" ]
}

# Every command that could change the host is recorded instead of run, and the
# prefix does not exist, so do_uninstall only prints.
_run_stubbed_uninstall() {
    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"
    local name
    for name in rm rmdir systemctl mv chown chmod; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    _obs_provisioned() { return 0; }
    PREFIX="${BATS_TEST_TMPDIR}/no-such-prefix"
    run do_uninstall
    unset -f rm rmdir systemctl mv chown chmod
}

@test "#328: the printed purge names the keyring package and Grafana's logs" {
    # The install's apt-get pulls influxdata-archive-keyring in as a dependency,
    # and grafana leaves /var/log/grafana; both survived the old purge text.
    _run_stubbed_uninstall
    [ "$status" -eq 0 ]
    [[ "${output}" == *"apt-get purge influxdb2 influxdb2-cli grafana influxdata-archive-keyring"* ]]
    [[ "${output}" == *"rm -rf /var/log/grafana"* ]]

    # The same text when recorded data is found.
    mkdir -p "${FAKE}/var/lib/influxdb"
    _run_stubbed_uninstall
    [ "$status" -eq 0 ]
    [[ "${output}" == *"apt-get purge influxdb2 influxdb2-cli grafana influxdata-archive-keyring"* ]]
    [[ "${output}" == *"rm -rf /var/log/grafana"* ]]
}
