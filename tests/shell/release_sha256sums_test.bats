#!/usr/bin/env bats
# #446: the release's SHA256SUMS covers install.sh and the manifests, and the
# release workflow's check fails when it does not. These run
# tools/release-sha256sums.sh on a fake release directory.

SCRIPT="${BATS_TEST_DIRNAME}/../../tools/release-sha256sums.sh"

setup() {
    REL="${BATS_TEST_TMPDIR}/release"
    mkdir -p "${REL}"
    printf 'runtime' > "${REL}/anolis-9.9.9-linux-arm64.tar.gz"
    printf 'runtime' > "${REL}/anolis-9.9.9-linux-x86_64.tar.gz"
    printf '#!/bin/bash\n' > "${REL}/install.sh"
    printf '{}' > "${REL}/manifest.json"
    printf '{}' > "${REL}/manifest-linux-arm64.json"
    printf '{}' > "${REL}/runtime-config-schema-manifest.json"
    printf '{}' > "${REL}/metrics.json"
}

@test "#446 write covers the tarballs, install.sh and the manifests, not metrics.json" {
    run bash "${SCRIPT}" write "${REL}"
    [ "$status" -eq 0 ]
    run awk '{ print $2 }' "${REL}/SHA256SUMS"
    [ "${lines[*]}" = "anolis-9.9.9-linux-arm64.tar.gz anolis-9.9.9-linux-x86_64.tar.gz install.sh manifest-linux-arm64.json manifest.json runtime-config-schema-manifest.json" ]
}

@test "#446 check passes on what write produced" {
    bash "${SCRIPT}" write "${REL}"
    run bash "${SCRIPT}" check "${REL}"
    [ "$status" -eq 0 ]
}

@test "#446 check fails when SHA256SUMS has no line for install.sh" {
    bash "${SCRIPT}" write "${REL}"
    grep -v '  install\.sh$' "${REL}/SHA256SUMS" > "${REL}/sums" && mv "${REL}/sums" "${REL}/SHA256SUMS"
    run bash "${SCRIPT}" check "${REL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHA256SUMS has 0 lines for install.sh, expected 1"* ]]
}

@test "#446 check fails when install.sh is not in the directory and SHA256SUMS omits it" {
    bash "${SCRIPT}" write "${REL}"
    grep -v '  install\.sh$' "${REL}/SHA256SUMS" > "${REL}/sums" && mv "${REL}/sums" "${REL}/SHA256SUMS"
    rm "${REL}/install.sh"
    run bash "${SCRIPT}" check "${REL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"0 lines for install.sh"* ]]
}

@test "#446 check fails when install.sh does not match its line" {
    bash "${SCRIPT}" write "${REL}"
    printf '#!/bin/bash\necho changed\n' > "${REL}/install.sh"
    run bash "${SCRIPT}" check "${REL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"install.sh: FAILED"* ]]
}

@test "#446 check fails when a downloaded manifest has no line" {
    bash "${SCRIPT}" write "${REL}"
    printf '{}' > "${REL}/telemetry-schema-manifest.json"
    run bash "${SCRIPT}" check "${REL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"0 lines for telemetry-schema-manifest.json"* ]]
}

@test "#446 write refuses a release directory without install.sh" {
    rm "${REL}/install.sh"
    run bash "${SCRIPT}" write "${REL}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"install.sh is not in"* ]]
    [ ! -e "${REL}/SHA256SUMS" ]
}
