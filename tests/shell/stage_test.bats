#!/usr/bin/env bats
# Unit tests for do_stage() in tools/install.sh — build an offline bundle from a
# local config. These source install.sh (main() is guarded) and call do_stage
# with _stage_fetch mocked, so no network/root is needed. The fixture project
# declares providers "foo"/"bar" (not the real bread/ezo) to prove --stage is
# generic over whatever the machine-profile declares — nothing is hardcoded.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
PROJECT="${BATS_TEST_DIRNAME}/fixtures/stage-project"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    STAGE_DIR=""
    PROJECT_DIR="${PROJECT}"
    TARGET_ARCH="x86_64"
    PREFIX="/opt/anolis"
    GITHUB_TOKEN=""
    OUT="$(mktemp -d)"

    # Mock the release download: fabricate tarballs with the expected bin/ layout.
    _stage_fetch() {
        local asset="$3" dir="$4" t
        case "${asset}" in
            install.sh)
                printf '#!/bin/bash\n' > "${dir}/install.sh" ;;
            anolis-provider-*-linux-*.tar.gz)
                local name
                name=$(printf '%s' "${asset}" | sed -E 's/^anolis-provider-([a-z0-9-]+)-.*/\1/')
                t=$(mktemp -d); mkdir -p "${t}/bin"; printf 'bin' > "${t}/bin/anolis-provider-${name}"
                tar -czf "${dir}/${asset}" -C "${t}" bin; rm -rf "${t}" ;;
            anolis-*-linux-*.tar.gz)
                t=$(mktemp -d); mkdir -p "${t}/bin"; printf 'bin' > "${t}/bin/anolis-runtime"
                tar -czf "${dir}/${asset}" -C "${t}" bin; rm -rf "${t}" ;;
        esac
    }
}

teardown() {
    rm -rf "${OUT:-}" "${STAGE_TMP:-}"
}

@test "--stage requires --project" {
    STAGE_DIR="${OUT}"; PROJECT_DIR=""
    run do_stage
    [ "$status" -ne 0 ]
    [[ "$output" == *"--project"* ]]
}

@test "--stage fails on missing machine-profile.yaml" {
    STAGE_DIR="${OUT}"; PROJECT_DIR="$(mktemp -d)"
    run do_stage
    [ "$status" -ne 0 ]
    [[ "$output" == *"machine-profile.yaml"* ]]
}

@test "--stage builds a bundle generic over the profile's providers (foo/bar)" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]

    local bundle="${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz"
    [ -f "${bundle}" ]

    local x; x="$(mktemp -d)"
    tar -xzf "${bundle}" -C "${x}" --strip-components=1

    # Binaries for BOTH declared providers — proves generic, no hardcoding.
    [ -f "${x}/bin/anolis-runtime" ]
    [ -f "${x}/bin/anolis-provider-foo" ]
    [ -f "${x}/bin/anolis-provider-bar" ]

    # Bundle layout install.sh --local consumes.
    [ -f "${x}/config/runtime.yaml" ]
    [ -f "${x}/config/providers/foo.yaml" ]
    [ -f "${x}/config/providers/bar.yaml" ]
    [ -f "${x}/systemd/anolis-runtime.service" ]
    [ -f "${x}/manifest.json" ]
    [ -f "${x}/checksums.sha256" ]
    [ -f "${x}/install.sh" ]

    # Render: dev-relative → production absolute + bind rewrite.
    grep -q "/opt/anolis/bin/anolis-provider-foo" "${x}/config/runtime.yaml"
    grep -q "bind: 0.0.0.0" "${x}/config/runtime.yaml"

    # Manifest carries both providers.
    grep -q '"foo"' "${x}/manifest.json"
    grep -q '"bar"' "${x}/manifest.json"

    # Checksums verify.
    ( cd "${x}" && sha256sum -c checksums.sha256 >/dev/null )

    rm -rf "${x}"
}
