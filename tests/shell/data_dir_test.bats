#!/usr/bin/env bats
# runtime.data_dir in the installed config (anolishq/anolis#562). The runtime
# resolves a default or relative data_dir against the config file's directory,
# so the installed runtime.yaml names <prefix>/anolis-data, where the unit's
# WorkingDirectory used to put it: a fresh install writes the key, and an
# upgrade adds it to a preserved config that lacks it. Nothing on disk moves.
#
# Source install.sh (main() guarded) and run the config phase with chown
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
    mkdir -p "${pc}" "${PREFIX}/config"
    printf 'runtime_profiles:\n  manual: config/anolis-runtime.manual.yaml\ncomponents: {}\n' \
        > "${BUNDLE_DIR}/projects/proj/machine-profile.yaml"
    printf 'runtime:\n  name: rig\nautomation:\n  enabled: false\n' > "${pc}/anolis-runtime.manual.yaml"
    RUNTIME_VARIANT=""
    REPLACE_CONFIG=0
}

dest() { printf '%s' "${PREFIX}/config/runtime.yaml"; }

@test "562: a fresh install names <prefix>/anolis-data" {
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/anolis-data" ]
    [ "$(_yaml_block_value "$(dest)" runtime name)" = "rig" ]
}

@test "562: a bundled config with no runtime block gets one" {
    printf 'automation:\n  enabled: false\n' > "${BUNDLE_DIR}/projects/proj/config/anolis-runtime.manual.yaml"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/anolis-data" ]
    [ "$(_yaml_block_value "$(dest)" automation enabled)" = "false" ]
}

@test "562: a preserved config without data_dir gets the key, and nothing else changes" {
    printf 'runtime:\n    name: kept\nautomation:\n  enabled: false\n# the operator edited this\n' > "$(dest)"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [[ "${output}" == *"runtime.data_dir"*"${PREFIX}/anolis-data"* ]]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/anolis-data" ]
    grep -qx "    data_dir: ${PREFIX}/anolis-data" "$(dest)"
    [ "$(grep -v data_dir "$(dest)")" = "$(printf 'runtime:\n    name: kept\nautomation:\n  enabled: false\n# the operator edited this')" ]
}

@test "562: a preserved config with an absolute data_dir is untouched" {
    printf 'runtime:\n  name: kept\n  data_dir: /srv/anolis\n' > "$(dest)"
    local before
    before=$(cat "$(dest)")
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(cat "$(dest)")" = "${before}" ]
}

@test "562: a preserved relative data_dir is pinned to where it resolved before" {
    printf 'runtime:\n  data_dir: state\n  name: kept\n' > "$(dest)"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/state" ]
    [ "$(_yaml_block_value "$(dest)" runtime name)" = "kept" ]
}

@test "562: a flow-style runtime block is left as it is, with a warning" {
    printf 'runtime: {name: kept}\n' > "$(dest)"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(cat "$(dest)")" = "runtime: {name: kept}" ]
    [[ "${output}" == *"runtime.data_dir"*"${PREFIX}/anolis-data"* ]]
}
