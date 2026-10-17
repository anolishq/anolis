#!/usr/bin/env bats
# Native-architecture detection tests for tools/install.sh.
#
# Unlike the other suites (which hardcode TARGET_ARCH or hit the dry-run path),
# these call resolve_target_arch / phase_detect with NO override so the real
# `uname -m` mapping runs. On the arm64 CI leg (anolishq/.github#104) this proves
# the aarch64->arm64 branch handles actual Pi-class hardware; on x86_64 it proves
# that branch. These source install.sh (main() is guarded) — no root/network.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
}

# Expected bundle arch for whatever host this test runs on.
_expected_host_arch() {
    case "$(uname -m)" in
        aarch64|arm64) printf 'arm64' ;;
        x86_64)        printf 'x86_64' ;;
        *)             printf 'unsupported' ;;
    esac
}

@test "resolve_target_arch maps the native host arch (no --arch override)" {
    local expected
    expected="$(_expected_host_arch)"
    [ "${expected}" != "unsupported" ] || skip "unmapped host arch $(uname -m)"

    TARGET_ARCH=""
    run resolve_target_arch
    [ "$status" -eq 0 ]
    [ "$output" = "${expected}" ]
}

@test "resolve_target_arch honors an explicit --arch override over the host" {
    TARGET_ARCH="arm64"
    run resolve_target_arch
    [ "$status" -eq 0 ]
    [ "$output" = "arm64" ]
}

@test "phase_detect sets ARCH to the mapped native host arch" {
    local expected
    expected="$(_expected_host_arch)"
    [ "${expected}" != "unsupported" ] || skip "unmapped host arch $(uname -m)"

    ARCH=""
    phase_detect
    [ "${ARCH}" = "${expected}" ]
}

# --- #407: an explicit --arch is validated against the release asset names ---

@test "#407 --arch aarch64 maps to the arm64 release asset name" {
    TARGET_ARCH="aarch64"
    run resolve_target_arch
    [ "$status" -eq 0 ]
    [ "$output" = "arm64" ]
}

@test "#407 --arch x86_64 is accepted as given" {
    TARGET_ARCH="x86_64"
    run resolve_target_arch
    [ "$status" -eq 0 ]
    [ "$output" = "x86_64" ]
}

@test "#407 an unsupported --arch dies naming the valid values" {
    TARGET_ARCH="armv7l"
    run resolve_target_arch
    [ "$status" -ne 0 ]
    [[ "$output" == *"unsupported --arch 'armv7l' (arm64|x86_64)"* ]]
}

@test "#407 --stage with an unsupported --arch dies before any download" {
    # Run the script itself (main + set -e), with curl stubbed to log each URL.
    local stub="${BATS_TEST_TMPDIR}/bin" log="${BATS_TEST_TMPDIR}/curl.log"
    mkdir -p "${stub}"
    printf '#!/bin/sh\necho "$*" >> "%s"\nexit 22\n' "${log}" > "${stub}/curl"
    chmod +x "${stub}/curl"
    run env -u ANOLIS_INSTALL_SH_NO_MAIN PATH="${stub}:${PATH}" bash "${INSTALL_SH}" \
        --stage "${BATS_TEST_TMPDIR}/out" \
        --project "${BATS_TEST_DIRNAME}/fixtures/stage-project" --arch armv7l
    [ "$status" -ne 0 ]
    [[ "$output" == *"unsupported --arch 'armv7l' (arm64|x86_64)"* ]]
    [ ! -e "${log}" ]
}
