#!/usr/bin/env bats
# Unit tests for phase_resolve() in tools/install.sh — bundle tag/version →
# download-URL resolution. These source install.sh (main() is guarded so it
# does not execute) and call phase_resolve directly with a mocked curl, so no
# network, root, or hardware is required.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
FIXTURE="${BATS_TEST_DIRNAME}/fixtures/releases-bioreactor.json"

setup() {
    # Load the script's functions without running main().
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    # The script sets `set -euo pipefail`; relax it so it doesn't interfere with
    # the BATS harness (which manages errexit itself).
    set +e +u +o pipefail

    # Minimal state phase_resolve depends on.
    PROFILE="bioreactor-v1"
    ARCH="arm64"
    VERSION=""
    LOCAL_PATH=""
    GITHUB_TOKEN=""
    BUNDLE_URL=""
    BUNDLE_TAG=""
}

# curl shim returning the canned releases list for the API call.
_mock_curl_ok() { curl() { cat "${FIXTURE}"; }; }
# curl shim simulating an API failure (rate limit / offline).
_mock_curl_fail() { curl() { return 22; }; }

# ---------------------------------------------------------------------------
# Latest resolution (semver, not date/first-listed)
# ---------------------------------------------------------------------------

@test "latest resolves to the highest SEMVER tag" {
    _mock_curl_ok
    phase_resolve
    # Fixture order is 0.2.0, 0.10.0, 0.3.0 — highest is 0.10.0 (not first, not
    # a lexical max where 0.3.0 > 0.10.0).
    [ "${BUNDLE_TAG}" = "bioreactor-v1-0.10.0" ]
    [ "${BUNDLE_URL}" = "https://github.com/anolishq/anolis-projects/releases/download/bioreactor-v1-0.10.0/anolis-bioreactor-v1-0.10.0-arm64.tar.gz" ]
}

@test "latest ignores tags for other profiles" {
    _mock_curl_ok
    phase_resolve
    [[ "${BUNDLE_TAG}" != *"other-profile"* ]]
}

# ---------------------------------------------------------------------------
# Explicit --version pinning (the G2 regression)
# ---------------------------------------------------------------------------

@test "explicit --version pins to that version even when a newer release exists" {
    VERSION="0.2.0"
    _mock_curl_ok   # newest in fixture is 0.10.0; must be ignored
    phase_resolve
    [ "${BUNDLE_TAG}" = "bioreactor-v1-0.2.0" ]
    [[ "${BUNDLE_URL}" == *"/download/bioreactor-v1-0.2.0/anolis-bioreactor-v1-0.2.0-arm64.tar.gz" ]]
    # Regression guard: must NOT resolve to the latest bundle.
    [[ "${BUNDLE_URL}" != *"0.10.0"* ]]
}

@test "explicit --version makes no API call" {
    VERSION="0.3.0"
    # Fail loudly if curl is invoked; phase_resolve must not reach it.
    curl() { echo "curl must not be called for a pinned version" >&2; return 1; }
    phase_resolve
    [ "${BUNDLE_TAG}" = "bioreactor-v1-0.3.0" ]
}

@test "arch is reflected in the derived URL" {
    ARCH="x86_64"
    VERSION="0.3.0"
    phase_resolve
    [[ "${BUNDLE_URL}" == *"-x86_64.tar.gz" ]]
}

# ---------------------------------------------------------------------------
# Robustness / error paths
# ---------------------------------------------------------------------------

@test "tag parse tolerates whitespace in the JSON" {
    curl() { printf '%s' '[ { "tag_name" : "bioreactor-v1-0.5.0" , "assets": [] } ]'; }
    phase_resolve
    [ "${BUNDLE_TAG}" = "bioreactor-v1-0.5.0" ]
}

@test "no matching releases fails with guidance" {
    PROFILE="nonexistent-profile"
    _mock_curl_ok
    run phase_resolve
    [ "$status" -ne 0 ]
    [[ "${output}" == *"No releases found for profile"* ]]
}

@test "API failure prints rate-limit/offline guidance and --local hint" {
    _mock_curl_fail
    run phase_resolve
    [ "$status" -ne 0 ]
    [[ "${output}" == *"GitHub API request failed"* ]]
    [[ "${output}" == *"--local"* ]]
}

@test "local mode skips resolution entirely" {
    local d
    d=$(mktemp -d)
    LOCAL_PATH="${d}"
    # curl must never be called in local mode.
    curl() { echo "curl must not be called in local mode" >&2; return 1; }
    run phase_resolve
    [ "$status" -eq 0 ]
    [[ "${output}" == *"local path"* ]]
    rmdir "${d}"
}
