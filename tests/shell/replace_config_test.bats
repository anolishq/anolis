#!/usr/bin/env bats
# --replace-config (anolishq/anolis#470): install a project's config onto a
# machine that already has one. An existing runtime.yaml, and each provider
# config the bundle ships, moves to a timestamped backup next to it; the
# selected variant is installed, still through verify_inert_runtime_config,
# and each path is logged. Without the flag an existing config is preserved.
#
# Source install.sh (main() guarded) and run the config phases with chown
# mocked. No root, network or systemd.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    TMP="${BATS_TEST_TMPDIR}"
    chown() { return 0; }
    BUNDLE_DIR="${TMP}/bundle"
    PREFIX="${TMP}/prefix"
    INSTALLED_PROFILE="proj"
    local pc="${BUNDLE_DIR}/projects/proj/config"
    mkdir -p "${pc}" "${PREFIX}/config/providers" "${BUNDLE_DIR}/config/providers"
    printf 'runtime_profiles:\n  manual: config/anolis-runtime.manual.yaml\ncomponents: {}\n' \
        > "${BUNDLE_DIR}/projects/proj/machine-profile.yaml"
    printf 'automation:\n  enabled: false\n# from the bundle\n' > "${pc}/anolis-runtime.manual.yaml"
    printf 'automation:\n  enabled: true\n# the operator activated this one\n' > "${PREFIX}/config/runtime.yaml"
    RUNTIME_VARIANT=""
    REPLACE_STAMP="20261027T120000Z"
}

@test "470: --replace-config backs up runtime.yaml and installs the variant" {
    REPLACE_CONFIG=1
    run phase_config_runtime
    [ "$status" -eq 0 ]
    local backup="${PREFIX}/config/runtime.yaml.replaced-20261027T120000Z"
    grep -qx "# the operator activated this one" "${backup}"
    grep -qx "# from the bundle" "${PREFIX}/config/runtime.yaml"
    [[ "${output}" == *"${PREFIX}/config/runtime.yaml"*"${backup}"* ]]
    [[ "${output}" == *"variant 'manual'"* ]]
}

@test "470: without --replace-config the existing runtime.yaml is preserved, with no backup" {
    REPLACE_CONFIG=0
    run phase_config_runtime
    [ "$status" -eq 0 ]
    grep -qx "# the operator activated this one" "${PREFIX}/config/runtime.yaml"
    ! compgen -G "${PREFIX}/config/runtime.yaml.replaced-*" >/dev/null
}

@test "470: a non-inert variant is refused before anything is moved" {
    REPLACE_CONFIG=1
    printf 'automation:\n  enabled: true\n' > "${BUNDLE_DIR}/projects/proj/config/anolis-runtime.manual.yaml"
    run preflight_variant_selection
    [ "$status" -ne 0 ]
    [[ "${output}" == *"not inert"* ]]
    run phase_config_runtime
    [ "$status" -ne 0 ]
    grep -qx "# the operator activated this one" "${PREFIX}/config/runtime.yaml"
    ! compgen -G "${PREFIX}/config/runtime.yaml.replaced-*" >/dev/null
}

@test "470: --replace-config backs up and replaces each provider config the bundle ships" {
    REPLACE_CONFIG=1
    printf 'from: bundle\n' > "${BUNDLE_DIR}/config/providers/bread.yaml"
    printf 'from: operator\n' > "${PREFIX}/config/providers/bread.yaml"
    printf 'from: operator, not in the bundle\n' > "${PREFIX}/config/providers/other.yaml"
    run phase_config_providers
    [ "$status" -eq 0 ]
    grep -qx "from: operator" "${PREFIX}/config/providers/bread.yaml.replaced-20261027T120000Z"
    grep -qx "from: bundle" "${PREFIX}/config/providers/bread.yaml"
    grep -qx "from: operator, not in the bundle" "${PREFIX}/config/providers/other.yaml"
    ! compgen -G "${PREFIX}/config/providers/other.yaml.replaced-*" >/dev/null
    [[ "${output}" == *"${PREFIX}/config/providers/bread.yaml.replaced-20261027T120000Z"* ]]
}

@test "470: without --replace-config provider configs are preserved" {
    REPLACE_CONFIG=0
    printf 'from: bundle\n' > "${BUNDLE_DIR}/config/providers/bread.yaml"
    printf 'from: operator\n' > "${PREFIX}/config/providers/bread.yaml"
    run phase_config_providers
    [ "$status" -eq 0 ]
    grep -qx "from: operator" "${PREFIX}/config/providers/bread.yaml"
}

@test "470: --replace-config is parsed and documented" {
    parse_args --replace-config --project ./cfg
    [ "${REPLACE_CONFIG}" -eq 1 ]
    run show_help
    [[ "${output}" == *"--replace-config"* ]]
}

@test "470: --dry-run --replace-config says what it would back up" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --replace-config --project ./cfg
    [ "$status" -eq 0 ]
    [[ "${output}" == *"replace the runtime config"*"runtime.yaml.replaced-"* ]]
    [[ "${output}" == *"replace provider configs"*".replaced-"* ]]
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --project ./cfg
    [[ "${output}" != *"replaced-"* ]]
}
