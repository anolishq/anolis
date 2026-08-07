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
