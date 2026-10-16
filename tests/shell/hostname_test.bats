#!/usr/bin/env bats
# Tests for --keep-hostname (#327).
#
# By default the install renames an arm64 host to anolis-<serial>. With
# --keep-hostname phase_hostname does not run, and the summary's URL uses the
# host's own name.
#
# main runs in-process with every phase stubbed to record its name, so no
# test renames this host or touches /etc/hosts.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

_stub_cmd() {
    local name
    for name in "$@"; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
}

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u

    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"
    hostname() { echo bench-pi; }
}

_stub_phases() {
    _stub_cmd hostnamectl check_prefix \
        phase_root_check phase_detect phase_prepare_bundle phase_verify \
        preflight_variant_selection phase_version_check phase_system_user \
        phase_directories phase_backup phase_install_binaries \
        phase_config_project phase_config_runtime phase_runtime_auth \
        phase_config_providers phase_host_preflight phase_manifest \
        phase_systemd phase_telemetry_export phase_observability \
        phase_hostname phase_start phase_health phase_summary
}

@test "327: by default the install renames the host" {
    _stub_phases
    run main --local "${BATS_TEST_TMPDIR}" --no-start
    [ "$status" -eq 0 ]
    grep -qxF "phase_hostname " "${CALLS}"
}

@test "327: --keep-hostname skips the rename and says so" {
    _stub_phases
    run main --local "${BATS_TEST_TMPDIR}" --no-start --keep-hostname
    [ "$status" -eq 0 ]
    [[ "${output}" == *"hostname: keeping bench-pi (--keep-hostname)"* ]]
    if grep -q '^phase_hostname' "${CALLS}"; then return 1; fi
    # The phases after it still run.
    grep -qxF "phase_summary " "${CALLS}"
}

@test "327: the dry run says it would set the hostname by default" {
    _stub_phases
    run main --dry-run --project "${BATS_TEST_TMPDIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"would execute: set hostname"* ]]
    [[ "${output}" != *"keep hostname"* ]]
}

@test "327: the dry run with --keep-hostname says it would keep the name" {
    _stub_phases
    run main --dry-run --project "${BATS_TEST_TMPDIR}" --keep-hostname
    [ "$status" -eq 0 ]
    [[ "${output}" == *"would execute: keep hostname bench-pi (--keep-hostname)"* ]]
    [[ "${output}" != *"set hostname"* ]]
}

@test "327: the summary's access URL uses the host's own name" {
    _runtime_port() { echo 8080; }
    ARCH=arm64
    KEEP_HOSTNAME=1
    run phase_summary
    [[ "${output}" == *"Access:   http://bench-pi.local:8080"* ]]
}

@test "327: --help lists --keep-hostname" {
    run main --help
    [ "$status" -eq 0 ]
    [[ "${output}" == *"--keep-hostname"* ]]
}
