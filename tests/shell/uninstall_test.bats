#!/usr/bin/env bats
# Tests for `install.sh --uninstall` keeping recorded runtime data (#304).
#
# The run journal lives at <prefix>/anolis-data. Before #303 it never came up
# on an installed machine, so `rm -rf <prefix>` lost nothing; now it does. The
# uninstall must keep recorded runs the way it already keeps observability
# data, and say so. These exercise the pure helper on a scratch tree — no root.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    P="${BATS_TEST_TMPDIR}/prefix"
    mkdir -p "${P}/bin" "${P}/config/providers" "${P}/projects/x" "${P}/.prev"
    touch "${P}/bin/anolis-runtime" "${P}/config/runtime.yaml" "${P}/manifest.json" "${P}/.prev/anolis-runtime"
}

@test "304: a non-empty anolis-data survives; everything else, dotfiles included, is removed" {
    mkdir -p "${P}/anolis-data/runs"
    printf '{"run":1}\n' > "${P}/anolis-data/runs/index.jsonl"

    run _remove_prefix_keeping_data "${P}"
    [ "$status" -eq 0 ]
    [ "${output}" = "${P}/anolis-data" ]

    [ -f "${P}/anolis-data/runs/index.jsonl" ]
    [ ! -e "${P}/bin" ]
    [ ! -e "${P}/config" ]
    [ ! -e "${P}/projects" ]
    [ ! -e "${P}/manifest.json" ]
    [ ! -e "${P}/.prev" ]
    # Only the kept directory remains under the prefix.
    [ "$(ls -A "${P}")" = "anolis-data" ]
}

@test "304: an empty anolis-data is not worth keeping — the whole prefix goes" {
    mkdir -p "${P}/anolis-data"
    run _remove_prefix_keeping_data "${P}"
    [ "$status" -eq 0 ]
    [ -z "${output}" ]
    [ ! -e "${P}" ]
}

@test "304: no anolis-data at all — the whole prefix goes (pre-#303 machines)" {
    run _remove_prefix_keeping_data "${P}"
    [ "$status" -eq 0 ]
    [ -z "${output}" ]
    [ ! -e "${P}" ]
}
