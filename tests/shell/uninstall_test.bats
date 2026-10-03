#!/usr/bin/env bats
# Tests for `install.sh --uninstall` keeping recorded runtime data (#304), and
# for the guard on the path it removes.
#
# The run journal lives at <prefix>/anolis-data. Before #303 it never came up
# on an installed machine, so `rm -rf <prefix>` lost nothing; now it does. The
# uninstall must keep recorded runs the way it already keeps observability
# data, and say so.
#
# The removal is recursive and runs as root, so the path is refused unless it
# is provably an install prefix. An earlier draft of this fix deleted the
# prefix's children by glob, which turns an empty or `//` prefix into every
# top-level directory on the machine.
#
# rm and mv are stubbed in every test: each call is recorded, and carried out
# only when every path operand lies strictly inside this test's fixture. A
# refusal test therefore cannot delete anything even when the guard under test
# is broken -- the guard must not be its own test's safety.
#
# Not covered on its own: the two-component check. Every input that collapses
# to `/` or a top-level directory also lacks bin/anolis-runtime, and a test
# cannot build a marked install at the top level without writing to `/`. The
# marker check refuses those inputs here; the component check stands behind it.

bats_require_minimum_version 1.5.0

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u

    FIX="$(mktemp -d "${BATS_TEST_TMPDIR}/fx.XXXXXX")"
    [[ -d "${FIX}" && "${FIX}" == /*/* ]] || return 1
    CALLS="${BATS_TEST_TMPDIR}/calls.log"
    : >"${CALLS}"

    # Only the root filesystem is mounted, unless a test says otherwise.
    export ANOLIS_MOUNTINFO="${BATS_TEST_TMPDIR}/mountinfo"
    printf '1 0 8:1 / / rw - ext4 /dev/sda1 rw\n' >"${ANOLIS_MOUNTINFO}"

    P="${FIX}/opt/anolis"
    mkdir -p "${P}/bin" "${P}/config/providers" "${P}/projects/x" "${P}/.prev"
    touch "${P}/bin/anolis-runtime" "${P}/config/runtime.yaml" "${P}/manifest.json" "${P}/.prev/anolis-runtime"
    touch "${FIX}/sentinel"
}

# bats removes its own output file after teardown; let it.
teardown() {
    unset -f rm mv
}

_confined() {
    local a
    for a in "$@"; do
        [[ "${a}" == -* ]] && continue
        [[ "${a}" == "${FIX}"/?* && "${a}" != *..* ]] || return 1
    done
}

rm() {
    printf 'rm %s\n' "$*" >>"${CALLS}"
    _confined "$@" || { echo "stub refused: rm $*" >&2; return 99; }
    command rm "$@"
}

mv() {
    printf 'mv %s\n' "$*" >>"${CALLS}"
    _confined "$@" || { echo "stub refused: mv $*" >&2; return 99; }
    command mv "$@"
}

# The guard refused: non-zero, nothing removed or moved, fixture untouched.
# Every check returns explicitly, so this holds when called under `||`, where
# errexit is off.
assert_refused() {
    [ "$status" -ne 0 ] || return 1
    [ -z "${output}" ] || return 1
    [ ! -s "${CALLS}" ] || return 1
    [ -f "${FIX}/sentinel" ] || return 1
    [ -f "${P}/bin/anolis-runtime" ] || return 1
}

# Each input on its own: the call log is reset first, and a failure names it.
assert_each_refused() {
    local input
    for input in "$@"; do
        : >"${CALLS}"
        run --separate-stderr _remove_prefix_keeping_data "${input}"
        assert_refused || { echo "not refused: '${input}' (status ${status})" >&2; return 1; }
    done
}

@test "304: a non-empty anolis-data survives; everything else, dotfiles included, is removed" {
    mkdir -p "${P}/anolis-data/runs"
    printf '{"run":1}\n' >"${P}/anolis-data/runs/index.jsonl"
    chmod 750 "${P}"

    run --separate-stderr _remove_prefix_keeping_data "${P}"
    [ "$status" -eq 0 ]
    [ "${output}" = "${P}/anolis-data" ]

    [ -f "${P}/anolis-data/runs/index.jsonl" ]
    # Only the kept directory remains under the prefix, nothing was left beside
    # it, and the recreated prefix kept its mode.
    [ "$(ls -A "${P}")" = "anolis-data" ]
    [ "$(ls -A "${FIX}/opt")" = "anolis" ]
    [ "$(stat -c '%a' "${P}")" = "750" ]
}

@test "304: an empty anolis-data is not worth keeping -- the whole prefix goes" {
    mkdir -p "${P}/anolis-data"
    run --separate-stderr _remove_prefix_keeping_data "${P}"
    [ "$status" -eq 0 ]
    [ -z "${output}" ]
    [ ! -e "${P}" ]
}

@test "304: no anolis-data at all -- the whole prefix goes (pre-#303 machines)" {
    run --separate-stderr _remove_prefix_keeping_data "${P}"
    [ "$status" -eq 0 ]
    [ -z "${output}" ]
    [ ! -e "${P}" ]
}

@test "304: the prefix is removed by its canonical path, as one rm that stays on its filesystem" {
    mkdir -p "${FIX}/opt/other"
    run --separate-stderr _remove_prefix_keeping_data "${FIX}/opt/other/../anolis"
    [ "$status" -eq 0 ]
    [ ! -e "${P}" ]
    [ "$(cat "${CALLS}")" = "rm -rf --one-file-system -- ${P}" ]
}

@test "304: an empty, unset, relative or root-collapsing prefix is refused" {
    assert_each_refused "" "." ".." "relative/path" "/" "//" "///" "///x" "/./" "/a/.." "/opt" "/opt/"
    : >"${CALLS}"
    run --separate-stderr _remove_prefix_keeping_data
    assert_refused
}

@test "304: a relative path is refused even when it names a real install" {
    cd "${FIX}/opt"
    assert_each_refused "anolis" "./anolis"
}

@test "304: a path with a control character is refused" {
    mkdir -p "${P}"$'\n'
    assert_each_refused "${P}"$'\n' "${P}"$'\t'
}

@test "304: a symlink to an install is refused, however it is spelled, and the install survives" {
    ln -s "${P}" "${FIX}/link"
    mkdir -p "${FIX}/x"
    assert_each_refused "${FIX}/link" "${FIX}/link/" "${FIX}/link//" "${FIX}/link/." "${FIX}/link/projects/.."
    [ -L "${FIX}/link" ]
}

@test "304: a directory that is not an anolis install is refused, manifest.json or not" {
    command rm "${P}/bin/anolis-runtime"
    run --separate-stderr _remove_prefix_keeping_data "${P}"
    [ "$status" -ne 0 ]
    [ ! -s "${CALLS}" ]
    [ -f "${P}/manifest.json" ]
}

@test "304: \$HOME, or a directory holding it, is refused" {
    HOME="${P}" assert_each_refused "${P}"
    HOME="${P}/projects/x" assert_each_refused "${P}"
}

@test "304: a prefix that is a mount root is refused" {
    printf '2 1 8:2 / %s rw - ext4 /dev/sda2 rw\n' "${P}" >>"${ANOLIS_MOUNTINFO}"
    assert_each_refused "${P}"
}

@test "304: a prefix with a mount below it is refused" {
    printf '3 1 8:3 / %s rw - ext4 /dev/sda3 rw\n' "${P}/anolis-data" >>"${ANOLIS_MOUNTINFO}"
    assert_each_refused "${P}"
}

@test "304: a mount table entry is matched after unescaping (a space is \\040)" {
    local spaced="${FIX}/opt/ano lis"
    mkdir -p "${spaced}/bin"
    touch "${spaced}/bin/anolis-runtime"
    printf '5 1 8:5 / %s/opt/ano\\040lis rw - ext4 /dev/sda5 rw\n' "${FIX}" >>"${ANOLIS_MOUNTINFO}"
    assert_each_refused "${spaced}"
    [ -f "${spaced}/bin/anolis-runtime" ]
}

@test "304: an unreadable mount table is refused, not skipped" {
    ANOLIS_MOUNTINFO="${BATS_TEST_TMPDIR}/no-such-mountinfo" assert_each_refused "${P}"
}
