#!/usr/bin/env bats
# The main-failure reporter (#504): one open ci-main issue per workflow, a
# comment on it for each later failure. gh is a stub that records its
# arguments and answers `issue list` from ISSUES; nothing reaches GitHub.

SCRIPT="${BATS_TEST_DIRNAME}/../../.github/scripts/report-main-failure.sh"

setup() {
    export WORKFLOW="Extended Checks" RUN_URL="https://example.invalid/run/1" SHA="abc1234"
    export GH_LOG="${BATS_TEST_TMPDIR}/gh.log" ISSUES=""
    mkdir -p "${BATS_TEST_TMPDIR}/bin"
    cat > "${BATS_TEST_TMPDIR}/bin/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${GH_LOG}"
if [[ "$1 $2" == "issue list" ]]; then
    [[ -n "${LIST_FAILS:-}" ]] && exit 1
    printf '%b' "${ISSUES}"
fi
STUB
    chmod +x "${BATS_TEST_TMPDIR}/bin/gh"
    export PATH="${BATS_TEST_TMPDIR}/bin:${PATH}"
}

@test "504: a first failure opens an issue labelled ci-main" {
    run bash "${SCRIPT}"
    [ "${status}" -eq 0 ]
    grep -q '^issue create --title main: Extended Checks failed --label ci-main --body Extended Checks failed on main at abc1234: https://example.invalid/run/1' "${GH_LOG}"
    ! grep -q '^issue comment' "${GH_LOG}"
}

@test "504: a later failure comments on the open issue for that workflow" {
    ISSUES='12\tmain: Hardening failed\n34\tmain: Extended Checks failed\n'
    run bash "${SCRIPT}"
    [ "${status}" -eq 0 ]
    grep -q '^issue comment 34 --body Extended Checks failed on main at abc1234' "${GH_LOG}"
    ! grep -q '^issue create' "${GH_LOG}"
}

@test "504: another workflow's open issue is not reused" {
    ISSUES='12\tmain: Hardening failed\n'
    run bash "${SCRIPT}"
    [ "${status}" -eq 0 ]
    grep -q '^issue create --title main: Extended Checks failed' "${GH_LOG}"
}

@test "504: a title that only starts the same is not reused" {
    ISSUES='56\tmain: Extended Checks failed twice\n'
    run bash "${SCRIPT}"
    [ "${status}" -eq 0 ]
    grep -q '^issue create' "${GH_LOG}"
}

@test "504: a missing input stops before calling gh" {
    unset RUN_URL
    run bash "${SCRIPT}"
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"RUN_URL is required"* ]]
    [ ! -s "${GH_LOG}" ]
}

@test "504: a failed issue listing stops the script and opens no duplicate" {
    export LIST_FAILS=1
    run bash "${SCRIPT}"
    [ "${status}" -ne 0 ]
    ! grep -q '^issue create' "${GH_LOG}"
    ! grep -q '^issue comment' "${GH_LOG}"
}
