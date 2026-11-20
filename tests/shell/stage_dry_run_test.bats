#!/usr/bin/env bats
# --stage with --dry-run (#532). The stage branch ran before any DRY_RUN check,
# so a "dry run" of a stage downloaded every component and wrote the bundle.
# The combination is refused before anything runs.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

@test "532: --stage with --dry-run is refused and writes nothing" {
    out="${BATS_TEST_TMPDIR}/out"
    mkdir -p "${BATS_TEST_TMPDIR}/project"
    run bash "${INSTALL_SH}" --stage "${out}" --project "${BATS_TEST_TMPDIR}/project" --dry-run
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"--dry-run cannot be combined with --stage"* ]]
    [ ! -e "${out}" ]
}

@test "532: --help says --dry-run does not apply to --stage" {
    run bash "${INSTALL_SH}" --help
    [[ "$(sed -n '/--dry-run/,/--prefix/p' <<< "${output}" | tr -s ' \n' ' ')" == *"not with --stage"* ]]
}
