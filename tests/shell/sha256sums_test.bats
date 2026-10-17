#!/usr/bin/env bats
# #408: _stage_fetch checks a downloaded release tarball against the release's
# SHA256SUMS. These source install.sh (main() is guarded) and replace curl with
# a function that serves files from a fake release dir, so no network is used.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    GITHUB_TOKEN=""
    RELEASE="${BATS_TEST_TMPDIR}/release"
    DL="${BATS_TEST_TMPDIR}/dl"
    CURL_LOG="${BATS_TEST_TMPDIR}/curl.log"
    mkdir -p "${RELEASE}" "${DL}"

    ASSET="anolis-provider-foo-1.0.0-linux-arm64.tar.gz"
    printf 'tarball bytes' > "${RELEASE}/${ASSET}"
    GOOD_SHA=$(sha256sum "${RELEASE}/${ASSET}" | awk '{print $1}')

    # Serve <url basename> from ${RELEASE}; a missing file is curl -f's 22.
    curl() {
        local out="" url="" a
        while [[ $# -gt 0 ]]; do
            a="$1"; shift
            case "${a}" in
                -o) out="$1"; shift ;;
                -H) shift ;;
                -*) ;;
                *)  url="${a}" ;;
            esac
        done
        printf '%s\n' "${url}" >> "${CURL_LOG}"
        [[ -f "${RELEASE}/${url##*/}" ]] || return 22
        cp "${RELEASE}/${url##*/}" "${out}"
    }
}

@test "#408 a tarball matching its SHA256SUMS entry passes" {
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/${ASSET}" ]
    [[ "$output" != *"not verified"* ]]
    grep -q '/v1.0.0/SHA256SUMS$' "${CURL_LOG}"
}

@test "#408 a tarball not matching its SHA256SUMS entry fails, naming both hashes" {
    local bad
    bad=$(printf 'other bytes' | sha256sum | awk '{print $1}')
    printf '%s  %s\n' "${bad}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -ne 0 ]
    [[ "$output" == *"${ASSET}"* ]]
    [[ "$output" == *"expected ${bad}"* ]]
    [[ "$output" == *"got ${GOOD_SHA}"* ]]
}

@test "#408 a SHA256SUMS with no entry for the tarball warns and continues" {
    printf '%s  %s\n' "${GOOD_SHA}" "anolis-provider-foo-1.0.0-linux-x86_64.tar.gz" \
        > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/${ASSET}" ]
    [[ "$output" == *"anolishq/foo v1.0.0 has no entry for ${ASSET}"* ]]
}

@test "#408 a release with no SHA256SUMS warns and continues" {
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/${ASSET}" ]
    [[ "$output" == *"no SHA256SUMS fetched for anolishq/foo v1.0.0; ${ASSET} is not verified"* ]]
}

@test "#408 install.sh is not checked against SHA256SUMS" {
    printf '#!/bin/bash\n' > "${RELEASE}/install.sh"
    run _stage_fetch anolishq/foo v1.0.0 install.sh "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/install.sh" ]
    ! grep -q 'SHA256SUMS' "${CURL_LOG}"
}
