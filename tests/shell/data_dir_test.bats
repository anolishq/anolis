#!/usr/bin/env bats
# runtime.data_dir in the installed config (anolishq/anolis#562). The runtime
# resolves a default or relative data_dir against the config file's directory,
# so the installed runtime.yaml names <prefix>/anolis-data, where the unit's
# WorkingDirectory used to put it: a fresh install writes the key, and an
# upgrade adds it to a preserved config that lacks it. Nothing on disk moves.
# A relative data_dir in a variant means what the runtime makes of it, the
# config's directory (#643); a preserved one keeps the place its data is in.
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
    # A runtime before 0.2.0 resolved it against WorkingDirectory=<prefix>,
    # and its data is there.
    printf 'runtime:\n  data_dir: state\n  name: kept\n' > "$(dest)"
    mkdir -p "${PREFIX}/state/estop"
    printf '{"latched":true}\n' > "${PREFIX}/state/estop/0123456789abcdef.json"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/state" ]
    [ "$(_yaml_block_value "$(dest)" runtime name)" = "kept" ]
}

@test "643: a fresh install of a variant naming ../anolis-data names <prefix>/anolis-data" {
    printf 'runtime:\n  name: rig\n  data_dir: ../anolis-data\nautomation:\n  enabled: false\n' \
        > "${BUNDLE_DIR}/projects/proj/config/anolis-runtime.manual.yaml"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/anolis-data" ]
    [ "$(grep -c data_dir "$(dest)")" -eq 1 ]
}

@test "643: --replace-config with a variant naming ../anolis-data names <prefix>/anolis-data" {
    printf 'runtime:\n  name: old\n  data_dir: %s/anolis-data\n' "${PREFIX}" > "$(dest)"
    printf 'runtime:\n  name: rig\n  data_dir: ../anolis-data\nautomation:\n  enabled: false\n' \
        > "${BUNDLE_DIR}/projects/proj/config/anolis-runtime.manual.yaml"
    REPLACE_CONFIG=1
    REPLACE_STAMP=test
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/anolis-data" ]
    [ "$(_yaml_block_value "$(dest)" runtime name)" = "rig" ]
    [ "$(grep -c data_dir "$(dest)")" -eq 1 ]
}

@test "643: a preserved ../anolis-data copied in by hand stays at <prefix>/anolis-data" {
    # A quickstart's cp of a variant over runtime.yaml, run by runtime 0.2.0,
    # which keeps its data at <prefix>/config/../anolis-data.
    printf 'runtime:\n  name: kept\n  data_dir: ../anolis-data\n' > "$(dest)"
    mkdir -p "${PREFIX}/anolis-data/estop"
    printf '{"latched":true}\n' > "${PREFIX}/anolis-data/estop/0123456789abcdef.json"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/anolis-data" ]
}

@test "643: a preserved relative data_dir with data beside the config resolves there" {
    printf 'runtime:\n  name: kept\n  data_dir: state\n' > "$(dest)"
    mkdir -p "${PREFIX}/config/state/runs"
    printf '{"run_id":"r1"}\n' > "${PREFIX}/config/state/runs/index.jsonl"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/config/state" ]
}

@test "643: a preserved relative data_dir with data in both places keeps the config's" {
    # Runtime 0.2.0 has run on it since an older one did: its data is current.
    printf 'runtime:\n  name: kept\n  data_dir: state\n' > "$(dest)"
    mkdir -p "${PREFIX}/state/estop" "${PREFIX}/config/state/estop"
    printf '{"latched":true}\n' | tee "${PREFIX}/state/estop/a.json" > "${PREFIX}/config/state/estop/a.json"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/config/state" ]
}

@test "643: directories a runtime only started in are not data" {
    # Runtime 0.2.0 creates runs/ at every start, so a config it read once
    # without data_dir leaves <config dir>/x/runs/ behind, empty.
    printf 'runtime:\n  name: kept\n  data_dir: state\n' > "$(dest)"
    mkdir -p "${PREFIX}/config/state/runs" "${PREFIX}/config/state/estop" "${PREFIX}/state/estop"
    : > "${PREFIX}/config/state/runs/index.jsonl"
    printf '{"latched":true}\n' > "${PREFIX}/state/estop/0123456789abcdef.json"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/state" ]
}

@test "643: a run journal alone, with no latch, is data" {
    # A runtime before 0.2.0 that was never stopped still kept its runs at <prefix>/x.
    printf 'runtime:\n  name: kept\n  data_dir: state\n' > "$(dest)"
    mkdir -p "${PREFIX}/state/runs"
    printf '{"run_id":"r1"}\n' > "${PREFIX}/state/runs/index.jsonl"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/state" ]
}

@test "643: a preserved relative data_dir with no data yet resolves against the config" {
    printf 'runtime:\n  name: kept\n  data_dir: state\n' > "$(dest)"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(_yaml_block_value "$(dest)" runtime data_dir)" = "${PREFIX}/config/state" ]
}

@test "562: a flow-style runtime block is left as it is, with a warning" {
    printf 'runtime: {name: kept}\n' > "$(dest)"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(cat "$(dest)")" = "runtime: {name: kept}" ]
    [[ "${output}" == *"runtime.data_dir"*"${PREFIX}/anolis-data"* ]]
}
