#!/usr/bin/env bats
# --uninstall acts only on the install at the prefix it was given (#544).
#
# The units install.sh writes record their prefix: anolis-runtime.service's
# WorkingDirectory and ExecStart, anolis-telemetry-export.service's ExecStart.
# An uninstall whose prefix is not the one those units name, such as a typo for
# a live install, must stop before it changes anything. Units install.sh does
# not write, such as the workbench's anolis-workbench.service, are left alone.
#
# SYSTEMD_DIR and the paths under /etc/anolis are pointed into the test's temp
# dir before install.sh is sourced. systemctl, rm and rmdir are stubs that
# record their arguments and do nothing else, so no test acts on the host even
# when the guard under test is missing.

bats_require_minimum_version 1.5.0

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    T="$(mktemp -d "${BATS_TEST_TMPDIR}/un.XXXXXX")"
    [[ -d "${T}" && "${T}" == /*/* ]] || return 1
    UNITS="${T}/systemd"
    mkdir -p "${UNITS}" "${T}/etc"
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    # shellcheck disable=SC1090
    source <(sed -E \
        -e "s|^readonly ([A-Z_]+)=\"/etc/anolis/|readonly \\1=\"${T}/etc/|" \
        -e "s|^readonly SYSTEMD_DIR=.*|readonly SYSTEMD_DIR=\"${UNITS}\"|" \
        -e "s|^readonly GRAFANA_DROPIN_DIR=.*|readonly GRAFANA_DROPIN_DIR=\"${T}/grafana.d\"|" \
        -e "s|^readonly INFLUX_DROPIN_DIR=.*|readonly INFLUX_DROPIN_DIR=\"${T}/influx.d\"|" \
        "${INSTALL_SH}")
    set +u
    [[ "${SYSTEMD_DIR}" == "${UNITS}" && "${RUNTIME_ENV_FILE}" == "${T}/etc/runtime.env" ]] || return 1

    CALLS="${T}/calls.log"
    : >"${CALLS}"
    local name
    for name in systemctl rm rmdir; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    _obs_provisioned() { return 1; }
}

teardown() {
    unset -f systemctl rm rmdir
}

# runtime_unit <prefix>: the unit phase_systemd writes for that prefix.
runtime_unit() {
    printf '[Service]\nUser=anolis\nWorkingDirectory=%s\nExecStart=%s/bin/anolis-runtime --config %s/config/runtime.yaml\n' \
        "$1" "$1" "$1" >"${UNITS}/anolis-runtime.service"
}

telemetry_unit() {
    printf '[Service]\nExecStart=%s/telemetry-export/venv/bin/anolis-telemetry-export --config %s/config/telemetry-export.yaml\n' \
        "$1" "$1" >"${UNITS}/anolis-telemetry-export.service"
}

workbench_unit() {
    printf '[Service]\nUser=pi\nExecStart=/home/pi/.local/share/anolis-workbench/venv/bin/anolis-workbench\n' \
        >"${UNITS}/anolis-workbench.service"
}

# Refused: non-zero, the prefix the units name is in the message, and no
# systemctl, rm or rmdir was run.
assert_refused_naming() {
    [ "$status" -ne 0 ] || return 1
    [[ "${output}" == *"$1"* ]] || return 1
    [ ! -s "${CALLS}" ] || { cat "${CALLS}"; return 1; }
}

@test "544: a typo for a live install's prefix changes nothing and names the live one" {
    runtime_unit "${T}/opt/anolis"
    : >"${T}/etc/runtime.env"
    PREFIX="${T}/opt/anolsi"
    run do_uninstall
    assert_refused_naming "${T}/opt/anolis"
}

@test "544: a telemetry-export unit naming another prefix stops it too" {
    telemetry_unit "${T}/opt/anolis"
    PREFIX="${T}/opt/anolsi"
    run do_uninstall
    assert_refused_naming "${T}/opt/anolis"
}

@test "544: an install.sh unit whose prefix cannot be read stops it" {
    printf '[Service]\nExecStart=/usr/bin/true\n' >"${UNITS}/anolis-runtime.service"
    PREFIX="${T}/opt/anolis"
    run do_uninstall
    assert_refused_naming "anolis-runtime.service"
}

@test "544: the units of a prefix that no longer exists are still removed (#339)" {
    runtime_unit "${T}/opt/anolis"
    telemetry_unit "${T}/opt/anolis"
    PREFIX="${T}/opt/anolis"
    run do_uninstall
    [ "$status" -eq 0 ]
    [[ "${output}" == *"uninstall complete"* ]]
    grep -qxF "systemctl stop anolis-runtime.service" "${CALLS}"
    grep -qxF "rm -f ${UNITS}/anolis-runtime.service" "${CALLS}"
    grep -qxF "systemctl stop anolis-telemetry-export.service" "${CALLS}"
    grep -qxF "rm -f ${UNITS}/anolis-telemetry-export.service" "${CALLS}"
}

@test "544: the workbench's own unit is kept, and the output says how to remove it" {
    runtime_unit "${T}/opt/anolis"
    workbench_unit
    PREFIX="${T}/opt/anolis"
    run do_uninstall
    [ "$status" -eq 0 ]
    [ "$(grep -cF "anolis-workbench.service" "${CALLS}")" -eq 0 ]
    [[ "${output}" == *"Kept: anolis-workbench.service"* ]]
    grep -qxF "systemctl stop anolis-runtime.service" "${CALLS}"
}
