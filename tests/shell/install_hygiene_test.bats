#!/usr/bin/env bats
# What install.sh prints or leaves behind should be true and needed
# (anolishq/anolis#515): apt runs without a terminal, so debconf has no
# frontend to fail to initialise, and a service user kept from an earlier
# install is shown with its groups, which install.sh never grants or revokes.
#
# Source install.sh (main() guarded); every command that would change the
# host is a stub that records what it saw.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"
    # apt-get records the DEBIAN_FRONTEND it was run with.
    apt-get() { printf 'apt-get DEBIAN_FRONTEND=%s %s\n' "${DEBIAN_FRONTEND:-unset}" "$*" >>"${CALLS}"; }
    dpkg() { return 1; }  # nothing installed yet
    local name
    for name in systemctl curl sleep python3 _obs_apt_repos _obs_write_influx_dropin _obs_setup_influx _obs_provision_grafana; do
        eval "${name}() { printf '%s\n' \"${name} \$*\" >>\"\${CALLS}\"; }"
    done
    unset DEBIAN_FRONTEND
}

_apt_calls() { grep '^apt-get ' "${CALLS}"; }

@test "515: observability's apt calls run non-interactive" {
    WITH_OBSERVABILITY=1
    run phase_observability
    [ "$(_apt_calls | wc -l)" -ge 2 ]
    if _apt_calls | grep -v 'DEBIAN_FRONTEND=noninteractive '; then return 1; fi
}

@test "515: telemetry-export's apt calls run non-interactive" {
    WITH_TELEMETRY_EXPORT=1
    LOCAL_PATH=""
    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    run phase_telemetry_export
    [ "$(_apt_calls | wc -l)" -ge 2 ]
    if _apt_calls | grep -v 'DEBIAN_FRONTEND=noninteractive '; then return 1; fi
}

@test "515: a kept service user is shown with its groups and how to drop one" {
    id() {
        case "$*" in
            "anolis") return 0 ;;
            "-nG anolis") echo "anolis dialout gpio i2c" ;;
            *) return 1 ;;
        esac
    }
    run phase_system_user
    [ "$status" -eq 0 ]
    [[ "${output}" == *"system user: anolis already exists, in groups: dialout gpio i2c"* ]]
    [[ "${output}" == *"install.sh grants no groups"* ]]
    [[ "${output}" == *"sudo gpasswd -d anolis <group>"* ]]
}

@test "515: a new service user is not described by groups" {
    id() { return 1; }
    useradd() { printf 'useradd %s\n' "$*" >>"${CALLS}"; }
    run phase_system_user
    [ "$status" -eq 0 ]
    [[ "${output}" == *"created anolis"* ]]
    [[ "${output}" != *"groups"* ]]
}
