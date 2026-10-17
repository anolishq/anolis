#!/usr/bin/env bats
# #408: _obs_provision_grafana checks the observability tarball against the
# release's SHA256SUMS. These source install.sh (main() guarded) and run the
# real function with the network and every host write stubbed: curl serves a
# fake release dir, and install(1) runs only when every path it names is under
# BATS_TEST_TMPDIR, otherwise it is recorded and skipped.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    # GRAFANA_DROPIN_DIR is readonly in install.sh; point it into the test
    # dir before sourcing, as rollback_test.bats does for SYSTEMD_DIR.
    source <(sed "s|^readonly GRAFANA_DROPIN_DIR=.*|readonly GRAFANA_DROPIN_DIR=\"${BATS_TEST_TMPDIR}/dropin\"|" "${INSTALL_SH}")
    set +u  # errexit+pipefail stay on (anolishq/anolis#207)

    export TMPDIR="${BATS_TEST_TMPDIR}"
    GITHUB_TOKEN=""
    GRAFANA_NETWORK=0
    PREFIX="${BATS_TEST_TMPDIR}/prefix"
    RELEASE="${BATS_TEST_TMPDIR}/release"
    HOST_WRITES="${BATS_TEST_TMPDIR}/host-writes.log"
    mkdir -p "${PREFIX}/bin" "${RELEASE}"
    printf '#!/bin/sh\necho "anolis-runtime 9.9.9"\n' > "${PREFIX}/bin/anolis-runtime"
    chmod +x "${PREFIX}/bin/anolis-runtime"

    # The copy a previous run provisioned.
    mkdir -p "${PREFIX}/observability"
    printf 'old' > "${PREFIX}/observability/marker"

    # The release asset: a new copy with the layout the function installs.
    ASSET="anolis-9.9.9-observability.tar.gz"
    local t="${BATS_TEST_TMPDIR}/asset"
    mkdir -p "${t}/grafana/dashboards" "${t}/grafana/provisioning/datasources" \
        "${t}/grafana/provisioning/dashboards"
    printf '{}' > "${t}/grafana/dashboards/runtime.json"
    printf 'url: ${INFLUX_URL}\n' > "${t}/grafana/provisioning/datasources/influxdb.yml"
    printf 'apiVersion: 1\n' > "${t}/grafana/provisioning/dashboards/default.yml"
    printf 'new' > "${t}/marker"
    tar -czf "${RELEASE}/${ASSET}" -C "${t}" .
    GOOD_SHA=$(sha256sum "${RELEASE}/${ASSET}" | awk '{print $1}')

    # Serve release URLs from ${RELEASE}, as the stub in sha256sums_test.bats
    # does: a missing file is curl -f's 22 with HTTP 404, SUMS_FAIL="<exit>
    # <http_code>" makes the SHA256SUMS request fail that way instead, and
    # -w '%{http_code}' prints the code. Any other URL (the vendor apt keys,
    # Grafana's health endpoint) succeeds.
    SUMS_FAIL=""
    curl() {
        local out="" url="" w="" a code=200 rc=0
        while [[ $# -gt 0 ]]; do
            a="$1"; shift
            case "${a}" in
                -o) out="$1"; shift ;;
                -w) w="$1"; shift ;;
                -H) shift ;;
                -*) ;;
                *)  url="${a}" ;;
            esac
        done
        if [[ "${url}" != https://github.com/*/releases/download/* ]]; then
            return 0
        elif [[ -n "${SUMS_FAIL}" && "${url}" == */SHA256SUMS ]]; then
            read -r rc code <<< "${SUMS_FAIL}"
        elif [[ -f "${RELEASE}/${url##*/}" ]]; then
            cp "${RELEASE}/${url##*/}" "${out}"
        else
            rc=22 code=404
        fi
        [[ "${w}" == '%{http_code}' ]] && printf '%s' "${code}"
        return "${rc}"
    }
    install() {
        local a
        for a in "$@"; do
            case "${a}" in
                -*|[0-7][0-7][0-7]) ;;
                "${BATS_TEST_TMPDIR}"/*) ;;
                *) printf 'install %s\n' "$*" >> "${HOST_WRITES}"; return 0 ;;
            esac
        done
        command install "$@"
    }
    systemctl() { :; }
}

# Run the function and report whether control came back to the caller, the
# way phase_observability calls it.
_provision() {
    if _obs_provision_grafana; then echo "RETURNED 0"; else echo "RETURNED $?"; fi
}

@test "#408 an observability tarball matching SHA256SUMS is provisioned" {
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    run _provision
    [[ "$output" == *"RETURNED 0"* ]]
    [[ "$output" != *"not verified"* ]]
    [ "$(cat "${PREFIX}/observability/marker")" = "new" ]
    grep -q 'dashboards' "${HOST_WRITES}"
}

@test "#408 a mismatched observability tarball is skipped and the previous copy kept" {
    local bad
    bad=$(printf 'other bytes' | sha256sum | awk '{print $1}')
    printf '%s  %s\n' "${bad}" "${ASSET}" > "${RELEASE}/SHA256SUMS"
    run _provision
    [[ "$output" == *"RETURNED 1"* ]]
    [[ "$output" == *"expected ${bad}, got ${GOOD_SHA}"* ]]
    [[ "$output" == *"${ASSET} failed verification; dashboards not updated"* ]]
    [ "$(cat "${PREFIX}/observability/marker")" = "old" ]
    [ ! -e "${HOST_WRITES}" ]
}

@test "#408 a SHA256SUMS with no entry for the observability tarball warns and provisions" {
    printf '%s  %s\n' "${GOOD_SHA}" "anolis-9.9.9-linux-x86_64.tar.gz" > "${RELEASE}/SHA256SUMS"
    run _provision
    [[ "$output" == *"RETURNED 0"* ]]
    [[ "$output" == *"anolishq/anolis v9.9.9 has no entry for ${ASSET}"* ]]
    [ "$(cat "${PREFIX}/observability/marker")" = "new" ]
}

@test "#408 a release with no SHA256SUMS warns and provisions the observability tarball" {
    run _provision
    [[ "$output" == *"RETURNED 0"* ]]
    [[ "$output" == *"no SHA256SUMS fetched for anolishq/anolis v9.9.9; ${ASSET} is not verified"* ]]
    [ "$(cat "${PREFIX}/observability/marker")" = "new" ]
}

@test "#408 a failed SHA256SUMS fetch skips only observability, not the install" {
    # Connection refused: curl exit 7, HTTP 000. The helper dies on this; the
    # observability phase must turn it into a skip and return to main().
    SUMS_FAIL="7 000"
    printf '%s  %s\n' "${GOOD_SHA}" "${ASSET}" > "${RELEASE}/SHA256SUMS"

    run _provision
    [[ "$output" == *"RETURNED 1"* ]]
    [[ "$output" == *"could not fetch SHA256SUMS for anolishq/anolis v9.9.9 (curl exit 7, HTTP 000)"* ]]
    [[ "$output" == *"${ASSET} failed verification; dashboards not updated"* ]]
    [ "$(cat "${PREFIX}/observability/marker")" = "old" ]
    [ ! -e "${HOST_WRITES}" ]

    # The real phase, with only its host and package steps stubbed.
    WITH_OBSERVABILITY=1
    apt-get() { :; }
    dpkg() { :; }
    sleep() { :; }
    _obs_apt_repos() { :; }
    _obs_write_influx_dropin() { :; }
    _obs_setup_influx() { :; }
    _phase() { phase_observability; echo "CONTINUED rc=$?"; }
    run _phase
    [[ "$output" == *"grafana provisioning failed"* ]]
    [[ "$output" == *"CONTINUED rc=0"* ]]
    [ "$(cat "${PREFIX}/observability/marker")" = "old" ]
    [ ! -e "${HOST_WRITES}" ]
}
