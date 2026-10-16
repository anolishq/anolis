#!/usr/bin/env bats
# install.sh refuses to install an older runtime over a newer one unless
# --allow-downgrade is given (#366). The installed version is read from
# bin/anolis-runtime --version. Runs without root, network or systemd: the
# prefix is a temp dir and chown/systemctl are recording stubs.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
RUN_AS_ROOT="${BATS_TEST_DIRNAME}/helpers/run_as_root.sh"

# A runtime stub that reports <version>.
_runtime_stub() {  # <path> <version>
    mkdir -p "$(dirname "$1")"
    printf '#!/bin/sh\necho "anolis-runtime %s"\n' "$2" > "$1"
    chmod +x "$1"
}

# A bundle that passes the read-only phases main runs before the version
# check: a runtime reporting <version>, a manifest naming it, and a project
# with an inert manual variant.
_mk_bundle() {  # <dir> <version>
    local dir="$1" version="$2" proj="$1/projects/p"
    _runtime_stub "${dir}/bin/anolis-runtime" "${version}"
    printf '{\n  "version": "%s"\n}\n' "${version}" > "${dir}/manifest.json"
    mkdir -p "${proj}/config"
    printf 'runtime_profiles:\n  manual: config/anolis-runtime.manual.yaml\n' > "${proj}/machine-profile.yaml"
    printf 'http:\n  bind: 127.0.0.1\n' > "${proj}/config/anolis-runtime.manual.yaml"
}

_source_install_sh() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    STUB_LOG="${BATS_TEST_TMPDIR}/stub.log"
    chown() { echo "chown $*" >> "${STUB_LOG}"; }
    systemctl() { echo "systemctl $*" >> "${STUB_LOG}"; }
}

# The phases main runs from the version check through the binary install.
# Chained, because bats' run does not stop at a step that returns non-zero.
_install_bundle() {  # <version>
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bundle-$1"
    _mk_bundle "${BUNDLE_DIR}" "$1"
    phase_version_check && phase_directories && phase_backup && phase_install_binaries
}

@test "--help documents --allow-downgrade" {
    run bash "${INSTALL_SH}" --help
    [ "$status" -eq 0 ]
    [[ "${output}" == *"--allow-downgrade"* ]]
}

@test "an older bundle over a newer runtime exits non-zero and changes nothing (#366)" {
    local prefix="${BATS_TEST_TMPDIR}/prefix" bundle="${BATS_TEST_TMPDIR}/bundle"
    _runtime_stub "${prefix}/bin/anolis-runtime" 0.1.44
    _mk_bundle "${bundle}" 0.1.39
    find "${prefix}" -exec stat -c '%n %s %Y' {} + | sort > "${BATS_TEST_TMPDIR}/before"

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --local "${bundle}" --prefix "${prefix}" --no-start
    [ "$status" -ne 0 ]
    [[ "${output}" == *"0.1.39 is older than the installed 0.1.44"* ]]
    [[ "${output}" == *"--rollback"* ]]
    [[ "${output}" == *"--allow-downgrade"* ]]
    [[ "${output}" == *"refusing to downgrade the runtime"* ]]
    [[ "${output}" != *"system user"* ]]

    find "${prefix}" -exec stat -c '%n %s %Y' {} + | sort > "${BATS_TEST_TMPDIR}/after"
    diff "${BATS_TEST_TMPDIR}/before" "${BATS_TEST_TMPDIR}/after"
    [ "$("${prefix}/bin/anolis-runtime" --version)" = "anolis-runtime 0.1.44" ]
}

@test "older without --allow-downgrade: refused before the binaries change (#366)" {
    _source_install_sh
    _install_bundle 0.1.44

    run _install_bundle 0.1.39
    [ "$status" -ne 0 ]
    [[ "${output}" == *"refusing to downgrade the runtime"* ]]
    [ "$("${PREFIX}/bin/anolis-runtime" --version)" = "anolis-runtime 0.1.44" ]
}

@test "older with --allow-downgrade: installs it with a warning (#366)" {
    _source_install_sh
    _install_bundle 0.1.44
    ALLOW_DOWNGRADE=1

    run _install_bundle 0.1.39
    [ "$status" -eq 0 ]
    [[ "${output}" == *"downgrading the runtime from 0.1.44 to 0.1.39 (--allow-downgrade)"* ]]
    [ "$("${PREFIX}/bin/anolis-runtime" --version)" = "anolis-runtime 0.1.39" ]
}

@test "same version, with or without --allow-downgrade: redeploys (#366)" {
    _source_install_sh
    _install_bundle 0.1.44

    run _install_bundle 0.1.44
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: 0.1.44, same as installed"* ]]

    ALLOW_DOWNGRADE=1
    run _install_bundle 0.1.44
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: 0.1.44, same as installed"* ]]
    [[ "${output}" != *"downgrading"* ]]
}

@test "newer version, with or without --allow-downgrade: upgrades (#366)" {
    _source_install_sh
    _install_bundle 0.1.9

    run _install_bundle 0.1.10
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: upgrade 0.1.9 -> 0.1.10"* ]]
    [ "$("${PREFIX}/bin/anolis-runtime" --version)" = "anolis-runtime 0.1.10" ]

    ALLOW_DOWNGRADE=1
    run _install_bundle 0.1.11
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: upgrade 0.1.10 -> 0.1.11"* ]]
    [[ "${output}" != *"downgrading"* ]]
}

@test "the installed version is read from the binary, not manifest.json (#366)" {
    _source_install_sh
    _install_bundle 0.1.44
    # A manifest left stale by a rollback (#345) names an older version.
    printf '{\n  "version": "0.1.30"\n}\n' > "${PREFIX}/manifest.json"

    run _install_bundle 0.1.39
    [ "$status" -ne 0 ]
    [[ "${output}" == *"0.1.39 is older than the installed 0.1.44"* ]]
}

@test "--dry-run --local reports the downgrade and changes nothing (#366)" {
    local prefix="${BATS_TEST_TMPDIR}/prefix" bundle="${BATS_TEST_TMPDIR}/bundle"
    _runtime_stub "${prefix}/bin/anolis-runtime" 0.1.44
    _mk_bundle "${bundle}" 0.1.39

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --local "${bundle}" --prefix "${prefix}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"would refuse to downgrade the runtime from 0.1.44 to 0.1.39"* ]]
    [ "$("${prefix}/bin/anolis-runtime" --version)" = "anolis-runtime 0.1.44" ]

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --local "${bundle}" --prefix "${prefix}" --allow-downgrade
    [ "$status" -eq 0 ]
    [[ "${output}" == *"downgrading the runtime from 0.1.44 to 0.1.39 (--allow-downgrade)"* ]]
}

@test "--dry-run --local reports an upgrade (#366)" {
    local prefix="${BATS_TEST_TMPDIR}/prefix" bundle="${BATS_TEST_TMPDIR}/bundle"
    _runtime_stub "${prefix}/bin/anolis-runtime" 0.1.39
    _mk_bundle "${bundle}" 0.1.44

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --local "${bundle}" --prefix "${prefix}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: upgrade 0.1.39 -> 0.1.44"* ]]
}

@test "--dry-run --local reads a bundle tarball for the comparison (#366)" {
    local prefix="${BATS_TEST_TMPDIR}/prefix" bundle="${BATS_TEST_TMPDIR}/src/anolis-p-0.1.39"
    _runtime_stub "${prefix}/bin/anolis-runtime" 0.1.44
    _mk_bundle "${bundle}" 0.1.39
    tar -czf "${BATS_TEST_TMPDIR}/b.tar.gz" -C "${BATS_TEST_TMPDIR}/src" anolis-p-0.1.39

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --local "${BATS_TEST_TMPDIR}/b.tar.gz" --prefix "${prefix}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"would refuse to downgrade the runtime from 0.1.44 to 0.1.39"* ]]
}

@test "--dry-run --local checks the prefix the bundle was staged for (#366)" {
    local prefix="${BATS_TEST_TMPDIR}/pfx" bundle="${BATS_TEST_TMPDIR}/bundle"
    _runtime_stub "${prefix}/bin/anolis-runtime" 0.1.44
    _mk_bundle "${bundle}" 0.1.39
    printf '{\n  "version": "0.1.39",\n  "prefix": "%s"\n}\n' "${prefix}" > "${bundle}/manifest.json"

    # No --prefix: the dry run adopts the staged one, as the install does.
    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --local "${bundle}"
    [ "$status" -eq 0 ]
    [[ "${output}" == *"would refuse to downgrade the runtime from 0.1.44 to 0.1.39"* ]]
    [[ "${output}" == *"create directories at ${prefix}"* ]]
}

@test "--dry-run --local verifies checksums before it runs the bundle's binary (#366)" {
    local prefix="${BATS_TEST_TMPDIR}/prefix" bundle="${BATS_TEST_TMPDIR}/bundle"
    _runtime_stub "${prefix}/bin/anolis-runtime" 0.1.44
    _mk_bundle "${bundle}" 0.1.39
    printf '#!/bin/sh\ntouch "%s"\necho "anolis-runtime 0.1.39"\n' "${BATS_TEST_TMPDIR}/ran" > "${bundle}/bin/anolis-runtime"
    printf '%064d  ./bin/anolis-runtime\n' 0 > "${bundle}/checksums.sha256"

    run bash "${RUN_AS_ROOT}" "${INSTALL_SH}" --dry-run --local "${bundle}" --prefix "${prefix}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"Checksum verification failed"* ]]
    [ ! -e "${BATS_TEST_TMPDIR}/ran" ]
}

@test "a bundle runtime that will not run falls back to its manifest, normalised to X.Y.Z (#366)" {
    _source_install_sh
    _install_bundle 0.1.44
    BUNDLE_DIR="${BATS_TEST_TMPDIR}/bundle-fallback"
    _mk_bundle "${BUNDLE_DIR}" 0.1.44
    chmod -x "${BUNDLE_DIR}/bin/anolis-runtime"

    printf '{\n  "version": "0.1.44-rc1"\n}\n' > "${BUNDLE_DIR}/manifest.json"
    run phase_version_check
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: 0.1.44, same as installed"* ]]

    printf '{\n  "version": "0.1.39-rc1"\n}\n' > "${BUNDLE_DIR}/manifest.json"
    run phase_version_check
    [ "$status" -ne 0 ]
    [[ "${output}" == *"0.1.39 is older than the installed 0.1.44"* ]]
}

@test "an installed runtime that will not report a version: warns and installs (#366)" {
    _source_install_sh
    _install_bundle 0.1.44
    printf 'not a runtime\n' > "${PREFIX}/bin/anolis-runtime"

    run _install_bundle 0.1.39
    [ "$status" -eq 0 ]
    [[ "${output}" == *"version: cannot compare (installed unknown, bundle 0.1.39); continuing"* ]]
    [ "$("${PREFIX}/bin/anolis-runtime" --version)" = "anolis-runtime 0.1.39" ]
}
