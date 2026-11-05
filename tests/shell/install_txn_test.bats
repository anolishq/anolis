#!/usr/bin/env bats
# Tests for the install transaction (#535): the phases from the backup to the
# host preflight replace bin/, the project and the runtime config, and a die
# before the preflight passes puts back what they replaced.
#
# main() runs for real from phase_directories to the host preflight, over a
# fake prefix and bundle in BATS_TEST_TMPDIR. Everything that would reach the
# host is a stub: the root check, the system user, chown, runuser (runs the
# command as us), the API token (it writes /etc/anolis), and every phase after
# the preflight (systemd, hostname, start, ...), which only records that it ran.
# The "binaries" are small scripts.

bats_require_minimum_version 1.5.0

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    PFX="${BATS_TEST_TMPDIR}/prefix"
    BUNDLE="${BATS_TEST_TMPDIR}/bundle"
    LATER="${BATS_TEST_TMPDIR}/later.log"

    phase_root_check() { :; }
    phase_detect() { ARCH=x86_64; }
    phase_prepare_bundle() { BUNDLE_DIR="${BUNDLE}"; }
    phase_verify() { :; }
    phase_version_check() { :; }
    phase_system_user() { :; }
    phase_runtime_auth() { :; }
    chown() { :; }
    runuser() { shift 3; "$@"; }
    local p
    for p in phase_manifest phase_systemd phase_telemetry_export phase_observability \
        phase_hostname phase_start phase_health phase_summary; do
        eval "${p}() { echo ${p} >> '${LATER}'; }"
    done

    _old_install
    _new_bundle "${UNMET_EXIT}"
}

UNMET_EXIT=1

# _script <path> <body>: an executable script.
_script() {
    mkdir -p "$(dirname "$1")"
    printf '#!/usr/bin/env bash\n%s\n' "$2" > "$1"
    chmod +x "$1"
}

# A provider that answers --version and --check-host with exit <rc>.
_provider_body() {
    printf 'case "$1" in\n  --version) echo %s ;;\n  --check-host) echo '\''{"check_host_version":1,"requirements":[]}'\''; exit %s ;;\nesac' "$1" "$2"
}

# The install being upgraded: runtime and provider v1, a project, a kept
# runtime.yaml, and a .prev holding v0. Modes as install.sh leaves them (its
# phase_directories sets 755 on every run, before anything is saved).
_old_install() {
    umask 022
    _script "${PFX}/bin/anolis-runtime" 'echo "anolis-runtime 1.0.0"'
    _script "${PFX}/bin/anolis-provider-demo" "$(_provider_body 1.0.0 0)"
    _script "${PFX}/.prev/anolis-runtime" 'echo "anolis-runtime 0.9.0"'
    mkdir -p "${PFX}/projects/demo/config" "${PFX}/config" "${PFX}/anolis-data"
    echo "device: old" > "${PFX}/projects/demo/config/provider-demo.yaml"
    _runtime_yaml > "${PFX}/config/runtime.yaml"
}

_runtime_yaml() {
    printf '%s\n' "runtime:" "  name: demo" "providers:" "  - id: demo0" \
        "    command: ${PFX}/bin/anolis-provider-demo" \
        "    args: [\"--config\", \"${PFX}/projects/demo/config/provider-demo.yaml\"]" \
        "http:" "  port: 8080"
}

# The incoming bundle: runtime and provider v2, whose --check-host exits <rc>.
_new_bundle() {
    _script "${BUNDLE}/bin/anolis-runtime" 'echo "anolis-runtime 2.0.0"'
    _script "${BUNDLE}/bin/anolis-provider-demo" "$(_provider_body 2.0.0 "$1")"
    mkdir -p "${BUNDLE}/projects/demo/config"
    echo "device: new" > "${BUNDLE}/projects/demo/config/provider-demo.yaml"
    _runtime_yaml > "${BUNDLE}/projects/demo/config/anolis-runtime.manual.yaml"
    printf 'runtime_profiles:\n  manual: config/anolis-runtime.manual.yaml\n' \
        > "${BUNDLE}/projects/demo/machine-profile.yaml"
}

# Path, mode and content of every file and directory under the prefix's
# bin/, config/, projects/ and .prev/.
_tree() {
    ( cd "${PFX}" && find bin config projects .prev -printf '%p %M\n' | LC_ALL=C sort \
        && find bin config projects .prev -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum )
}

# The tree is what it was in <before>; show the difference when it is not.
_tree_is() {
    local now
    now=$(_tree)
    [ "${now}" == "$1" ] || { diff <(printf '%s\n' "$1") <(printf '%s\n' "${now}"); return 1; }
}

_main() {
    run main --local "${BUNDLE}" --prefix "${PFX}" "$@"
}

@test "535: an upgrade whose host preflight fails leaves bin, config, projects and .prev as they were" {
    before=$(_tree)
    _main
    [ "$status" -ne 0 ]
    [[ "${output}" == *"host preflight: requirements unmet"* ]]
    # The preflight ran the incoming binaries: they had been installed.
    [[ "${output}" == *"host preflight: demo0: host requirements unmet"* ]]
    _tree_is "${before}"
    [[ "${output}" == *"install: restored ${PFX}/bin, config/, projects/ and .prev/ as they were before this run"* ]]
    [ ! -e "${PFX}/.install-txn" ]
    [ ! -e "${LATER}" ]
}

@test "535: a refused runtime config after the binaries were replaced is restored too" {
    # A kept runtime.yaml the runtime would refuse: phase_config_runtime dies,
    # after phase_install_binaries has replaced bin/.
    printf 'http:\n  bind: 0.0.0.0\n' >> "${PFX}/config/runtime.yaml"
    before=$(_tree)
    _main
    [ "$status" -ne 0 ]
    [[ "${output}" == *"refusing to install a runtime config that cannot start"* ]]
    _tree_is "${before}"
    [[ "${output}" == *"install: restored"* ]]
    [ ! -e "${PFX}/.install-txn" ]
}

@test "535: an upgrade whose host preflight passes keeps the new install and drops the saved copy" {
    _new_bundle 0
    _main --no-start
    [ "$status" -eq 0 ]
    grep -q '2.0.0' "${PFX}/bin/anolis-runtime"
    grep -q 'device: new' "${PFX}/projects/demo/config/provider-demo.yaml"
    # The backup ran: .prev now holds the v1 runtime.
    grep -q '1.0.0' "${PFX}/.prev/anolis-runtime"
    [[ "${output}" != *"install: restored"* ]]
    [ ! -e "${PFX}/.install-txn" ]
    grep -qx phase_manifest "${LATER}"
}

@test "535: --allow-unmet-host still installs over unmet requirements" {
    _main --allow-unmet-host --no-start
    [ "$status" -eq 0 ]
    grep -q '2.0.0' "${PFX}/bin/anolis-runtime"
    [ ! -e "${PFX}/.install-txn" ]
}

@test "535: a die outside the transaction restores nothing" {
    INSTALL_TXN=""
    run die "boom"
    [ "$status" -eq 1 ]
    [[ "${output}" != *"restored"* ]]
}

@test "535: a die in a subshell does not restore under the install's shell" {
    PREFIX="${PFX}"
    _install_txn_begin
    echo changed > "${PFX}/bin/anolis-runtime"
    ( die "in a subshell" ) || true
    grep -qx changed "${PFX}/bin/anolis-runtime"
    [ -d "${INSTALL_TXN}" ]
    _install_txn_restore >/dev/null
    grep -q '1.0.0' "${PFX}/bin/anolis-runtime"
    [ ! -e "${PFX}/.install-txn" ]
}
