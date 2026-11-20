#!/usr/bin/env bats
# Tests for the health check's time bounds (#745).
#
# A socket that accepts a connection and never answers (a wedged runtime, or
# another process on the port) used to block the first health poll forever:
# _runtime_curl set no timeout, and HEALTH_TIMEOUT counted only the sleeps
# between polls. The first test points the real curl at such a socket; the
# second stubs a poll that hangs for its whole budget and counts the polls.

bats_require_minimum_version 1.5.0

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u
}

teardown() {
    [[ -n "${LISTENER_PID:-}" ]] && kill "${LISTENER_PID}" 2>/dev/null || true
}

# A loopback listener that completes the handshake and never accepts, so a
# request is sent and never answered.
start_silent_listener() {
    LISTENER_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
    python3 -c "
import socket, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', ${LISTENER_PORT}))
s.listen(16)
time.sleep(60)
" &
    LISTENER_PID=$!
    for _ in $(seq 50); do
        python3 -c "import socket; socket.create_connection(('127.0.0.1', ${LISTENER_PORT}), 0.1).close()" 2>/dev/null && return
        command sleep 0.1
    done
    return 1
}

@test "745: _runtime_curl gives up on a port that accepts and never answers" {
    start_silent_listener
    _runtime_api_token() { :; }

    local start=${SECONDS}
    run _runtime_curl -fsS "http://127.0.0.1:${LISTENER_PORT}/v0/runtime/status"
    local took=$((SECONDS - start))

    [ "${status}" -ne 0 ]
    [ "${took}" -le $((RUNTIME_CURL_MAX_TIME + 2)) ]
}

@test "745: a poll that hangs counts its own time against HEALTH_TIMEOUT" {
    _runtime_probe_host() { echo 127.0.0.1; }
    _runtime_port() { echo 8080; }
    sleep() { :; }
    POLLS=0
    # Each poll hangs for 5 s, then fails as a curl timeout does (exit 28).
    # Advancing SECONDS stands in for the wait, so the test takes no time.
    _runtime_curl() {
        POLLS=$((POLLS + 1))
        SECONDS=$((SECONDS + 5))
        return 28
    }
    NO_START=0
    HEALTH_FAILED=0

    phase_health >/dev/null 2>&1

    [ "${HEALTH_FAILED}" -eq 1 ]
    # Each failed poll costs its 5 s plus the interval, so the polls fit in
    # HEALTH_TIMEOUT, rather than HEALTH_TIMEOUT / HEALTH_INTERVAL of them.
    local per_poll=$((5 + HEALTH_INTERVAL))
    [ "${POLLS}" -le $(((HEALTH_TIMEOUT + per_poll - 1) / per_poll)) ]
}
