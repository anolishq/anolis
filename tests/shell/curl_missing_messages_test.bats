#!/usr/bin/env bats
# What install.sh says when the target has no curl (#746). The two online-only
# components decide reachability with curl, so without it they blamed the
# network; and the health check's hint said "install" during a rollback. Here
# `command -v curl` fails and curl itself fails as it would for a missing
# binary. Nothing reaches the network or the host.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    command() { [[ "$1" == -v && "$2" == curl ]] && return 1; builtin command "$@"; }
    curl() { return 127; }
    apt-get() { :; }
    systemctl() { :; }
}

plain() { sed 's/\x1b\[[0-9;]*m//g' <<< "$1"; }

@test "746: --local telemetry-export with no curl says curl, not the network" {
    WITH_TELEMETRY_EXPORT=1
    LOCAL_PATH="${BATS_TEST_TMPDIR}/bundle"
    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    resolve_telemetry_export_version() { TELEMETRY_EXPORT_VERSION=0.0.0; }
    run phase_telemetry_export
    [ "${status}" -eq 0 ]
    [[ "$(plain "${output}")" == *"telemetry-export: curl not found"* ]]
    [[ "${output}" != *"needs network"* ]]
}

@test "746: observability with no curl says curl, not the network" {
    WITH_OBSERVABILITY=1
    run phase_observability
    [ "${status}" -eq 0 ]
    [[ "$(plain "${output}")" == *"observability: curl not found"* ]]
    [[ "${output}" != *"unreachable"* ]]
}

@test "746: the no-curl health hint names no operation, so it reads right in a rollback" {
    NO_START=0
    RUNTIME_RESTART_FAILED=0
    run phase_health
    [ "${status}" -eq 0 ]
    [[ "$(plain "${output}")" == *"Install curl (sudo apt install curl)"* ]]
    [[ "${output}" != *"for the install"* ]]
}
