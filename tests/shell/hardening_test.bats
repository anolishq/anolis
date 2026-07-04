#!/usr/bin/env bats
# Tests for the install.sh hardening fixes (anolishq/anolis#166):
#   B4 — the runtime systemd unit's ExecStart honors --prefix (not hardcoded).
#   B6 — the health/summary port is read from runtime.yaml, not hardcoded 8080.
#   N1 — checksum verification uses `sha256sum -c` and names a bad file.
#
# These source install.sh (main() guarded) and exercise the pure helpers — no
# root, network, or systemd. The live install path is validated on real
# hardware (anolishq/anolis#138).

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +e +u +o pipefail
}

# ---------------------------------------------------------------------------
# B4 — runtime unit ExecStart follows ${PREFIX}
# ---------------------------------------------------------------------------

@test "B4: runtime unit ExecStart uses a custom --prefix" {
    PREFIX=/srv/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"ExecStart=/srv/anolis/bin/anolis-runtime --config /srv/anolis/config/runtime.yaml"* ]]
    [[ "${output}" != *"/opt/anolis"* ]]
}

@test "B4: default prefix still renders /opt/anolis (no regression)" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"ExecStart=/opt/anolis/bin/anolis-runtime --config /opt/anolis/config/runtime.yaml"* ]]
}

@test "B4: comment backticks stay literal (unquoted heredoc not command-substituted)" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *'`providers:`'* ]]
    [[ "${output}" == *'`Wants=`'* ]]
}

@test "B4: still one unit — User/Group anolis, SupplementaryGroups, no per-provider Wants" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [[ "${output}" == *"User=anolis"* ]]
    [[ "${output}" == *"Group=anolis"* ]]
    [[ "${output}" == *"SupplementaryGroups=i2c gpio dialout"* ]]
    # No active Wants= directive (the only occurrence is the escaped comment).
    ! grep -qE '^Wants=' <<< "${output}"
}

# ---------------------------------------------------------------------------
# B6 — port derived from the installed runtime config
# ---------------------------------------------------------------------------

@test "B6: _runtime_port reads http.port from runtime.yaml" {
    PREFIX="${BATS_TEST_TMPDIR}/p"
    mkdir -p "${PREFIX}/config"
    printf 'runtime:\n  id: x\nhttp:\n  bind: 127.0.0.1\n  port: 9137\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_port
    [ "$status" -eq 0 ]
    [ "${output}" = "9137" ]
}

@test "B6: _runtime_port ignores a port: under a non-http block" {
    PREFIX="${BATS_TEST_TMPDIR}/p"
    mkdir -p "${PREFIX}/config"
    printf 'http:\n  port: 8080\nproviders:\n  port: 5\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_port
    [ "${output}" = "8080" ]
}

@test "B6: _runtime_port falls back to DEFAULT_PORT when config missing" {
    PREFIX="${BATS_TEST_TMPDIR}/none"
    run _runtime_port
    [ "${output}" = "8080" ]
}

@test "B6: _runtime_port falls back when port is unparseable" {
    PREFIX="${BATS_TEST_TMPDIR}/p"
    mkdir -p "${PREFIX}/config"
    printf 'http:\n  bind: 127.0.0.1\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_port
    [ "${output}" = "8080" ]
}

# ---------------------------------------------------------------------------
# N1 — checksum verification via sha256sum -c
# ---------------------------------------------------------------------------

_make_bundle() {
    local b="$1"
    mkdir -p "${b}/bin" "${b}/config"
    printf 'binary\n' > "${b}/bin/anolis-runtime"
    printf 'http:\n  port: 8080\n' > "${b}/config/runtime.yaml"
    ( cd "${b}" && find . -type f -not -name 'checksums.sha256' | sort | xargs sha256sum > checksums.sha256 )
}

@test "N1: phase_verify passes a good bundle" {
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/good"
    _make_bundle "${BUNDLE_DIR}"
    run phase_verify
    [ "$status" -eq 0 ]
    [[ "${output}" == *"all checksums pass"* ]]
}

@test "N1: phase_verify fails and names a tampered file" {
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bad"
    _make_bundle "${BUNDLE_DIR}"
    printf 'TAMPERED\n' > "${BUNDLE_DIR}/bin/anolis-runtime"
    run phase_verify
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bin/anolis-runtime: FAILED"* ]]
    [[ "${output}" == *"Checksum verification failed"* ]]
}

@test "N1: phase_verify skips cleanly when no checksums file" {
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/empty"
    mkdir -p "${BUNDLE_DIR}"
    run phase_verify
    [ "$status" -eq 0 ]
    [[ "${output}" == *"skipping verification"* ]]
}
