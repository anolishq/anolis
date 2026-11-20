#!/usr/bin/env bats
# The commands --stage prints for the target (#533). They named the staging
# host's absolute output directory and a ./install.sh that is not on the
# target. They now work verbatim from the directory on the target that holds
# the carried .tar.gz and .sha256, which this test checks by running them
# there. assemble_bundle is a stub, so nothing is downloaded.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    stage_check_deps() { :; }
    resolve_target_arch() { echo arm64; }
    assemble_bundle() {
        mkdir -p "$1"
        printf '#!/usr/bin/env bash\necho bundled installer\n' > "$1/install.sh"
        chmod +x "$1/install.sh"
        ASSEMBLED_PROFILE=demo
        ASSEMBLED_RUNTIME_VERSION=0.0.1
    }
    STAGE_DIR="${BATS_TEST_TMPDIR}/stage-out"
    BUNDLE=anolis-demo-0.0.1-arm64
}

@test "533: the stage hints name no staging path and use the bundle's own install.sh" {
    run do_stage
    [ "${status}" -eq 0 ]
    plain=$(sed 's/\x1b\[[0-9;]*m//g' <<< "${output}")
    [[ "${plain}" == *"sha256sum -c ${BUNDLE}.tar.gz.sha256"* ]]
    [[ "${plain}" == *"tar xzf ${BUNDLE}.tar.gz"* ]]
    [[ "${plain}" == *"sudo ${BUNDLE}/install.sh --local ${BUNDLE}.tar.gz"* ]]
    [[ "${plain}" != *"./install.sh"* ]]
    # The staging directory may appear only on the line that says where the
    # files were written, never in a command for the target.
    abs_out=$(cd "${STAGE_DIR}" && pwd)
    if grep -F "${abs_out}" <<< "${plain}" | grep -E 'sha256sum|tar xzf|install\.sh'; then
        return 1
    fi
}

@test "533: the printed commands work from a target directory holding only the carried files" {
    run do_stage
    [ "${status}" -eq 0 ]
    plain=$(sed 's/\x1b\[[0-9;]*m//g' <<< "${output}")
    # The three commands exactly as printed, sudo dropped.
    verify=$(sed -n "s/^.*\(sha256sum -c ${BUNDLE}.*\)$/\1/p" <<< "${plain}")
    extract=$(sed -n "s/^.*\(tar xzf ${BUNDLE}.*\)$/\1/p" <<< "${plain}")
    install=$(sed -n "s/^.*sudo \(${BUNDLE}\/install\.sh .*\)$/\1/p" <<< "${plain}")
    [ -n "${verify}" ]
    [ -n "${extract}" ]
    [ -n "${install}" ]

    target="${BATS_TEST_TMPDIR}/target"
    mkdir -p "${target}"
    cp "${STAGE_DIR}/${BUNDLE}.tar.gz" "${STAGE_DIR}/${BUNDLE}.tar.gz.sha256" "${target}/"
    cd "${target}"
    bash -c "${verify}"
    bash -c "${extract}"
    run bash -c "${install}"
    [ "${status}" -eq 0 ]
    [ "${output}" == "bundled installer" ]
}
