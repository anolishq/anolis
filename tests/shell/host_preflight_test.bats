#!/usr/bin/env bats
# Tests for the host preflight (anolis#318): before anything starts, install.sh
# runs every binary and each provider's --check-host as the service user, prints
# what providers report, and fails on unmet requirements unless
# --allow-unmet-host. Source install.sh (main() guarded); runuser is stubbed to
# run the command as the current user, and the "binaries" are small scripts. No
# root, network or systemd.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    mkdir -p "${PREFIX}/bin" "${PREFIX}/config"
    # runuser -u <user> -- <cmd...>: record the user, run the command as us.
    runuser() {
        echo "$2" >> "${BATS_TEST_TMPDIR}/runuser.log"
        shift 3
        "$@"
    }
    _fake_bin anolis-runtime 'echo "anolis-runtime 0.1.43"'
}

# _fake_bin <name> <body> — an executable script under ${PREFIX}/bin.
_fake_bin() {
    printf '#!/usr/bin/env bash\n%s\n' "$2" > "${PREFIX}/bin/$1"
    chmod +x "${PREFIX}/bin/$1"
}

# _provider <name> <check-host stdout> <check-host exit> — a provider that
# answers --version and --check-host.
_provider() {
    _fake_bin "anolis-provider-$1" "case \"\$1\" in
  --version) echo 1.0.0 ;;
  --check-host) printf '%s\n' '$2'; exit $3 ;;
esac"
}

# _runtime_yaml <id>... — a rendered runtime.yaml naming each provider with a
# flow-style args list, as install.sh's config render produces.
_runtime_yaml() {
    {
        echo "runtime:"
        echo "  name: test"
        echo "providers:"
        for id in "$@"; do
            echo "  - id: ${id}0"
            echo "    command: ${PREFIX}/bin/anolis-provider-${id}"
            echo "    args: [\"--config\", \"${PREFIX}/config/${id}.yaml\"]"
            echo "    timeout_ms: 5000"
        done
        echo "http:"
        echo "  port: 8080"
    } > "${PREFIX}/config/runtime.yaml"
}

MET='{"check_host_version":1,"requirements":[{"id":"i2c.bus_present","status":"met","detail":"/dev/i2c-1 exists"}]}'
UNMET='{"check_host_version":1,"requirements":[{"id":"i2c.bus_access","status":"unmet","detail":"anolis cannot open /dev/i2c-1 read-write (group '"'"'i2c'"'"', mode 0660)","remedy":"add anolis to group '"'"'i2c'"'"'"}]}'

# ---------------------------------------------------------------------------
# _runtime_providers
# ---------------------------------------------------------------------------

@test "_runtime_providers: one line per provider with its command and --config" {
    _runtime_yaml bread ezo
    run _runtime_providers "${PREFIX}/config/runtime.yaml"
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "bread0	${PREFIX}/bin/anolis-provider-bread	${PREFIX}/config/bread.yaml" ]
    [ "${lines[1]}" = "ezo0	${PREFIX}/bin/anolis-provider-ezo	${PREFIX}/config/ezo.yaml" ]
    [ "${#lines[@]}" -eq 2 ]
}

@test "_runtime_providers: reads a block-style args list" {
    printf 'providers:\n  - id: sim0\n    command: /opt/anolis/bin/anolis-provider-sim\n    args:\n      - --config\n      - "/opt/anolis/config/sim.yaml"\n    timeout_ms: 5000\n' > "${PREFIX}/config/runtime.yaml"
    run _runtime_providers "${PREFIX}/config/runtime.yaml"
    [ "${lines[0]}" = "sim0	/opt/anolis/bin/anolis-provider-sim	/opt/anolis/config/sim.yaml" ]
}

@test "_runtime_providers: same answer under mawk and gawk" {
    _runtime_yaml bread ezo
    local want got bin
    want=$(_runtime_providers "${PREFIX}/config/runtime.yaml")
    for bin in mawk gawk; do
        command -v "${bin}" >/dev/null || continue
        got=$(awk() { command "${bin}" "$@"; }; _runtime_providers "${PREFIX}/config/runtime.yaml")
        [ "${got}" = "${want}" ]
    done
}

# ---------------------------------------------------------------------------
# phase_host_preflight
# ---------------------------------------------------------------------------

@test "preflight: met requirements pass, run as the service user" {
    _provider bread "${MET}" 0
    _runtime_yaml bread
    run phase_host_preflight
    [ "$status" -eq 0 ]
    [[ "${output}" == *"bread0: host requirements met"* ]]
    # Every run went through runuser as anolis.
    ! grep -qv '^anolis$' "${BATS_TEST_TMPDIR}/runuser.log"
}

@test "preflight: unmet requirements fail and print the provider's detail and fix" {
    _provider bread "${UNMET}" 1
    _runtime_yaml bread
    run phase_host_preflight
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bread0: host requirements unmet"* ]]
    [[ "${output}" == *"i2c.bus_access (unmet)"* ]]
    [[ "${output}" == *"add anolis to group"* ]]
    [[ "${output}" == *"--allow-unmet-host"* ]]
}

@test "preflight: --allow-unmet-host continues past unmet requirements" {
    _provider bread "${UNMET}" 1
    _runtime_yaml bread
    ALLOW_UNMET_HOST=1
    run phase_host_preflight
    [ "$status" -eq 0 ]
    [[ "${output}" == *"continuing (--allow-unmet-host)"* ]]
}

@test "preflight: exit 2 with no JSON is no answer: shown with its stderr, skipped" {
    # The profile lets a provider exit 2 with empty stdout on a config it cannot
    # read, which looks the same as an argparse-style unknown flag; like the
    # conformance harness, the preflight does not fail on it, but shows stderr.
    _fake_bin anolis-provider-bread 'case "$1" in
  --version) echo 1.0.0 ;;
  --check-host) echo "Invalid config: hardware.bogus: unknown key" >&2; exit 2 ;;
esac'
    _runtime_yaml bread
    run phase_host_preflight
    [ "$status" -eq 0 ]
    [[ "${output}" == *"bread0: --check-host gave no answer (exit 2)"* ]]
    [[ "${output}" == *"hardware.bogus: unknown key"* ]]
}

@test "preflight: exit 2 with an envelope fails, even with --allow-unmet-host" {
    _provider bread '{"check_host_version":1,"requirements":[]}' 2
    _runtime_yaml bread
    ALLOW_UNMET_HOST=1
    run phase_host_preflight
    [ "$status" -ne 0 ]
    [[ "${output}" == *"could not evaluate"* ]]
}

@test "preflight: a provider that predates --check-host is skipped, not failed" {
    # bread <= 0.4.0 rejects an unknown flag with its usage text on stdout.
    _fake_bin anolis-provider-bread 'case "$1" in
  --version) echo 0.4.0 ;;
  *) printf "Usage:\n  anolis-provider-bread --version\n"; exit 1 ;;
esac'
    _runtime_yaml bread
    run phase_host_preflight
    [ "$status" -eq 0 ]
    [[ "${output}" == *"bread0: --check-host gave no answer (exit 1)"* ]]
}

@test "preflight: a provider binary that cannot run fails, even with --allow-unmet-host" {
    # What a too-old glibc looks like: the loader refuses before main().
    _fake_bin anolis-provider-bread 'echo "anolis-provider-bread: /lib/aarch64-linux-gnu/libc.so.6: version \`GLIBC_2.38'"'"' not found" >&2; exit 1'
    _runtime_yaml bread
    ALLOW_UNMET_HOST=1
    run phase_host_preflight
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bread0:"*"cannot run on this host"* ]]
    [[ "${output}" == *"GLIBC_2.38"* ]]
}

@test "preflight: a runtime binary that cannot run fails" {
    _fake_bin anolis-runtime 'echo "version GLIBC_2.38 not found" >&2; exit 1'
    _provider bread "${MET}" 0
    _runtime_yaml bread
    run phase_host_preflight
    [ "$status" -ne 0 ]
    [[ "${output}" == *"anolis-runtime cannot run on this host"* ]]
}

@test "preflight: every provider is checked, and one unmet fails the install" {
    _provider bread "${MET}" 0
    _provider ezo "${UNMET}" 1
    _runtime_yaml bread ezo
    run phase_host_preflight
    [ "$status" -ne 0 ]
    [[ "${output}" == *"bread0: host requirements met"* ]]
    [[ "${output}" == *"ezo0: host requirements unmet"* ]]
}

# ---------------------------------------------------------------------------
# CLI surface
# ---------------------------------------------------------------------------

@test "--allow-unmet-host is parsed and documented" {
    parse_args --allow-unmet-host
    [ "${ALLOW_UNMET_HOST}" -eq 1 ]
    run show_help
    [[ "${output}" == *"--allow-unmet-host"* ]]
}

@test "dry run lists the host preflight and no I2C steps" {
    run bash "${BATS_TEST_DIRNAME}/helpers/run_as_root.sh" "${INSTALL_SH}" --dry-run --local "${BATS_TEST_TMPDIR}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"host preflight"* ]]
    [[ "${output}" != *"I2C"* ]]
    [[ "${output}" != *"i2c-tools"* ]]
}
