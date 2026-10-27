#!/usr/bin/env bats
# Tests for --variant selection via runtime_profiles + verify-inert
# (anolishq/anolis-workbench#225).
#
# install.sh selects the active runtime variant by a runtime_profiles map KEY
# (never by filename — a name cannot be trusted, anolishq/anolis-workbench#254)
# and refuses to install a
# non-inert config. Source install.sh (main() guarded) and exercise the pure
# helpers + the selection phase with chown mocked. No root/network/systemd.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test
    TMP="${BATS_TEST_TMPDIR}"
    chown() { return 0; }  # no root in CI
}

_write_profile() {  # <path> — machine-profile.yaml with a runtime_profiles map
    printf 'runtime_profiles:\n  manual: config/anolis-runtime.manual.yaml\n  telemetry: config/anolis-runtime.telemetry.yaml\ncomponents: {}\n' > "$1"
}

# ---------------------------------------------------------------------------
# verify_inert_runtime_config
# ---------------------------------------------------------------------------

@test "verify-inert: accepts a config with no automation block" {
    cfg="${TMP}/a.yaml"; printf 'http:\n  bind: 127.0.0.1\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "verify-inert: accepts automation.enabled false" {
    cfg="${TMP}/b.yaml"; printf 'automation:\n  enabled: false\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "verify-inert: refuses automation.enabled true" {
    cfg="${TMP}/c.yaml"; printf 'automation:\n  enabled: true\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"not inert"* ]]
}

@test "verify-inert: refuses enabled false WITH mode_transition_hooks (not a clean inert baseline)" {
    cfg="${TMP}/d.yaml"
    printf 'automation:\n  enabled: false\n  mode_transition_hooks:\n    before_transition: []\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"mode_transition_hooks"* ]]
}

@test "verify-inert: content wins over a .manual. filename (the #254 mislabel)" {
    cfg="${TMP}/anolis-runtime.bioreactor.manual.yaml"
    printf 'automation:\n  enabled: true\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
}

@test "verify-inert: warn mode logs but returns 0 (preserved operator config)" {
    cfg="${TMP}/e.yaml"; printf 'automation:\n  enabled: true\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}" warn
    [ "$status" -eq 0 ]
    [[ "${output}" == *"not inert"* ]]
    [[ "${output}" == *"preserved"* ]]
}

# ---------------------------------------------------------------------------
# _profile_variant_path / _profile_variant_keys
# ---------------------------------------------------------------------------

@test "variant path: resolves a known key" {
    mp="${TMP}/mp.yaml"; _write_profile "${mp}"
    run _profile_variant_path "${mp}" manual
    [ "${output}" = "config/anolis-runtime.manual.yaml" ]
}

@test "variant path: empty for an unknown key" {
    mp="${TMP}/mp.yaml"; _write_profile "${mp}"
    run _profile_variant_path "${mp}" nope
    [ -z "${output}" ]
}

@test "variant keys: lists all map keys" {
    mp="${TMP}/mp.yaml"; _write_profile "${mp}"
    run _profile_variant_keys "${mp}"
    [[ "${output}" == *"manual"* ]]
    [[ "${output}" == *"telemetry"* ]]
}

@test "variant keys: empty when there is no map" {
    mp="${TMP}/mp.yaml"; printf 'components: {}\n' > "${mp}"
    run _profile_variant_keys "${mp}"
    [ -z "${output}" ]
}

# ---------------------------------------------------------------------------
# phase_config_runtime — selection + fail-closed
# ---------------------------------------------------------------------------

# Fabricate a bundle: projects/proj/{machine-profile.yaml, config/<variants>}
# plus an empty PREFIX/config for the destination. Sets the globals the phase reads.
_make_bundle() {  # <profile-writer-fn>
    BUNDLE_DIR="${TMP}/bundle"
    PREFIX="${TMP}/prefix"
    INSTALLED_PROFILE="proj"
    local pc="${BUNDLE_DIR}/projects/proj/config"
    mkdir -p "${pc}" "${PREFIX}/config"
    "$1" "${BUNDLE_DIR}/projects/proj/machine-profile.yaml"
    printf 'automation:\n  enabled: false\n' > "${pc}/anolis-runtime.manual.yaml"
    printf 'automation:\n  enabled: false\ntelemetry:\n  enabled: true\n' > "${pc}/anolis-runtime.telemetry.yaml"
}

@test "phase_config_runtime: default installs the manual variant" {
    _make_bundle _write_profile
    RUNTIME_VARIANT=""
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [[ "${output}" == *"installed variant 'manual'"* ]]
    grep -q "enabled: false" "${PREFIX}/config/runtime.yaml"
}

@test "phase_config_runtime: --variant selects by key" {
    _make_bundle _write_profile
    RUNTIME_VARIANT="telemetry"
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [[ "${output}" == *"installed variant 'telemetry'"* ]]
    grep -q "telemetry" "${PREFIX}/config/runtime.yaml"
}

@test "phase_config_runtime: unknown variant fails closed and names available keys" {
    _make_bundle _write_profile
    RUNTIME_VARIANT="bogus"
    run phase_config_runtime
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bogus"* ]]
    [[ "${output}" == *"manual"* ]]
}

@test "phase_config_runtime: no runtime_profiles map fails closed" {
    _nomap() { printf 'components: {}\n' > "$1"; }
    _make_bundle _nomap
    RUNTIME_VARIANT=""
    run phase_config_runtime
    [ "$status" -ne 0 ]
    [[ "${output}" == *"runtime_profiles map"* ]]
}

@test "phase_config_runtime: refuses a mislabeled non-inert manual variant" {
    _make_bundle _write_profile
    printf 'automation:\n  enabled: true\n' > "${BUNDLE_DIR}/projects/proj/config/anolis-runtime.manual.yaml"
    RUNTIME_VARIANT=""
    run phase_config_runtime
    [ "$status" -ne 0 ]
    [[ "${output}" == *"not inert"* ]]
}

@test "phase_config_runtime: preserves an existing non-inert config with a warning (upgrade)" {
    _make_bundle _write_profile
    printf 'automation:\n  enabled: true\n' > "${PREFIX}/config/runtime.yaml"  # operator-activated
    RUNTIME_VARIANT=""
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [[ "${output}" == *"preserved"* ]]
    grep -q "enabled: true" "${PREFIX}/config/runtime.yaml"  # unchanged
}

# A kept runtime.yaml from an install under another project name (#439): bread0
# (flow args) and ezo0 (block args) read the old project's configs, sim0 this one's,
# and foo0 one outside any project.
_kept_runtime_yaml() {  # <project of bread0 and ezo0>
    local old="${PREFIX}/projects/$1/config"
    {
        echo "providers:"
        echo "  - id: bread0"
        echo "    command: ${PREFIX}/bin/anolis-provider-bread"
        echo "    args: [\"--config\", \"${old}/provider-bread.yaml\"]"
        echo "  - id: ezo0"
        echo "    command: ${PREFIX}/bin/anolis-provider-ezo"
        echo "    args:"
        echo "      - --config"
        echo "      - ${old}/provider-ezo.yaml"
        echo "  - id: sim0"
        echo "    command: ${PREFIX}/bin/anolis-provider-sim"
        echo "    args: [\"--config\", \"${PREFIX}/projects/proj/config/provider-sim.yaml\"]"
        echo "  - id: foo0"
        echo "    command: ${PREFIX}/bin/anolis-provider-foo"
        echo "    args: [\"--config\", \"${PREFIX}/config/providers/foo.yaml\"]"
        echo "automation:"
        echo "  enabled: false"
    } > "${PREFIX}/config/runtime.yaml"
}

@test "phase_config_runtime: warns per provider whose kept --config is another project's" {
    _make_bundle _write_profile
    _kept_runtime_yaml old-proj
    cp "${PREFIX}/config/runtime.yaml" "${TMP}/before.yaml"
    RUNTIME_VARIANT=""
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [ "$(grep -c "config (runtime): provider .* preserved" <<< "${output}")" -eq 2 ]
    [[ "${output}" == *"provider 'bread0' still reads ${PREFIX}/projects/old-proj/config/provider-bread.yaml"*"${PREFIX}/projects/proj/config/provider-bread.yaml"* ]]
    [[ "${output}" == *"provider 'ezo0' still reads ${PREFIX}/projects/old-proj/config/provider-ezo.yaml"*"${PREFIX}/projects/proj/config/provider-ezo.yaml"* ]]
    [[ "${output}" != *"'sim0'"* ]]
    [[ "${output}" != *"'foo0'"* ]]
    # unchanged, but for the data dir an upgrade names (#562)
    printf 'runtime:\n  data_dir: %s/anolis-data\n' "${PREFIX}" >> "${TMP}/before.yaml"
    cmp "${TMP}/before.yaml" "${PREFIX}/config/runtime.yaml"
}

@test "phase_config_runtime: no provider warning when the kept --config paths are this project's" {
    _make_bundle _write_profile
    _kept_runtime_yaml proj
    RUNTIME_VARIANT=""
    run phase_config_runtime
    [ "$status" -eq 0 ]
    [[ "${output}" == *"exists (preserved)"* ]]
    [[ "${output}" != *"⚠"* ]]
}

# ---------------------------------------------------------------------------
# arg parsing
# ---------------------------------------------------------------------------

@test "help documents --variant" {
    run show_help
    [ "$status" -eq 0 ]
    [[ "${output}" == *"--variant"* ]]
}

# ---------------------------------------------------------------------------
# verify-inert bypass regressions — valid YAML the runtime treats as enabled
# must NOT pass as inert (default-deny).
# ---------------------------------------------------------------------------

@test "verify-inert: refuses non-canonical truthy enabled (True/yes/on/y)" {
    for v in True TRUE yes on y; do
        cfg="${TMP}/t-${v}.yaml"; printf 'automation:\n  enabled: %s\n' "${v}" > "${cfg}"
        run verify_inert_runtime_config "${cfg}"
        [ "$status" -ne 0 ] || { echo "accepted enabled: ${v}"; false; }
    done
}

@test "verify-inert: refuses a quoted true" {
    cfg="${TMP}/q.yaml"; printf 'automation:\n  enabled: "true"\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
}

@test "verify-inert: refuses a flow-style automation mapping" {
    cfg="${TMP}/flow.yaml"; printf 'automation: {enabled: true}\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
}

@test "verify-inert: refuses a CRLF enabled true (Windows-edited config)" {
    cfg="${TMP}/crlf.yaml"; printf 'automation:\r\n  enabled: true\r\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
}

@test "verify-inert: accepts a CRLF enabled false" {
    cfg="${TMP}/crlf-ok.yaml"; printf 'automation:\r\n  enabled: false\r\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "verify-inert: mode_transition_hooks under a different block does not trip it" {
    cfg="${TMP}/other.yaml"
    printf 'automation:\n  enabled: false\nother:\n  mode_transition_hooks: x\n' > "${cfg}"
    run verify_inert_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "phase_config_runtime: missing resolved variant file fails closed" {
    _make_bundle _write_profile
    rm -f "${BUNDLE_DIR}/projects/proj/config/anolis-runtime.telemetry.yaml"
    RUNTIME_VARIANT="telemetry"
    run phase_config_runtime
    [ "$status" -ne 0 ]
    [[ "${output}" == *"missing file"* ]]
}

@test "variant path: resolves a quoted map value" {
    mp="${TMP}/mpq.yaml"
    printf 'runtime_profiles:\n  manual: "config/anolis-runtime.manual.yaml"\ncomponents: {}\n' > "${mp}"
    run _profile_variant_path "${mp}" manual
    [ "${output}" = "config/anolis-runtime.manual.yaml" ]
}

@test "preflight_variant_selection: fails closed on a bad variant before any mutation" {
    _make_bundle _write_profile
    RUNTIME_VARIANT="bogus"
    run preflight_variant_selection
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bogus"* ]]
}
