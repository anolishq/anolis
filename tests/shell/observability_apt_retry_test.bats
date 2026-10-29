#!/usr/bin/env bats
# anolishq/anolis#669: phase_observability's package step against a stubbed
# apt-get. A repo whose index is mid-publish ("Mirror sync in progress?") makes
# `apt-get update` exit non-zero; install.sh used to skip the whole install
# with only "package install failed". It retries the update, and when it gives
# up it names the failing step and apt's first error.
#
# Every external the phase reaches before and after the package step is a stub
# function: no root, network, apt, dpkg or systemd. _obs_write_influx_dropin
# fails on purpose, so the phase stops right after the package step.
#
# errexit and pipefail stay active (anolishq/anolis#207).

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
MIRROR_ERROR="E: Failed to fetch https://apt.grafana.com/dists/stable/main/binary-amd64/Packages.bz2  File has unexpected size (380598 != 380229). Mirror sync in progress?"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    WITH_OBSERVABILITY=1
    CALLS="${BATS_TEST_TMPDIR}/calls"
    : >"${CALLS}"
    # How many `apt-get update` calls fail before one succeeds; a large
    # number means every one fails.
    UPDATE_FAILURES=0
    INSTALL_FAILS=0

    apt-get() { :; }
    systemctl() { :; }
    curl() { return 0; }
    dpkg() { return 1; }
    _obs_apt_repos() { return 0; }
    _generate_api_token() { echo token; }
    _obs_write_influx_dropin() { return 1; }
    sleep() { echo "sleep $*" >>"${CALLS}"; }
    _apt() {
        echo "apt $*" >>"${CALLS}"
        if [[ $1 == update ]]; then
            local done
            done=$(grep -c '^apt update' "${CALLS}")
            if [[ ${done} -le ${UPDATE_FAILURES} ]]; then
                echo "${MIRROR_ERROR}" >&2
                echo "E: Some index files failed to download. They have been ignored, or old ones used instead." >&2
                return 100
            fi
            return 0
        fi
        if [[ ${INSTALL_FAILS} -eq 1 ]]; then
            echo "E: Unable to locate package grafana" >&2
            return 100
        fi
        return 0
    }
}

@test "669: a failed apt-get update is retried, and the packages are installed" {
    UPDATE_FAILURES=1
    run phase_observability
    [ "$status" -eq 0 ]
    [ "$(grep -c '^apt update' "${CALLS}")" -eq 2 ]
    grep -qx 'apt install -y -qq influxdb2 influxdb2-cli grafana' "${CALLS}"
    [[ "${output}" != *"package install failed"* ]]
}

@test "669: an apt-get update that keeps failing names the step and apt's first error" {
    UPDATE_FAILURES=99
    run phase_observability
    [ "$status" -eq 0 ]
    [ "$(grep -c '^apt update' "${CALLS}")" -eq 3 ]
    if grep -q '^apt install' "${CALLS}"; then return 1; fi
    [[ "${output}" == *"observability: apt-get update failed after 3 attempts"* ]]
    # The first E: line names the repo and the cause; the last is apt's
    # generic "Some index files failed to download".
    [[ "${output}" == *"apt-get update failed after 3 attempts: ${MIRROR_ERROR}"* ]]
}

@test "669: the retries wait a bounded total, about a minute" {
    UPDATE_FAILURES=99
    run phase_observability
    [ "$status" -eq 0 ]
    local total
    total=$(awk '/^sleep / { s += $2 } END { print s + 0 }' "${CALLS}")
    [ "${total}" -gt 0 ]
    [ "${total}" -le 60 ]
}

@test "669: an apt-get install that fails names the step and apt's first error" {
    INSTALL_FAILS=1
    run phase_observability
    [ "$status" -eq 0 ]
    [ "$(grep -c '^apt update' "${CALLS}")" -eq 1 ]
    [[ "${output}" == *"observability: apt-get install failed"* ]]
    [[ "${output}" == *"E: Unable to locate package grafana"* ]]
}
