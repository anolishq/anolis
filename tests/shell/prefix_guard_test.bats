#!/usr/bin/env bats
# Tests for the --prefix guard on install, rollback and uninstall (#335).
#
# All three modes run as root and act on ${PREFIX}: install chowns the prefix
# recursively and every file in ${PREFIX}/bin, rollback the files in
# ${PREFIX}/bin. With an empty prefix or `/`, that is the system /bin.
# Uninstall stops and deletes every anolis unit and the secrets in
# /etc/anolis whatever the prefix, so a prefix that names no install must
# stop it before it starts.
#
# main runs in-process. Every command that changes the host -- chown, chmod,
# rm, rmdir, install, cp, mv, systemctl, useradd, runuser -- is a stub that
# records its arguments and does nothing else. The phases that act on the
# prefix (directories, install binaries, rollback, uninstall) stay real,
# so a missing guard shows up as a recorded chown on the wrong path; every
# other phase is a stub that records its name. A refusal test therefore cannot
# act on the host even when the guard under test is missing.

bats_require_minimum_version 1.5.0

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

    FIX="$(mktemp -d "${BATS_TEST_TMPDIR}/fx.XXXXXX")"
    [[ -d "${FIX}" && "${FIX}" == /*/* ]] || return 1
    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"

    _stub_cmd chown chmod rm rmdir install cp mv systemctl useradd runuser
    _stub_cmd phase_root_check phase_detect phase_verify preflight_variant_selection \
        phase_system_user phase_backup phase_config_project phase_config_runtime \
        phase_runtime_auth phase_host_preflight phase_manifest \
        phase_systemd phase_telemetry_export phase_observability phase_hostname \
        phase_start phase_health phase_summary do_stage

    # An offline bundle with one binary and no recorded prefix.
    BUNDLE="${FIX}/bundle"
    mkdir -p "${BUNDLE}/bin"
    touch "${BUNDLE}/bin/anolis-runtime"
    printf '{\n  "profile": "x"\n}\n' >"${BUNDLE}/manifest.json"
}

teardown() {
    unset -f chown chmod rm rmdir install cp mv systemctl useradd runuser
}

# Refused: non-zero, the reason names --prefix, and no command was run.
assert_refused() {
    [ "$status" -ne 0 ] || return 1
    [[ "${output}" == *"--prefix"* ]] || return 1
    [ ! -s "${CALLS}" ] || return 1
}

# Each input on its own, for each mode: the call log is reset
# first, and every failure names the input, the mode and the commands that
# were recorded.
assert_each_refused() {
    local input mode failed=0
    for input in "$@"; do
        for mode in install rollback uninstall; do
            : >"${CALLS}"
            if [[ "${mode}" == install ]]; then
                run main --local "${BUNDLE}" --no-start --prefix "${input}"
            else
                run main "--${mode}" --prefix "${input}"
            fi
            assert_refused || {
                failed=1
                echo "not refused: ${mode} --prefix '${input}' (status ${status})" >&2
                grep -E '^(chown|chmod|install|cp|mv|rm|rmdir|systemctl) ' "${CALLS}" | cut -c1-120 | sed 's/^/  recorded: /' >&2 || true
            }
        done
    done
    return "${failed}"
}

@test "335: an empty prefix is refused before anything runs" {
    assert_each_refused ""
}

@test "335: a relative prefix is refused, even one that resolves to a real install" {
    mkdir -p "${FIX}/rel/bin" "${FIX}/rel/.prev"
    touch "${FIX}/rel/bin/anolis-runtime" "${FIX}/rel/.prev/anolis-runtime"
    cd "${FIX}"
    assert_each_refused "rel" "./rel" "rel/"
}

# Uninstall only checked a prefix that exists, so one that does not exist
# from here skipped the guard and the units and secrets went anyway.
@test "335: a relative prefix that does not exist is refused" {
    cd "${FIX}"
    assert_each_refused "opt/anolis" "./gone"
}

@test "335: / and paths that collapse to it are refused" {
    assert_each_refused "/" "//" "/./" "/opt/.."
}

@test "335: a single-component prefix is refused" {
    assert_each_refused "/opt" "/opt/" "/usr"
}

@test "335: \$HOME, or a directory holding it, is refused" {
    mkdir -p "${FIX}/home/user"
    HOME="${FIX}/home/user" assert_each_refused "${FIX}/home/user" "${FIX}/home"
}

@test "335: a symlink prefix is refused, however it is spelled" {
    mkdir -p "${FIX}/real/bin" "${FIX}/real/.prev"
    touch "${FIX}/real/.prev/anolis-runtime"
    ln -s "${FIX}/real" "${FIX}/link"
    assert_each_refused "${FIX}/link" "${FIX}/link/" "${FIX}/link/."
}

@test "335: a prefix baked into a staged bundle gets the same check" {
    printf '{\n  "prefix": "/",\n  "profile": "x"\n}\n' >"${BUNDLE}/manifest.json"
    run main --local "${BUNDLE}" --no-start
    [ "$status" -ne 0 ]
    [[ "${output}" == *"refusing the bundle's staged prefix '/'"* ]]
    if grep -E '^(chown|chmod|install|cp|mv|rm|systemctl) ' "${CALLS}" | cut -c1-120 | sed 's/^/recorded: /' >&2; then
        return 1
    fi
}

@test "335: install accepts a fresh prefix that does not exist yet" {
    local p="${FIX}/opt/anolis"
    run main --local "${BUNDLE}" --no-start --prefix "${p}"
    [ "$status" -eq 0 ]
    grep -qxF "install -d -m 755 ${p} ${p}/bin ${p}/config ${p}/projects ${p}/.prev ${p}/anolis-data" "${CALLS}"
    grep -qxF "chown anolis:anolis ${p}/bin/*" "${CALLS}"
    # Nothing reads <prefix>/config/providers/, so no phase writes it (#515).
    if grep -q '^phase_config_providers' "${CALLS}"; then return 1; fi
}

# Install never removes the prefix, so a prefix that is its own mount is a
# place to install into; only uninstall refuses a mount.
@test "335: install accepts an existing prefix that is a mount root" {
    local p="${FIX}/opt/anolis"
    mkdir -p "${p}/bin"
    export ANOLIS_MOUNTINFO="${BATS_TEST_TMPDIR}/mountinfo"
    printf '1 0 8:1 / / rw - ext4 /dev/sda1 rw\n2 1 8:2 / %s rw - ext4 /dev/sda2 rw\n' "${p}" >"${ANOLIS_MOUNTINFO}"
    run main --local "${BUNDLE}" --no-start --prefix "${p}"
    [ "$status" -eq 0 ]
    grep -qxF "chown -R anolis:anolis ${p}" "${CALLS}"
}

@test "335: rollback accepts an existing install prefix" {
    local p="${FIX}/opt/anolis"
    mkdir -p "${p}/bin" "${p}/.prev"
    touch "${p}/bin/anolis-runtime" "${p}/.prev/anolis-runtime"
    run main --rollback --prefix "${p}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"rollback complete"* ]]
    grep -qxF "chown anolis:anolis ${p}/bin/anolis-runtime" "${CALLS}"
}

# An install whose prefix is already gone still has units and secrets to
# remove.
@test "335: uninstall still runs for an absolute prefix that no longer exists" {
    run main --uninstall --prefix "${FIX}/opt/anolis"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"uninstall complete"* ]]
    grep -qxF "systemctl daemon-reload" "${CALLS}"
}
