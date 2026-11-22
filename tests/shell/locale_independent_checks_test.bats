#!/usr/bin/env bats
# install.sh's character checks hold in any locale (review of #916). Bash
# reads a bracket range such as [A-Za-z] by the locale's collation, so under a
# UTF-8 locale it matched letters such as é. A telemetry-export pin is passed
# to pip and a project name is written into configs, so both must be ASCII.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    UTF8_LOCALE=""
    for candidate in en_CA.utf8 en_US.utf8 C.utf8; do
        if locale -a 2>/dev/null | grep -qx "${candidate}"; then
            UTF8_LOCALE="${candidate}"
            break
        fi
    done
}

@test "916: a pin with a non-ASCII letter is refused under a UTF-8 locale" {
    [[ -n "${UTF8_LOCALE}" ]] || skip "no UTF-8 locale installed"
    run env LC_ALL="${UTF8_LOCALE}" bash -c '
        export ANOLIS_INSTALL_SH_NO_MAIN=1
        source "$1"
        set +u
        ASSEMBLED_TELEMETRY_EXPORT_PIN="0.4.2é"
        TELEMETRY_EXPORT_VERSION="0.1.1"
        resolve_telemetry_export_version
        echo "version=${TELEMETRY_EXPORT_VERSION}"
    ' _ "${INSTALL_SH}"
    [[ "${output}" == *"version=0.1.1"* ]]
    [[ "${output}" == *"ignoring malformed telemetry_export pin"* ]]
}

@test "916: an ASCII pin is still taken under a UTF-8 locale" {
    [[ -n "${UTF8_LOCALE}" ]] || skip "no UTF-8 locale installed"
    run env LC_ALL="${UTF8_LOCALE}" bash -c '
        export ANOLIS_INSTALL_SH_NO_MAIN=1
        source "$1"
        set +u
        ASSEMBLED_TELEMETRY_EXPORT_PIN="0.4.2rc1"
        TELEMETRY_EXPORT_VERSION="0.1.1"
        resolve_telemetry_export_version
        echo "version=${TELEMETRY_EXPORT_VERSION}"
    ' _ "${INSTALL_SH}"
    [[ "${output}" == *"version=0.4.2rc1"* ]]
}

@test "916: a project name with a non-ASCII letter is refused under a UTF-8 locale" {
    [[ -n "${UTF8_LOCALE}" ]] || skip "no UTF-8 locale installed"
    run env LC_ALL="${UTF8_LOCALE}" bash -c '
        export ANOLIS_INSTALL_SH_NO_MAIN=1
        source "$1"
        set +u
        die() { echo "DIE: $*"; exit 1; }
        _check_project_name "bioréacteur"
        echo "accepted"
    ' _ "${INSTALL_SH}"
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"DIE: project directory name"* ]]
}

@test "916: a plain project name passes under a UTF-8 locale" {
    [[ -n "${UTF8_LOCALE}" ]] || skip "no UTF-8 locale installed"
    run env LC_ALL="${UTF8_LOCALE}" bash -c '
        export ANOLIS_INSTALL_SH_NO_MAIN=1
        source "$1"
        set +u
        _check_project_name "bioreactor-v1.2_a"
        echo "accepted"
    ' _ "${INSTALL_SH}"
    [ "${status}" -eq 0 ]
    [[ "${output}" == *"accepted"* ]]
}
