#!/usr/bin/env bash
# The user path, with a real install.sh on a real host (#516). Each scenario
# runs this checkout's tools/install.sh against released runtime and simulator
# binaries (tests/e2e/e2e-sim pins them), and asserts on install.sh's exit
# code, its output, and the runtime's API. Run as root on a disposable host
# with systemd: the install is real, and so are the uninstalls.
#
#   install-walk.sh fresh      install --with-observability; switch variant;
#                              restart; --uninstall and reinstall
#   install-walk.sh recover    an --uninstall by v0.1.53, which deleted the
#                              InfluxDB credentials, then a current install
#   install-walk.sh upgrade    the previous release's install.sh and pins,
#                              then this one's
#   install-walk.sh workbench  anolis-provision install from the released
#                              workbench, and what its summary shows
#   install-walk.sh safestate  a safe state that fails at run time warns; one
#                              the runtime cannot plan fails the install
#   install-walk.sh handcopy   a variant copied over runtime.yaml by hand and a
#                              restart keep the data dir and a latched e-stop
#
# Each scenario ends by breaking what its last check depends on and requiring
# that check to fail, so a check that cannot fail fails the walk.
#
# Logs of every install go to ${WALK_LOGS:-/tmp/install-walk}.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALL_SH="${ROOT}/tools/install.sh"
PROJECT="${ROOT}/tests/e2e/e2e-sim"
LOGS="${WALK_LOGS:-/tmp/install-walk}"
API="http://127.0.0.1:8080"
mkdir -p "${LOGS}"

step() { printf '\n=== %s\n' "$*"; }
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

# run_install <log name> <install.sh> <args...>: run it, keep its whole output,
# fail the walk on a non-zero exit with the end of that output.
run_install() {
    local name="$1" script="$2"
    shift 2
    local log="${LOGS}/${name}.log"
    step "install.sh $* (${name})"
    if ! bash "${script}" "$@" >"${log}" 2>&1; then
        tail -40 "${log}" >&2
        fail "${name}: install.sh $* exited non-zero (log: ${log})"
    fi
    pass "${name}: exit 0"
}

# run_install_failing <log name> <install.sh> <args...>: as run_install, but
# the install must exit non-zero.
run_install_failing() {
    local name="$1" script="$2"
    shift 2
    local log="${LOGS}/${name}.log"
    step "install.sh $* (${name}, must fail)"
    if bash "${script}" "$@" >"${log}" 2>&1; then
        tail -40 "${log}" >&2
        fail "${name}: install.sh $* exited 0 (log: ${log})"
    fi
    pass "${name}: exit non-zero"
}

# refute_log <log name> <fixed text>: the install's output does not carry it.
refute_log() {
    if grep -qF -- "$2" "${LOGS}/$1.log"; then
        tail -40 "${LOGS}/$1.log" >&2
        fail "$1: output has: $2"
    fi
    pass "$1: does not say '$2'"
}

# expect_log <log name> <fixed text>: the install's output carries it.
expect_log() {
    grep -qF -- "$2" "${LOGS}/$1.log" || { tail -40 "${LOGS}/$1.log" >&2; fail "$1: output lacks: $2"; }
    pass "$1: says '$2'"
}

# api <path>: GET with the runtime's token, read from runtime.env and given to
# curl on stdin, never on its command line.
api() {
    sed -n 's/^ANOLIS_API_TOKEN=/Authorization: Bearer /p' /etc/anolis/runtime.env | curl -fsS -H @- "${API}$1"
}

# eventually <seconds> <description> <command...>: retry until it succeeds.
eventually() {
    local seconds="$1" what="$2"
    shift 2
    local deadline=$((SECONDS + seconds))
    until "$@" >/dev/null 2>&1; do
        ((SECONDS < deadline)) || fail "not within ${seconds}s: ${what}"
        sleep 2
    done
    pass "${what}"
}

# api_post <path> [json body]: POST with the runtime's token, as api does.
api_post() {
    local body=()
    [[ $# -lt 2 ]] || body=(-H 'Content-Type: application/json' --data "$2")
    sed -n 's/^ANOLIS_API_TOKEN=/Authorization: Bearer /p' /etc/anolis/runtime.env |
        curl -fsS -X POST -H @- "${body[@]}" "${API}$1"
}

runtime_answers() { api /v0/runtime/status >/dev/null; }
estop_latched() { api /v0/runtime/status | jq -e '.estop.latched == true'; }
run_listed() { api /v0/runs | jq -e --arg id "$1" 'any(.runs[]; .run_id == $id)'; }

runtime_ok() { api /v0/runtime/status | jq -e '.status.code == "OK"'; }
safe_state_achieved() { api /v0/runtime/status | jq -e '.startup_safe_state.safe_state_achieved == true'; }
runtime_version_is() { api /v0/runtime/status | jq -e --arg v "$1" '.version == $v'; }
# data_dir_is <dir>: the running runtime logged it as its data dir at startup.
data_dir_is() {
    local logged
    logged="$(journalctl -u anolis-runtime -o cat -n 400 | grep -F '[Config] Data dir: ' | tail -1)"
    [[ "${logged}" == "[Config] Data dir: $1" ]] || fail "the runtime's data dir is not $1: ${logged:-not logged}"
    pass "the runtime's data dir is $1"
}
telemetry_writing() { api /v0/telemetry/status | jq -e '.enabled and .connected and .total_written > 0'; }
influxdb_up() { curl -fsS http://127.0.0.1:8086/health | jq -e '.status == "pass"'; }
grafana_up() { curl -fsS http://127.0.0.1:3000/api/health | jq -e '.database == "ok"'; }

check_runtime() {
    eventually 60 "the runtime answers OK" runtime_ok
    eventually 30 "the startup safe state was achieved" safe_state_achieved
}

check_observability() {
    eventually 60 "InfluxDB is up" influxdb_up
    eventually 60 "Grafana is up" grafana_up
    grep -qE '^INFLUXDB_TOKEN=.+' /etc/anolis/runtime.env || fail "runtime.env has no INFLUXDB_TOKEN"
    pass "the runtime has an InfluxDB token"
}

check_telemetry() { eventually "${1:-90}" "the runtime is writing telemetry to InfluxDB" telemetry_writing; }

# The check-the-check: with the runtime's InfluxDB token gone, telemetry
# cannot be written, and check_telemetry must say so.
selftest_telemetry() {
    step "self-test: telemetry must fail without the runtime's InfluxDB token"
    sed -i '/^INFLUXDB_TOKEN=/d' /etc/anolis/runtime.env
    systemctl restart anolis-runtime
    eventually 60 "the runtime answers OK (no token)" runtime_ok
    if (check_telemetry 30) >/dev/null 2>&1; then
        fail "check_telemetry passed with no InfluxDB token: the check cannot fail"
    fi
    pass "check_telemetry fails without the token"
}

# A released install.sh, from that release's assets.
release_install_sh() {
    local version="$1" dest="${LOGS}/install-$1.sh"
    curl -fsSL -o "${dest}" "https://github.com/anolishq/anolis/releases/download/v${version}/install.sh"
    printf '%s' "${dest}"
}

# The fixture with its runtime pin replaced: a copy, so the checkout is untouched.
project_pinned_to() {
    local version="$1" dir="${LOGS}/project-$1/e2e-sim"
    rm -rf "${dir}"
    mkdir -p "$(dirname "${dir}")"
    cp -r "${PROJECT}" "${dir}"
    sed -i -E "/^  runtime:/,/^  [a-z]/ s/^(    version: ).*/\\1${version}/" "${dir}/machine-profile.yaml"
    grep -q "version: ${version}" "${dir}/machine-profile.yaml" || fail "could not pin ${dir} to ${version}"
    # A runtime before 0.2.0 resolves a relative data_dir against its working
    # directory, so the fixture's variants carry none for it, as they did then.
    sed -i -e '/^  data_dir: \.\.\/anolis-data$/d' -e '/#643/d' -e '/so a copy of this file anywhere keeps it$/d' "${dir}"/config/anolis-runtime.*.yaml
    ! grep -q data_dir "${dir}"/config/anolis-runtime.*.yaml || fail "could not strip data_dir from ${dir}"
    printf '%s' "${dir}"
}

scenario_fresh() {
    run_install fresh "${INSTALL_SH}" --project "${PROJECT}" --variant manual --with-observability --keep-hostname
    expect_log fresh "health: startup safe state achieved"
    expect_log fresh "observability: influxdb (:8086) and grafana (:3000) up"
    check_runtime
    check_observability

    run_install variant "${INSTALL_SH}" --project "${PROJECT}" --variant telemetry --with-observability \
        --replace-config --keep-hostname
    expect_log variant "config (runtime): replaced with variant 'telemetry'"
    check_runtime
    check_telemetry

    step "systemctl restart anolis-runtime"
    systemctl restart anolis-runtime
    check_runtime
    check_telemetry

    run_install uninstall "${INSTALL_SH}" --uninstall
    expect_log uninstall "Kept: the InfluxDB credentials"
    run_install reinstall "${INSTALL_SH}" --project "${PROJECT}" --variant telemetry --with-observability --keep-hostname
    check_runtime
    check_observability
    check_telemetry

    selftest_telemetry
}

scenario_recover() {
    run_install fresh "${INSTALL_SH}" --project "${PROJECT}" --variant telemetry --with-observability --keep-hostname
    check_runtime
    check_telemetry

    local old
    old="$(release_install_sh 0.1.53)"
    run_install old-uninstall "${old}" --uninstall
    [[ ! -e /etc/anolis/observability.env ]] || fail "v0.1.53's uninstall kept observability.env; this scenario needs the stripped host"
    pass "v0.1.53's uninstall deleted the InfluxDB credentials and kept the instance"

    run_install recover "${INSTALL_SH}" --project "${PROJECT}" --variant telemetry --with-observability --keep-hostname
    expect_log recover "observability: minted new write and read tokens"
    check_runtime
    check_observability
    check_telemetry

    selftest_telemetry
}

scenario_upgrade() {
    local previous=0.1.57 current old project
    current="$(sed -n '/^  runtime:/,/^  [a-z]/ s/^    version: //p' "${PROJECT}/machine-profile.yaml")"
    old="$(release_install_sh "${previous}")"
    project="$(project_pinned_to "${previous}")"
    run_install previous "${old}" --project "${project}" --variant telemetry --with-observability --keep-hostname
    check_runtime
    eventually 30 "the runtime is v${previous}" runtime_version_is "${previous}"

    # The latch and the run journal are in the data dir, which the upgrade
    # names in the kept runtime.yaml where it already was (#562).
    local run_id
    run_id="$(api_post /v0/runs '{"experiment_label":"install-walk upgrade"}' | jq -er '.run.run_id')" ||
        fail "could not open a run on v${previous}"
    api_post /v0/estop >/dev/null || fail "could not latch the e-stop on v${previous}"
    pass "v${previous}: run ${run_id} open and the e-stop latched"

    run_install upgrade "${INSTALL_SH}" --project "${PROJECT}" --variant telemetry --with-observability --keep-hostname
    expect_log upgrade "set runtime.data_dir to /opt/anolis/anolis-data"
    grep -qx '  data_dir: /opt/anolis/anolis-data' /opt/anolis/config/runtime.yaml ||
        fail "the kept runtime.yaml does not name /opt/anolis/anolis-data"
    pass "the kept runtime.yaml names /opt/anolis/anolis-data"
    eventually 60 "the upgraded runtime answers" runtime_answers
    eventually 30 "the e-stop is still latched after the upgrade" estop_latched
    eventually 30 "run ${run_id} is still in the journal after the upgrade" run_listed "${run_id}"
    api_post /v0/estop/clear >/dev/null || fail "could not clear the e-stop after the upgrade"
    check_runtime
    eventually 30 "the runtime is v${current}" runtime_version_is "${current}"
    check_observability
    check_telemetry

    selftest_telemetry
}

# A safe state that ran and failed is a warning, and one the runtime cannot
# plan on these devices fails the install (#616).
scenario_safestate() {
    run_install manual "${INSTALL_SH}" --project "${PROJECT}" --variant manual --keep-hostname
    check_runtime

    run_install runfail "${INSTALL_SH}" --project "${PROJECT}" --variant runfail --replace-config --keep-hostname
    expect_log runfail "health: startup safe state NOT achieved"
    expect_log runfail "sim0/motorctl0 set_motor_duty"
    refute_log runfail "cannot run on this machine's devices"

    run_install_failing unplannable "${INSTALL_SH}" --project "${PROJECT}" --variant unplannable --replace-config --keep-hostname
    expect_log unplannable "health: safety.safe_state cannot run on this machine's devices: sim0/"
    expect_log unplannable "the install did NOT succeed"

    # The check-the-check: the same install over a plannable safe state
    # exits 0, so the failure above is the safe state's.
    step "self-test: an install over a plannable safe state must exit 0"
    run_install replanned "${INSTALL_SH}" --project "${PROJECT}" --variant manual --replace-config --keep-hostname
    expect_log replanned "health: startup safe state achieved"
}

# A project quickstart's variant switch (#643): copy a variant over the
# installed runtime.yaml and restart. The variant names its data dir against
# the config's directory, so the latch and the journal stay where install.sh
# put them, and a latched e-stop still holds.
scenario_handcopy() {
    run_install manual "${INSTALL_SH}" --project "${PROJECT}" --variant manual --keep-hostname
    check_runtime
    data_dir_is /opt/anolis/anolis-data
    api_post /v0/estop >/dev/null || fail "could not latch the e-stop"
    eventually 30 "the e-stop is latched" estop_latched

    step "cp the manual variant over runtime.yaml; systemctl restart anolis-runtime"
    cp "${PROJECT}/config/anolis-runtime.manual.yaml" /opt/anolis/config/runtime.yaml
    systemctl restart anolis-runtime
    eventually 60 "the runtime answers after the copy" runtime_answers
    data_dir_is /opt/anolis/anolis-data
    eventually 30 "the e-stop is still latched after the copy" estop_latched

    # The check-the-check: the same copy without the variant's data_dir moves
    # the data dir beside the config, and the latch is left behind.
    step "self-test: a copied variant without data_dir must move the data dir"
    sed '/^  data_dir:/d' "${PROJECT}/config/anolis-runtime.manual.yaml" >/opt/anolis/config/runtime.yaml
    systemctl restart anolis-runtime
    eventually 60 "the runtime answers after the copy without data_dir" runtime_answers
    if (data_dir_is /opt/anolis/anolis-data) >/dev/null 2>&1; then
        fail "data_dir_is passed for a config without data_dir: the check cannot fail"
    fi
    if estop_latched >/dev/null 2>&1; then
        fail "the e-stop is latched with the data dir moved: the latch check cannot fail"
    fi
    pass "without data_dir the data dir moves and the latch is left behind"
}

# The workbench's own route: it fetches the install.sh of the runtime the
# project pins and runs it, so this checks the released install.sh behind it.
scenario_workbench() {
    step "pip install anolis-workbench"
    python3 -m venv "${LOGS}/workbench-venv"
    "${LOGS}/workbench-venv/bin/pip" install --quiet anolis-workbench
    local provision="${LOGS}/workbench-venv/bin/anolis-provision" log="${LOGS}/workbench.log"
    pass "$("${LOGS}/workbench-venv/bin/anolis-workbench" --version)"

    step "anolis-provision install --system ${PROJECT} --project e2e-sim --with-observability"
    if ! "${provision}" install --system "${PROJECT}" --project e2e-sim --with-observability >"${log}" 2>&1; then
        tail -60 "${log}" >&2
        fail "workbench: anolis-provision install exited non-zero (log: ${log})"
    fi
    pass "workbench: exit 0"
    expect_log workbench "Installation complete!"
    expect_log workbench "Health (install.sh):"
    expect_log workbench "health: startup safe state achieved"
    expect_log workbench "Observability (install.sh):"
    expect_log workbench "Reaching Grafana:"
    check_runtime
    check_observability

    step "self-test: the Grafana check must fail with grafana-server stopped"
    systemctl stop grafana-server
    if (eventually 10 "Grafana is up" grafana_up) >/dev/null 2>&1; then
        fail "the Grafana check passed with grafana-server stopped: the check cannot fail"
    fi
    pass "the Grafana check fails with grafana-server stopped"
}

[[ ${EUID} -eq 0 ]] || fail "run as root, on a disposable host"
case "${1:-}" in
    fresh) scenario_fresh ;;
    recover) scenario_recover ;;
    upgrade) scenario_upgrade ;;
    workbench) scenario_workbench ;;
    safestate) scenario_safestate ;;
    handcopy) scenario_handcopy ;;
    *) fail "usage: $0 fresh|recover|upgrade|workbench|safestate|handcopy" ;;
esac
printf '\nWALK PASSED: %s\n' "$1"
