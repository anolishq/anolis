#!/usr/bin/env bats
# Tests for the startup safe-state line in install.sh's health check (#471).
#
# Every install restarts the runtime, and the restart runs safety.on_startup
# again. phase_health already reads GET /v0/runtime/status; it now reports
# the startup_safe_state in that body, as a warning only when it was not
# achieved. _runtime_curl is a stub that prints a canned status body, so
# nothing here reaches a runtime or the host.

bats_require_minimum_version 1.5.0

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
    STATUS_URL="http://localhost:8080/v0/runtime/status"
}

# The canned body for the next _runtime_curl call.
status_body() {
    STATUS_BODY="$1"
    _runtime_curl() { printf '%s' "${STATUS_BODY}"; }
}

# The report's lines with colour codes removed.
report() {
    _report_startup_safe_state "${STATUS_URL}" | sed 's/\x1b\[[0-9;]*m//g'
}

run_both_parsers() {
    # jq and python3 must agree; run the report once with each.
    run report
    jq_output="${output}"
    jq_status="${status}"
    jq() { return 127; }
    command() { [[ "$1" == -v && "$2" == jq ]] && return 1; builtin command "$@"; }
    run report
    unset -f jq command
    [ "${status}" -eq "${jq_status}" ] || return 1
    [ "${output}" == "${jq_output}" ] || { echo "python3: ${output}"; echo "jq: ${jq_output}"; return 1; }
}

@test "achieved: one ok line naming the rung" {
    status_body '{"version":"0.1.52","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"hooks","safe_state_achieved":true,"failed_actions":[],"skipped_actions":[]}}'
    run_both_parsers
    [ "${status}" -eq 0 ]
    [ "${output}" == "✓ health: startup safe state achieved (hooks)" ]
}

@test "not achieved: a warning, then each failed and skipped call" {
    status_body '{"version":"0.1.52","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"hooks","safe_state_achieved":false,"failed_actions":[{"device_handle":"bread0/dcmt0","function":"set_mode","success":false,"error":"SET command refused: WATCHDOG_TRIP"}],"skipped_actions":[{"device_handle":"bread0/rlht0","function":"set_mode","reason":"device absent at startup"}]}}'
    run_both_parsers
    [ "${status}" -eq 0 ]
    expected=$(printf '%s\n' \
        "⚠ health: startup safe state NOT achieved" \
        "    bread0/dcmt0 set_mode: SET command refused: WATCHDOG_TRIP" \
        "    bread0/rlht0 set_mode: skipped, device absent at startup")
    [ "${output}" == "${expected}" ]
}

@test "not run under on_startup: leave: a skip line, not a warning" {
    status_body '{"version":"0.1.52","startup_safe_state":{"on_startup":"leave","ran":false,"software_safe_state":"hooks","safe_state_achieved":false,"failed_actions":[]}}'
    run_both_parsers
    [ "${status}" -eq 0 ]
    [ "${output}" == "→ health: startup safe state not run (on_startup: leave)" ]
}

@test "not run with no software safe state declared: a skip line" {
    status_body '{"version":"0.1.52","startup_safe_state":{"on_startup":"safe_state","ran":false,"software_safe_state":"none","safe_state_achieved":false,"failed_actions":[]}}'
    run_both_parsers
    [ "${status}" -eq 0 ]
    [ "${output}" == "→ health: startup safe state not run (no software safe state declared)" ]
}

@test "a runtime that does not report it: no line" {
    status_body '{"version":"0.1.44","mode":"MANUAL"}'
    run_both_parsers
    [ "${status}" -eq 0 ]
    [ "${output}" == "" ]
}

@test "phase_health prints the line and still passes when it was not achieved" {
    status_body '{"version":"0.1.52","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"zero","safe_state_achieved":false,"failed_actions":[{"device_handle":"bread0/dcmt0","function":"set_open_loop","success":false,"error":"refused"}]}}'
    _runtime_port() { echo 8080; }
    _run_registry_available() { return 0; }
    _check_provider_liveness() { :; }
    NO_START=0
    HEALTH_FAILED=0

    run phase_health
    [ "${status}" -eq 0 ]
    [[ "${output}" == *"health: startup safe state NOT achieved"* ]]
    [[ "${output}" == *"    bread0/dcmt0 set_open_loop: refused"* ]]
    phase_health >/dev/null
    [ "${HEALTH_FAILED}" -eq 0 ]
}

# #616: a safe state the config cannot express on these devices fails the
# same way on every restart and every e-stop, so the install fails. The
# runtime says so with cause "config" on the failed action.
health_with() {
    status_body "$1"
    _runtime_port() { echo 8080; }
    _run_registry_available() { return 0; }
    _check_provider_liveness() { :; }
    NO_START=0
    HEALTH_FAILED=0
    SAFE_STATE_UNPLANNABLE=0
}

UNPLANNABLE='{"version":"0.1.58","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"zero","safe_state_achieved":false,"failed_actions":[{"device_handle":"sim0/relayio0","function":"set_relay_ch1","success":false,"error":"cannot zero required non-numeric argument '\''enabled'\''","cause":"config"},{"device_handle":"sim0/tempctl0","function":"set_relay","success":false,"error":"cannot zero required non-numeric argument '\''state'\''","cause":"config"}],"skipped_actions":[]}}'

@test "616: a safe state the config cannot plan fails the install, naming the first refused action" {
    health_with "${UNPLANNABLE}"
    run_both_parsers
    [[ "${output}" == *"safety.safe_state cannot run on this machine's devices"* ]]
    [[ "${output}" == *"sim0/relayio0 set_relay_ch1: cannot zero required non-numeric argument 'enabled'"* ]]
    phase_health >/dev/null
    [ "${SAFE_STATE_UNPLANNABLE}" -eq 1 ]
    run phase_summary
    summary=$(sed 's/\x1b\[[0-9;]*m//g' <<< "${output}")
    [[ "${summary}" == *"sim0/relayio0 set_relay_ch1"*"the install did NOT succeed"* ]]
    [[ "${summary}" == *"Fix safety.safe_state in"* ]]
}

@test "616: a safe state that ran and failed on a tripped board stays a warning" {
    health_with '{"version":"0.1.58","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"hooks","safe_state_achieved":false,"failed_actions":[{"device_handle":"bread0/dcmt0","function":"set_mode","success":false,"error":"SET command refused: WATCHDOG_TRIP","cause":"run"}],"skipped_actions":[]}}'
    run_both_parsers
    [[ "${output}" == *"health: startup safe state NOT achieved"* ]]
    [[ "${output}" != *"cannot run on this machine"* ]]
    phase_health >/dev/null
    [ "${SAFE_STATE_UNPLANNABLE}" -eq 0 ]
    [ "${HEALTH_FAILED}" -eq 0 ]
}

@test "616: a runtime that does not report a cause keeps today's warning" {
    health_with '{"version":"0.1.57","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"zero","safe_state_achieved":false,"failed_actions":[{"device_handle":"sim0/relayio0","function":"set_relay_ch1","success":false,"error":"cannot zero required non-numeric argument '\''enabled'\''"}],"skipped_actions":[]}}'
    run_both_parsers
    [[ "${output}" == *"health: startup safe state NOT achieved"* ]]
    phase_health >/dev/null
    [ "${SAFE_STATE_UNPLANNABLE}" -eq 0 ]
}

@test "616: the install exits non-zero for a safe state that cannot be planned, and only then" {
    health_with "${UNPLANNABLE}"
    phase_health >/dev/null
    _install_failed
    health_with '{"version":"0.1.58","startup_safe_state":{"on_startup":"safe_state","ran":true,"software_safe_state":"hooks","safe_state_achieved":false,"failed_actions":[{"device_handle":"bread0/dcmt0","function":"set_mode","success":false,"error":"SET command refused: WATCHDOG_TRIP","cause":"run"}],"skipped_actions":[]}}'
    phase_health >/dev/null
    ! _install_failed
    grep -q '^    if _install_failed; then$' "${INSTALL_SH}"  # main() decides the exit with it
}
