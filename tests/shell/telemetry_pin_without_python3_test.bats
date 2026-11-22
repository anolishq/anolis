#!/usr/bin/env bats
# Reading the telemetry-export pin from manifest.json when the target has no
# python3 (review of #863). The reader runs as pin=$(...) under set -e, so a
# missing python3 ended the whole install with exit 127 and no message, after
# the new binaries were in place and before the runtime restarted. python3 is
# a function that fails as a missing binary does; nothing reaches the host.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    command() { [[ "$1" == -v && "$2" == python3 ]] && return 1; builtin command "$@"; }
    python3() { return 127; }
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bundle"
    mkdir -p "${BUNDLE_DIR}"
    printf '{"components":{"optional":{"telemetry_export":{"version":"0.4.2"}}}}\n' > "${BUNDLE_DIR}/manifest.json"
    ASSEMBLED_TELEMETRY_EXPORT_PIN=""
}

@test "863: under set -e, no python3 does not end the install" {
    run bash -c '
        export ANOLIS_INSTALL_SH_NO_MAIN=1
        source "$1"
        set -e
        command() { [[ "$1" == -v && "$2" == python3 ]] && return 1; builtin command "$@"; }
        python3() { return 127; }
        BUNDLE_DIR="$2"
        ASSEMBLED_TELEMETRY_EXPORT_PIN=""
        resolve_telemetry_export_version
        echo "REACHED version=${TELEMETRY_EXPORT_VERSION}"
    ' _ "${INSTALL_SH}" "${BUNDLE_DIR}"
    [ "${status}" -eq 0 ]
    [[ "${output}" == *"REACHED"* ]]
}

@test "863: with no python3 the pin read prints nothing and succeeds" {
    run _manifest_telemetry_export_pin "${BUNDLE_DIR}/manifest.json"
    [ "${status}" -eq 0 ]
    [ -z "${output}" ]
}

@test "863: with python3 the pin is read as before" {
    unset -f python3
    command() { builtin command "$@"; }
    if ! builtin command -v python3 >/dev/null; then skip "python3 not installed here"; fi
    run _manifest_telemetry_export_pin "${BUNDLE_DIR}/manifest.json"
    [ "${status}" -eq 0 ]
    [ "${output}" = "0.4.2" ]
}
