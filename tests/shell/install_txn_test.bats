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
    UNPACKED="${BATS_TEST_TMPDIR}/unpacked-bundle"
    _stubs
    _old_install
    _new_bundle "${UNMET_EXIT}"
}

# The stubs, as one function, so a test can also write them into a script that
# runs main in a process of its own (see _run_main_process).
_stubs() {
    phase_root_check() { :; }
    phase_detect() { ARCH=x86_64; }
    # As the real phase does for a tarball: a temp dir the EXIT trap removes.
    phase_prepare_bundle() {
        BUNDLE_DIR="${BUNDLE}"
        mkdir -p "${UNPACKED}"
        INSTALL_TMP="${UNPACKED}"
        trap _install_on_exit EXIT
    }
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
}

# bats `run` evaluates its command where errexit is off, so a `set -e` abort
# or a signal can only be seen with main in a process of its own, with
# install.sh's own `set -euo pipefail`. <fault> is shell code run in place of
# phase_runtime_auth, after the binaries and project were replaced.
_run_main_process() {
    local fault="$1" prelude="${2:-}" driver="${BATS_TEST_TMPDIR}/driver.sh"
    {
        echo '#!/usr/bin/env bash'
        echo 'export ANOLIS_INSTALL_SH_NO_MAIN=1'
        printf 'source %q\n' "${INSTALL_SH}"
        printf 'PFX=%q BUNDLE=%q LATER=%q UNPACKED=%q\n' "${PFX}" "${BUNDLE}" "${LATER}" "${UNPACKED}"
        declare -f _stubs
        echo '_stubs'
        printf 'phase_runtime_auth() { %s; }\n' "${fault}"
        printf 'MARK=%q\n' "${BATS_TEST_TMPDIR}/mark"
        printf '%s\n' "${prelude}"
        echo 'main --local "${BUNDLE}" --prefix "${PFX}" --no-start'
    } > "${driver}"
    run bash "${driver}"
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

# Path, mode, mtime and content of every file and directory under the
# prefix's bin/, config/, projects/ and .prev/.
_tree() {
    ( cd "${PFX}" && find bin config projects .prev -printf '%p %M %T@\n' | LC_ALL=C sort \
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
    # after phase_install_binaries has replaced bin/. (The kept config's http
    # block is its last, so this line joins it.)
    printf '  bind: 0.0.0.0\n' >> "${PFX}/config/runtime.yaml"
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

# Run under `run`, so the EXIT trap _install_txn_begin sets stays in its subshell.
_subshell_die() {
    PREFIX="${PFX}"
    _install_txn_begin
    echo changed > "${PFX}/bin/anolis-runtime"
    ( die "in a subshell" ) || true
    grep -qx changed "${PFX}/bin/anolis-runtime" || return 11
    [ -d "${INSTALL_TXN}" ] || return 12
    _install_txn_restore >/dev/null
    grep -q '1.0.0' "${PFX}/bin/anolis-runtime" || return 13
    [ ! -e "${PFX}/.install-txn" ] || return 14
}

@test "535: a die in a subshell does not restore under the install's shell" {
    run _subshell_die
    [ "$status" -eq 0 ]
}

@test "535: a copy left by an earlier install is never overwritten: the install refuses before it changes anything" {
    mkdir -p "${PFX}/.install-txn/bin"
    echo "the only record" > "${PFX}/.install-txn/bin/anolis-runtime"
    # The first phase that changes the host must not run.
    phase_system_user() { echo phase_system_user >> "${LATER}"; }
    before=$(_tree)
    _main
    [ "$status" -ne 0 ]
    [[ "${output}" == *"install: ${PFX}/.install-txn exists: an earlier install stopped before its host preflight passed"* ]]
    [[ "${output}" == *"sudo rm -rf ${PFX}/<dir> && sudo mv ${PFX}/.install-txn/<dir> ${PFX}/<dir>"* ]]
    [[ "${output}" == *"sudo rm -rf ${PFX}/.install-txn"* ]]
    [[ "${output}" == *"refusing to install over an unfinished install"* ]]
    [[ "${output}" != *"install: restored"* ]]
    grep -qx "the only record" "${PFX}/.install-txn/bin/anolis-runtime"
    _tree_is "${before}"
    [ ! -e "${LATER}" ]
}

@test "535: a die after the preflight passed keeps the new install, and restores nothing" {
    _new_bundle 0
    phase_systemd() { die "systemd: failed"; }
    _main --no-start
    [ "$status" -ne 0 ]
    [[ "${output}" == *"systemd: failed"* ]]
    [[ "${output}" != *"install: restored"* ]]
    [[ "${output}" != *"could not restore"* ]]
    grep -q '2.0.0' "${PFX}/bin/anolis-runtime"
    grep -q 'device: new' "${PFX}/projects/demo/config/provider-demo.yaml"
    [ ! -e "${PFX}/.install-txn" ]
    [ ! -e "${UNPACKED}" ]
}

@test "535: a restore whose copy is missing deletes nothing, says so, and keeps the rest of the copy" {
    _missing_copy() {
        PREFIX="${PFX}"
        _install_txn_begin
        rm -rf "${PFX}/.install-txn/projects"
        echo "written by this run" > "${PFX}/projects/demo/new"
        echo changed > "${PFX}/bin/anolis-runtime"
        die "preflight failed"
    }
    run _missing_copy
    [ "$status" -eq 1 ]
    [[ "${output}" == *"install: could not restore projects under ${PFX}; the copies taken before this run are in ${PFX}/.install-txn"* ]]
    [[ "${output}" != *"install: restored"* ]]
    # projects/ had no copy, so it was left alone, not deleted.
    grep -qx "written by this run" "${PFX}/projects/demo/new"
    # The others came back.
    grep -q '1.0.0' "${PFX}/bin/anolis-runtime"
    [ -d "${PFX}/.install-txn" ]
    # And the next install refuses to go on over it.
    _main
    [[ "${output}" == *"${PFX}/.install-txn exists"* ]]
}

@test "535: a command that fails under set -e restores the prefix, and the temp dir is still removed" {
    before=$(_tree)
    _run_main_process 'false'
    [ "$status" -eq 1 ]
    [[ "${output}" == *"install: restored ${PFX}/bin, config/, projects/ and .prev/ as they were before this run"* ]]
    _tree_is "${before}"
    [ ! -e "${PFX}/.install-txn" ]
    [ ! -e "${UNPACKED}" ]
}

@test "535: SIGTERM, SIGHUP and SIGINT restore the prefix and exit with the signal's status" {
    local sig want
    for sig in TERM:143 HUP:129 INT:130; do
        want="${sig#*:}"; sig="${sig%%:*}"
        rm -rf "${PFX}"
        _old_install
        before=$(_tree)
        _run_main_process "kill -${sig} \$\$"
        [ "$status" -eq "${want}" ] || { echo "${sig}: status ${status}"; return 1; }
        [[ "${output}" == *"install: restored"* ]] || { echo "${sig}: ${output}"; return 1; }
        _tree_is "${before}" || { echo "${sig}: tree differs"; return 1; }
        [ ! -e "${PFX}/.install-txn" ]
        [ ! -e "${UNPACKED}" ]
    done
}

@test "535: in a process of its own, a passing install drops the copy and still removes the temp dir" {
    _new_bundle 0
    _run_main_process ':'
    [ "$status" -eq 0 ]
    grep -q '2.0.0' "${PFX}/bin/anolis-runtime"
    [ ! -e "${PFX}/.install-txn" ]
    [ ! -e "${UNPACKED}" ]
    [[ "${output}" != *"install: restored"* ]]
}

# SIGKILL while the copy is taken, part way through projects/: nothing can trap
# it. The partial copy must never be offered as a source to restore from.
KILL_IN_COPY='cp() {
    if [[ "${3:-}" == *"/.install-txn.partial/projects" ]]; then
        command cp -a "$2" "$3"; command rm -rf "$3/demo/config"
        kill -KILL $$
    fi
    command cp "$@"
}'

@test "535: a kill while the copy is taken leaves it partial, and the next install says not to restore from it" {
    before=$(_tree)
    _run_main_process ':' "${KILL_IN_COPY}"
    [ "$status" -eq 137 ]
    # The live prefix was never touched, and the copy was never named as complete.
    _tree_is "${before}"
    [ -d "${PFX}/.install-txn.partial" ]
    [ ! -e "${PFX}/.install-txn" ]
    _main
    [ "$status" -ne 0 ]
    [[ "${output}" == *"${PFX}/.install-txn.partial exists: an earlier install was stopped while it saved the prefix, before it changed anything there"* ]]
    [[ "${output}" == *"That copy is incomplete; do not restore from it."* ]]
    [[ "${output}" == *"sudo rm -rf ${PFX}/.install-txn.partial"* ]]
    [[ "${output}" != *"mv ${PFX}/.install-txn"* ]]
    _tree_is "${before}"
    # Following the instruction leaves the prefix as it was, and installs again.
    rm -rf "${PFX}/.install-txn.partial"
    _tree_is "${before}"
}

@test "535: the stale copy's message lists only the directories it holds" {
    mkdir -p "${PFX}/.install-txn/projects"
    _main
    [[ "${output}" == *"It holds projects/ as they were before that install; the others were put back."* ]]
    [[ "${output}" != *"bin/, config/"* ]]
}

@test "535: a symlink at .install-txn or .install-txn.partial is refused too, and left alone" {
    ln -s "${BATS_TEST_TMPDIR}/nowhere" "${PFX}/.install-txn"
    _main
    [ "$status" -ne 0 ]
    [[ "${output}" == *"${PFX}/.install-txn exists"* ]]
    [[ "${output}" == *"It holds none of bin/, config/, projects/ or .prev/"* ]]
    [ -L "${PFX}/.install-txn" ]
    rm "${PFX}/.install-txn"
    ln -s "${BATS_TEST_TMPDIR}/nowhere" "${PFX}/.install-txn.partial"
    _main
    [ "$status" -ne 0 ]
    [[ "${output}" == *"${PFX}/.install-txn.partial exists"* ]]
    [ -L "${PFX}/.install-txn.partial" ]
}

# The restore is a run of rm and mv; a signal in the middle must not end it.
TERM_IN_RESTORE='mv() {
    if [[ "${2:-}" == "${PFX}/bin" && ! -e "${MARK}" ]]; then : > "${MARK}"; kill -TERM $$; fi
    command mv "$@"
}'

@test "535: a signal during the restore does not stop it" {
    before=$(_tree)
    _run_main_process 'false' "${TERM_IN_RESTORE}"
    [ -e "${BATS_TEST_TMPDIR}/mark" ]  # the signal was sent, from inside the restore
    [ "$status" -eq 1 ]
    [[ "${output}" == *"install: restored"* ]]
    _tree_is "${before}"
    [ ! -e "${PFX}/.install-txn" ]
}

# A command in the handler that fails must not end it under errexit: here the
# removal of the emptied copy fails, after a SIGTERM.
RM_COPY_FAILS='rm() {
    if [[ "$*" == "-rf ${PFX}/.install-txn" ]]; then return 1; fi
    command rm "$@"
}'

@test "535: the exit handler runs to the end with errexit off" {
    _run_main_process 'kill -TERM $$' "${RM_COPY_FAILS}"
    [ "$status" -eq 143 ]
    [[ "${output}" == *"install: restored"* ]]
    # The step after the failed one still ran: the temp dir is gone.
    [ ! -e "${UNPACKED}" ]
}
