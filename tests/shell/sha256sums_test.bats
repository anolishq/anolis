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

    # Serve <url basename> from ${RELEASE}. A missing file is curl -f's 22
    # with HTTP 404. SUMS_FAIL="<exit> <http_code>" makes the SHA256SUMS
    # request fail that way instead. -w '%{http_code}' prints the code, as
    # real curl does under -f.
    SUMS_FAIL=""
    curl() {
        local out="" url="" w="" a code=200 rc=0
        while [[ $# -gt 0 ]]; do
            a="$1"; shift
            case "${a}" in
                -o) out="$1"; shift ;;
                -w) w="$1"; shift ;;
                -H) shift ;;
                -*) ;;
                *)  url="${a}" ;;
            esac
        done
        printf '%s\n' "${url}" >> "${CURL_LOG}"
        if [[ -n "${SUMS_FAIL}" && "${url}" == */SHA256SUMS ]]; then
            read -r rc code <<< "${SUMS_FAIL}"
        elif [[ -f "${RELEASE}/${url##*/}" ]]; then
            cp "${RELEASE}/${url##*/}" "${out}"
        else
            rc=22 code=404
        fi
        [[ "${w}" == '%{http_code}' ]] && printf '%s' "${code}"
        return "${rc}"
    }
}

@test "#408 a tarball matching its SHA256SUMS entry passes" {
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/${ASSET}" ]
    [[ "$output" != *"unverified"* ]]
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

@test "#408 a SHA256SUMS with no entry for the tarball fails closed" {
    printf '%s  %s\n' "${GOOD_SHA}" "anolis-provider-foo-1.0.0-linux-x86_64.tar.gz" \
        > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHA256SUMS of anolishq/foo v1.0.0 has no entry for ${ASSET}; refusing to use it unverified"* ]]
}

@test "#408 a release with no SHA256SUMS (HTTP 404) fails closed" {
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"anolishq/foo v1.0.0 publishes no SHA256SUMS; refusing to use ${ASSET} unverified"* ]]
    [ ! -e "${DL}/${ASSET}.SHA256SUMS" ]
}

# #508: the bundled install.sh is checked like a tarball. SHA256SUMS has listed
# it since anolis 0.1.50 (#449); an older release that does not is a warning
# naming the version, as anolis-workbench's release_verify.py gives it.

@test "#508 an install.sh matching its SHA256SUMS entry passes" {
    printf '#!/bin/bash\n' > "${RELEASE}/install.sh"
    printf '%s  %s\n' "$(sha256sum "${RELEASE}/install.sh" | awk '{print $1}')" install.sh > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/anolis v1.0.0 install.sh "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/install.sh" ]
    [[ "$output" != *"not verified"* ]]
    grep -q '/v1.0.0/SHA256SUMS$' "${CURL_LOG}"
}

@test "#508 an install.sh not matching its SHA256SUMS entry fails, naming both hashes" {
    local good bad
    printf '#!/bin/bash\n' > "${RELEASE}/install.sh"
    good=$(sha256sum "${RELEASE}/install.sh" | awk '{print $1}')
    bad=$(printf 'other bytes' | sha256sum | awk '{print $1}')
    printf '%s  %s\n' "${bad}" install.sh > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/anolis v1.0.0 install.sh "${DL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"install.sh does not match SHA256SUMS of anolishq/anolis v1.0.0: expected ${bad}, got ${good}"* ]]
}

@test "#508 an install.sh with no SHA256SUMS line warns, naming the version" {
    printf '#!/bin/bash\n' > "${RELEASE}/install.sh"
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    run _stage_fetch anolishq/anolis v0.1.49 install.sh "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/install.sh" ]
    [[ "$output" == *"install.sh not verified: anolis v0.1.49 predates install.sh in SHA256SUMS"* ]]
}

@test "#508 an install.sh from a release with no SHA256SUMS (HTTP 404) warns" {
    printf '#!/bin/bash\n' > "${RELEASE}/install.sh"
    run _stage_fetch anolishq/anolis v0.1.40 install.sh "${DL}"
    [ "$status" -eq 0 ]
    [ -f "${DL}/install.sh" ]
    [[ "$output" == *"install.sh not verified: anolis v0.1.40 predates install.sh in SHA256SUMS"* ]]
}

@test "#508 an install.sh whose SHA256SUMS cannot be fetched fails closed" {
    printf '#!/bin/bash\n' > "${RELEASE}/install.sh"
    SUMS_FAIL="22 503"
    run _stage_fetch anolishq/anolis v1.0.0 install.sh "${DL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not fetch SHA256SUMS for anolishq/anolis v1.0.0 (curl exit 22, HTTP 503); refusing to use install.sh unverified"* ]]
}

@test "#408 a connection failure fetching SHA256SUMS fails closed" {
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    SUMS_FAIL="7 000"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not fetch SHA256SUMS for anolishq/foo v1.0.0 (curl exit 7, HTTP 000)"* ]]
    [[ "$output" != *"publishes no SHA256SUMS"* ]]
    [ ! -e "${DL}/${ASSET}.SHA256SUMS" ]
}

@test "#408 an HTTP 5xx fetching SHA256SUMS fails closed" {
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    SUMS_FAIL="22 503"
    run _stage_fetch anolishq/foo v1.0.0 "${ASSET}" "${DL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not fetch SHA256SUMS for anolishq/foo v1.0.0 (curl exit 22, HTTP 503)"* ]]
    [[ "$output" != *"publishes no SHA256SUMS"* ]]
}

@test "#408 --stage stops and writes no bundle when SHA256SUMS cannot be fetched" {
    # Run the script itself (main + set -e). The curl stub serves any asset
    # but refuses the SHA256SUMS connection, as curl does with exit 7.
    local stub="${BATS_TEST_TMPDIR}/bin" out="${BATS_TEST_TMPDIR}/out"
    mkdir -p "${stub}" "${out}"
    cat > "${stub}/curl" <<'STUB'
#!/bin/bash
out="" w="" url=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        -w) w="$2"; shift 2 ;;
        -H) shift 2 ;;
        -*) shift ;;
        *)  url="$1"; shift ;;
    esac
done
if [[ "${url}" == */SHA256SUMS ]]; then
    [[ -n "${w}" ]] && printf '000'
    exit 7
fi
printf 'bytes' > "${out}"
[[ -n "${w}" ]] && printf '200'
exit 0
STUB
    chmod +x "${stub}/curl"
    run env -u ANOLIS_INSTALL_SH_NO_MAIN PATH="${stub}:${PATH}" bash "${INSTALL_SH}" \
        --stage "${out}" \
        --project "${BATS_TEST_DIRNAME}/fixtures/stage-project" --arch x86_64
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not fetch SHA256SUMS for anolishq/anolis v9.9.9 (curl exit 7, HTTP 000)"* ]]
    [ -z "$(ls -A "${out}")" ]
}
