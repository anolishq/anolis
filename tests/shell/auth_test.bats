#!/usr/bin/env bats
# Tests for the LAN-bind auth provisioning and the config preflight
# (anolishq/anolis#172), plus the /etc/hosts hostname fix.
#
# Background: install.sh rewrote `bind: 127.0.0.1` to `0.0.0.0` without enabling
# auth, producing a config the runtime is guaranteed to reject at startup. The
# runtime crash-looped while the install reported success. These tests pin the
# guard that makes that unrepresentable.
#
# Source install.sh (main() guarded) and exercise the pure helpers — no root,
# network, or systemd. The live install path is validated on real hardware
# (anolishq/anolis#138).

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)
    TMP="${BATS_TEST_TMPDIR}"
}

# Write a runtime.yaml whose http block carries the given lines.
_cfg() {
    local path="${TMP}/runtime.yaml"
    {
        echo "runtime:"
        echo "  name: test"
        echo "http:"
        echo "  enabled: true"
        printf '  %s\n' "$@"
        echo "providers:"
        echo "  - id: p0"
    } > "${path}"
    printf '%s' "${path}"
}

# ---------------------------------------------------------------------------
# preflight_runtime_config — mirrors the runtime's startup guard
# ---------------------------------------------------------------------------

@test "preflight: refuses a non-loopback bind with auth disabled" {
    cfg=$(_cfg "bind: 0.0.0.0" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"non-loopback but authentication is disabled"* ]]
    [[ "${output}" == *"auth_enabled"* ]]
}

@test "preflight: refuses a routable bind with auth disabled" {
    cfg=$(_cfg "bind: 192.168.1.50" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"192.168.1.50"* ]]
}

@test "preflight: accepts a non-loopback bind when auth is enabled" {
    cfg=$(_cfg "bind: 0.0.0.0" "auth_enabled: true" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "preflight: accepts a non-loopback bind with the explicit insecure override" {
    cfg=$(_cfg "bind: 0.0.0.0" "allow_insecure_bind: true" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "preflight: accepts a loopback bind with auth disabled" {
    cfg=$(_cfg "bind: 127.0.0.1" "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

@test "preflight: an absent bind defaults to loopback and is accepted" {
    cfg=$(_cfg "port: 8080")
    run preflight_runtime_config "${cfg}"
    [ "$status" -eq 0 ]
}

# #530: the preflight reads the http block as the runtime's YAML parser does.
# Each case's verdict is the runtime's own for that http block.
_http_cfg() {
    printf 'runtime:\n  name: test\n%sproviders:\n  - id: p0\n' "$1" > "${TMP}/runtime.yaml"
    printf '%s' "${TMP}/runtime.yaml"
}

# An awk that fails has read nothing: refuse, with a message, whether or not
# the caller runs under errexit. Each mode runs in a new bash, because `run`
# calls a function where errexit does not apply.
_preflight_errexit() {
    # shellcheck disable=SC2016  # expanded by the inner bash
    bash -c 'source "$1"; if [[ "$2" == on ]]; then set -e; else set +e; fi; preflight_runtime_config "$3"' \
        _ "${INSTALL_SH}" "$1" "$2"
}

@test "530: an awk that fails refuses the config, with or without errexit" {
    local cfg mode
    cfg=$(_http_cfg $'http:\n  bind: 0.0.0.0\n')
    mkdir -p "${TMP}/badawk"
    printf '#!/bin/sh\nexit 2\n' > "${TMP}/badawk/awk"
    chmod +x "${TMP}/badawk/awk"
    for mode in on off; do
        PATH="${TMP}/badawk:${PATH}" run _preflight_errexit "${mode}" "${cfg}"
        [ "$status" -ne 0 ] || { echo "accepted, errexit ${mode}"; return 1; }
        [[ "${output}" == *"cannot verify the http settings in ${cfg}: the installer's reader failed (awk)"* ]] \
            || { echo "errexit ${mode}: ${output}"; return 1; }
    done
}

# http must be written in block style: a flow mapping or sequence is refused,
# whatever it holds, so the reader never has to follow flow syntax.
@test "530: a flow-style http cannot be verified: write http: in block style" {
    local f="http in flow style (write http: in block style)"
    _cannot_verify $'http: {bind: 0.0.0.0, port: 8080}\n' "${f}"
    _cannot_verify $'http: {bind: 0.0.0.0, auth_enabled: true}\n' "${f}"
    _cannot_verify $'http: {bind: 0.0.0.0, auth_enabled: true}  # exposed\n' "${f}"
    _cannot_verify $'http: {enabled: off, bind: 0.0.0.0}\n' "${f}"
    _cannot_verify $'http: {\n  bind: 0.0.0.0,\n  auth_enabled: true\n}\n' "${f}"
    _cannot_verify $'http: {\n  port: 8080, # api\n  bind: 0.0.0.0\n}\n' "${f}"
    _cannot_verify $'http: {"bind":"0.0.0.0"}\n' "${f}"
    _cannot_verify $'http: {x: "\\"}", bind: 0.0.0.0}\n' "${f}"
    _cannot_verify $'http: [bind, 0.0.0.0]\n' "${f}"
    # R4-B2: an apostrophe in a plain value, as a quote-tracking reader saw it.
    _cannot_verify $'http: {x: it\'s, y: \'q\', bind: 0.0.0.0, w: z\'}\n' "${f}"
}

@test "530: a quoted loopback bind is accepted" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: "127.0.0.1"\n')"
    [ "$status" -eq 0 ]
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: \'::1\'\n')"
    [ "$status" -eq 0 ]
}

@test "530: allow_insecure_bind: yes is a true the runtime accepts" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: 0.0.0.0\n  allow_insecure_bind: yes\n')"
    [ "$status" -eq 0 ]
}

@test "530: auth_enabled: True is a true the runtime accepts" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: 0.0.0.0\n  auth_enabled: True\n')"
    [ "$status" -eq 0 ]
}

@test "530: every spelling of true yaml-cpp accepts counts, quoted or not" {
    local v
    for v in y Y yes Yes YES true True TRUE on On ON '"yes"' "'on'"; do
        run preflight_runtime_config "$(_http_cfg "http:"$'\n'"  bind: 0.0.0.0"$'\n'"  auth_enabled: ${v}"$'\n')"
        [ "$status" -eq 0 ] || { echo "refused: ${v}"; return 1; }
    done
}

@test "530: a spelling of false still refuses the exposed bind" {
    local v
    for v in n N no No NO false False FALSE off Off OFF; do
        run preflight_runtime_config "$(_http_cfg "http:"$'\n'"  bind: 0.0.0.0"$'\n'"  auth_enabled: ${v}"$'\n')"
        [ "$status" -ne 0 ] || { echo "accepted: ${v}"; return 1; }
        [[ "${output}" == *"is non-loopback"* ]]
    done
}

@test "530: http.enabled: false skips the bind check, as the runtime does" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  enabled: false\n  bind: 0.0.0.0\n')"
    [ "$status" -eq 0 ]
    run preflight_runtime_config "$(_http_cfg $'http:\n  enabled: off\n  bind: 0.0.0.0\n')"
    [ "$status" -eq 0 ]
}

@test "530: a block-style non-loopback bind with no auth is still refused" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: 0.0.0.0\n')"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"http.bind '0.0.0.0' is non-loopback"* ]]
}

@test "530: a value the runtime cannot read as a boolean is refused, naming it" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: 127.0.0.1\n  auth_enabled: tRue\n')"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"http.auth_enabled 'tRue'"*"is not a boolean the runtime accepts"* ]]
}

@test "530: _http_value unquotes, drops comments, and reads only direct children" {
    cfg=$(_http_cfg $'http:\n    bind: \'0.0.0.0\'   # all interfaces\n    tls:\n      port: 1\n    port: 8080 # the API\n    auth_token: "a \\"q\\" b"\n')
    [ "$(_http_value "${cfg}" bind)" = "0.0.0.0" ]
    [ "$(_http_value "${cfg}" port)" = "8080" ]
    [ "$(_http_value "${cfg}" auth_token)" = 'a "q" b' ]
}

@test "530: _http_value reads a CRLF file without the CR" {
    printf 'http:\r\n  port: 9090\r\n' > "${TMP}/crlf.yaml"
    [ "$(_http_value "${TMP}/crlf.yaml" port)" = "9090" ]
}

# Where the reader cannot read the http block as yaml-cpp does, the preflight
# refuses with "cannot verify" rather than guess. Each config here is one the
# runtime refuses or accepts; the reader can tell neither.
_cannot_verify() {
    run preflight_runtime_config "$(_http_cfg "$1")"
    [ "$status" -ne 0 ] || { echo "accepted"; return 1; }
    [[ "${output}" == *"cannot verify the http settings in ${TMP}/runtime.yaml: $2 is not supported by the installer's reader"* ]] \
        || { echo "${output}"; return 1; }
    [[ "${output}" == *"anolis-runtime --check-config"* ]]
}

@test "530: an anchor, alias or tag on http or on a key it reads cannot be verified" {
    _cannot_verify $'http: &h\n  bind: 0.0.0.0\n' "an anchor, alias or tag on http"
    _cannot_verify $'http: !!map\n  bind: 0.0.0.0\n' "an anchor, alias or tag on http"
    _cannot_verify $'http: &h {bind: 0.0.0.0}\n' "an anchor, alias or tag on http"
    _cannot_verify $'h: &h {bind: 0.0.0.0}\nhttp: *h\n' "an anchor, alias or tag on http"
    _cannot_verify $'http:\n  bind: !!str 0.0.0.0\n' "an anchor, alias or tag on http.bind"
    _cannot_verify $'http:\n  bind: &b 127.0.0.1\n' "an anchor, alias or tag on http.bind"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  auth_enabled: !!bool yes\n' "an anchor, alias or tag on http.auth_enabled"
    _cannot_verify $'b: &b {bind: 0.0.0.0}\nhttp:\n  <<: *b\n' "a line in http not written name: value"
}

@test "530: a quoted empty bind is non-loopback, as the runtime reads it" {
    local v
    for v in '""' "''" '' '~'; do
        run preflight_runtime_config "$(_http_cfg "http:"$'\n'"  bind: ${v}"$'\n')"
        [ "$status" -ne 0 ] || { echo "accepted bind: ${v}"; return 1; }
        [[ "${output}" == *"is non-loopback but authentication is disabled"* ]]
    done
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: ""\n  auth_enabled: true\n')"
    [ "$status" -eq 0 ]
}

@test "530: an empty boolean is refused, as the runtime refuses it" {
    run preflight_runtime_config "$(_http_cfg $'http:\n  bind: 127.0.0.1\n  auth_enabled:\n')"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"http.auth_enabled ''"*"is not a boolean the runtime accepts"* ]]
    run preflight_runtime_config "$(_http_cfg $'http:\n  enabled: ""\n')"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"http.enabled ''"* ]]
}

@test "530: other forms the reader does not follow cannot be verified" {
    _cannot_verify $'http:\n  {bind: 0.0.0.0}\n' "a line in http not written name: value"
    _cannot_verify $'http:\n  bind: 127.0.0.1\nhttp:\n  auth_enabled: true\n' "a second top-level http"
    _cannot_verify $'http:\n  bind: >-\n    0.0.0.0\n' "a block scalar for http.bind"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n    0.0.0.0\n' "a value of http.bind on the next line"
    _cannot_verify $'http:\n  auth_token: "abc\n  bind: 0.0.0.0"\n' "a quoted value or a collection over several lines"
    _cannot_verify $'http:\n  bind: "127.0.0\\x2e1"\n' "an escape sequence in http.bind"
    _cannot_verify $'http:\n  port: 0x1F91\n' "http.port written as other than a plain decimal number"
    _cannot_verify $'http:\n  port: 010\n' "http.port written as other than a plain decimal number"
    _cannot_verify $'http:\n' "an empty http"
    printf '{"http": {"bind": "0.0.0.0"}}\n' > "${TMP}/runtime.yaml"
    run preflight_runtime_config "${TMP}/runtime.yaml"
    [[ "${output}" == *"a quoted mapping key is not supported"* ]]
    printf '{http: {bind: 0.0.0.0}}\n' > "${TMP}/runtime.yaml"
    run preflight_runtime_config "${TMP}/runtime.yaml"
    [[ "${output}" == *"a top-level line not written name: value is not supported"* ]]
    printf '\357\273\277http:\n  bind: 0.0.0.0\n' > "${TMP}/runtime.yaml"
    run preflight_runtime_config "${TMP}/runtime.yaml"
    [[ "${output}" == *"a byte-order mark is not supported"* ]]
}

@test "530: one leading --- is read; any other document marker cannot be verified" {
    # A leading --- starts the first document; it does not end one.
    printf -- '---\nhttp:\n  bind: 0.0.0.0\n' > "${TMP}/runtime.yaml"
    run preflight_runtime_config "${TMP}/runtime.yaml"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"is non-loopback"* ]]
    _cannot_verify $'http:\n  bind: 127.0.0.1\n---\nhttp:\n  bind: 0.0.0.0\n' "a second document marker"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n...\n' "a document end marker"
}

@test "530: the reader takes the '' escape and the first of duplicate keys" {
    cfg=$(_http_cfg $'http:\n  bind: 0.0.0.0\n  port: 9090\n  auth_token: \'it\'\'s\'\n  auth_enabled: false\n  auth_enabled: true\n')
    [ "$(_http_value "${cfg}" bind)" = "0.0.0.0" ]
    [ "$(_http_value "${cfg}" port)" = "9090" ]
    [ "$(_http_value "${cfg}" auth_token)" = "it's" ]
    # yaml-cpp keeps the first: auth is off, so the exposed bind is refused.
    run preflight_runtime_config "${cfg}"
    [ "$status" -ne 0 ]
    [[ "${output}" == *"is non-loopback"* ]]
}

@test "530: a quoted or escaped key name cannot be verified, at top level or in http" {
    _cannot_verify $'http:\n  "bind": 0.0.0.0\n' "a quoted mapping key"
    _cannot_verify $'http:\n  \'bind\': 0.0.0.0\n' "a quoted mapping key"
    _cannot_verify $'http:\n  "bin\\x64": 0.0.0.0\n' "a quoted mapping key"
    _cannot_verify $'"htt\\x70":\n  bind: 0.0.0.0\n' "a quoted mapping key"
}

# Constructs that would hide http from the reader, or hide lines inside a
# value from yaml-cpp, are outside the subset (#530).
_cannot_verify_file() {
    # shellcheck disable=SC2059  # the case is the format, for its escapes
    printf -- "$1" > "${TMP}/runtime.yaml"
    run preflight_runtime_config "${TMP}/runtime.yaml"
    [ "$status" -ne 0 ] || { echo "accepted"; return 1; }
    [[ "${output}" == *"cannot verify the http settings in ${TMP}/runtime.yaml: $2 is not supported"* ]] \
        || { echo "${output}"; return 1; }
}

@test "530: a directive, a --- with content, an indented root, CR endings, ? keys cannot be verified" {
    _cannot_verify_file '%%YAML 1.2\n---\nhttp:\n  bind: 0.0.0.0\n' "a directive"
    _cannot_verify_file '%%TAG ! tag:example.com,2000:\n---\nhttp:\n  bind: 0.0.0.0\n' "a directive"
    _cannot_verify_file '--- {http: {bind: 0.0.0.0}}\n' "content on the --- line"
    _cannot_verify_file ' runtime:\n   name: t\n http:\n   bind: 0.0.0.0\n' "an indented first line"
    _cannot_verify_file '  http:\n    bind: 0.0.0.0\n' "an indented first line"
    _cannot_verify_file 'runtime:\r  name: t\rhttp:\r  bind: 0.0.0.0\r' "a carriage return inside a line (CR line endings)"
    _cannot_verify_file '? http\n: {bind: 0.0.0.0}\n' "an explicit key (?)"
    _cannot_verify_file '' "a file with no top-level key"
    _cannot_verify_file '# only a comment\n' "a file with no top-level key"
    _cannot_verify_file 'http:\n  - bind: 0.0.0.0\n' "a line in http not written name: value"
}

@test "530: a quote or bracket left open on any line of the file cannot be verified" {
    # Each would make yaml-cpp read the auth line as part of a value.
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    note: "abc\n  auth_enabled: true\n    "\n' \
        "a quoted value or a collection over several lines"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    note: \'abc\n  allow_insecure_bind: true\n    \'\n' \
        "a quoted value or a collection over several lines"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  cors_allowed_origins: [a,\n  auth_enabled: true\n]\n' \
        "a quoted value or a collection over several lines"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  x: {a: 1,\n  auth_enabled: true\n}\n' \
        "a quoted value or a collection over several lines"
    # \" does not close a double-quoted value, so this one runs on.
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: "a\\"\n  auth_enabled: true"\n' \
        "a quoted value or a collection over several lines"
    # A quote that is part of a plain value opens nothing, and an escaped quote
    # does not close one.
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  note: don\'t panic\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  note: "say \\"hi\\""\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
}

@test "530: a quote after a tag, an anchor or a ? key is still tracked" {
    # Each opens a quote that runs on and hides the auth line from yaml-cpp.
    local q="a quoted value or a collection over several lines"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: !!str "abc\n  auth_enabled: true\n  zzz: end"\n' "${q}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: !x "abc\n  auth_enabled: true\n  zzz: end"\n' "${q}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: &a "abc\n  auth_enabled: true\n  zzz: end"\n' "${q}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: !!str \'abc\n  allow_insecure_bind: true\n  zzz: end\'\n' "${q}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    note: !!str "abc\n  auth_enabled: true\n  zzz: end"\n' "${q}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  list:\n    - !!str "abc\n  auth_enabled: true\n  zzz: end"\n' "${q}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    ? "abc\n  auth_enabled: true\n  zzz: end"\n    : v\n' "${q}"
}

@test "530: shapes yaml-cpp refuses outright cannot be verified" {
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  note: a\xc2\x85b\n' "a NEL, line separator or paragraph separator character"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  note: a\xe2\x80\xa8b\n' "a NEL, line separator or paragraph separator character"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  note: a\xe2\x80\xa9b\n' "a NEL, line separator or paragraph separator character"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  tls:\n    "a":"b\n' "a quoted mapping key"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  note: ? x\n' "an explicit key (?)"
    # A plain value may hold ! and & past its start.
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  note: a!b&c\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
}

@test "530: an alias, or a block scalar yaml-cpp cannot end, cannot be verified" {
    # Each is a file yaml-cpp refuses to load at all.
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: *a "abc\n  auth_enabled: true\n  q: w"\n' "an alias (*)"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  list:\n    - *a\n' "an alias (*)"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: |\n      deep\n    "less\n  auth_enabled: true\n' \
        "a block scalar line less indented than its first"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  tls:\n    note: |\n        deep\n      less\n' \
        "a block scalar line less indented than its first"
    _cannot_verify $'runtime:\n  note: >\n      deep\n    less\nhttp:\n  bind: 127.0.0.1\n' \
        "a block scalar line less indented than its first"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: |4\n      ok\n    "x\n  auth_enabled: true\n' \
        "a block scalar with an indentation indicator"
    _cannot_verify $'runtime:\n  note: >-2\n    x\nhttp:\n  bind: 127.0.0.1\n' \
        "a block scalar with an indentation indicator"
    # An asterisk past the start of a plain value, a block scalar whose lines
    # hold or deepen their indentation, and a chomping indicator all stage.
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  note: a*b\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  note: |-\n    a\n      b\n    c\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
}

@test "530: a tab in the indentation cannot be verified" {
    _cannot_verify $'http:\n\tbind: 0.0.0.0\n' "a tab in the indentation"
}

@test "530: a tag or anchor written with a quote or flow character cannot be verified" {
    local t="a tag or anchor written with a quote or flow character"
    # R4-B1: the quote right after the tag opens a string that hides the auth line.
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: !!str"abc\n  auth_enabled: true\n  zzz: end"\n' "${t}"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  note: &a\'abc\n  auth_enabled: true\n  zzz: end\'\n' "${t}"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  note: !x[1] v\n' "${t}"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  note: &a,b v\n' "${t}"
}

@test "530: a quoted mapping key anywhere in the file cannot be verified" {
    local k="a quoted mapping key"
    _cannot_verify $'http:\n  bind: 127.0.0.1\n  tls:\n    \'a\':\'b\'\n' "${k}"
    _cannot_verify $'runtime:\n  "q": "v"\nhttp:\n  bind: 127.0.0.1\n' "${k}"
    _cannot_verify $'http:\n  bind: 127.0.0.1\nproviders:\n  - "id" : p1\n' "${k}"
}

# A quoted key whose text holds ": |" or ": >" is no block-scalar header: the
# quote opened under it must still be checked (corpus5).
@test "530: a quoted key that looks like a block-scalar header does not hide the quote under it" {
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    "a: |b":\n      "zzz\n  auth_enabled: true\n  q: w"\n' "a quoted mapping key"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    \'a: |b\':\n      "zzz\n  auth_enabled: true\n  q: w"\n' "a quoted mapping key"
    _cannot_verify $'http:\n  bind: 0.0.0.0\n  tls:\n    "a: >b":\n      "zzz\n  allow_insecure_bind: true\n  q: w"\n' "a quoted mapping key"
    # Only a plain name (letters, digits, _ and -) starts a block scalar the
    # reader passes over; under any other key its lines are checked like any.
    _cannot_verify $'runtime:\n  a.b: |\n    "x\nhttp:\n  bind: 127.0.0.1\n' "a quoted value or a collection over several lines"
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  a_b-c: |\n    "x\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
}

@test "530: a watched value with anything after its closing quote cannot be verified" {
    _cannot_verify $'http:\n  bind: "127.0.0.1" 0.0.0.0\n' "http.bind not quoted on one line, alone"
}

@test "530: block scalars outside the watched keys are read past" {
    run preflight_runtime_config "$(_http_cfg $'runtime:\n  note: |\n    it\'s "fine\n    [ok\nhttp:\n  bind: 127.0.0.1\n')"
    [ "$status" -eq 0 ]
}

@test "530: callers take the reader's value as it is, without a second unquote" {
    PREFIX="${TMP}/prefix"
    mkdir -p "${PREFIX}/config"
    printf 'http:\n  auth_token: "'"'"'quoted'"'"'"\n  bind: "'"'"'h'"'"'"\n' > "${PREFIX}/config/runtime.yaml"
    [ "$(_runtime_api_token)" = "'quoted'" ]
    [ "$(_runtime_probe_host)" = "'h'" ]
    printf 'runtime:\n  data_dir: "it'"'"'s"\n' > "${TMP}/dd.yaml"
    [ "$(_yaml_block_value "${TMP}/dd.yaml" runtime data_dir)" = "it's" ]
    # The pin resolves the relative dir the runtime names, apostrophe and all.
    _pin_runtime_data_dir "${TMP}/dd.yaml" new >/dev/null
    grep -qxF "  data_dir: ${TMP}/it's" "${TMP}/dd.yaml"
}

# ---------------------------------------------------------------------------
# _http_value — scoped to the top-level http: block
# ---------------------------------------------------------------------------

@test "_http_value: reads a key from the http block" {
    cfg=$(_cfg "bind: 0.0.0.0" "port: 9090" "auth_enabled: true")
    [ "$(_http_value "${cfg}" bind)" = "0.0.0.0" ]
    [ "$(_http_value "${cfg}" port)" = "9090" ]
    [ "$(_http_value "${cfg}" auth_enabled)" = "true" ]
}

@test "_http_value: is empty for an absent key" {
    cfg=$(_cfg "bind: 0.0.0.0")
    [ -z "$(_http_value "${cfg}" auth_enabled)" ]
}

@test "_http_value: does not read a same-named key from another block" {
    # A `bind:` under telemetry must not be mistaken for http.bind — otherwise the
    # preflight could clear a config that actually exposes an unauthenticated API.
    cat > "${TMP}/other.yaml" <<'YAML'
http:
  enabled: true
  bind: 0.0.0.0
telemetry:
  enabled: true
  auth_enabled: true
YAML
    [ "$(_http_value "${TMP}/other.yaml" bind)" = "0.0.0.0" ]
    [ -z "$(_http_value "${TMP}/other.yaml" auth_enabled)" ]

    run preflight_runtime_config "${TMP}/other.yaml"
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# API token
# ---------------------------------------------------------------------------

@test "_generate_api_token: emits 64 hex chars" {
    run _generate_api_token
    [ "$status" -eq 0 ]
    [[ "${output}" =~ ^[0-9a-f]{64}$ ]]
}

@test "_generate_api_token: successive tokens differ" {
    a=$(_generate_api_token)
    b=$(_generate_api_token)
    [ "${a}" != "${b}" ]
}

@test "unit: injects the API token via EnvironmentFile, not the config" {
    PREFIX=/opt/anolis
    run emit_systemd_unit
    [ "$status" -eq 0 ]
    [[ "${output}" == *"EnvironmentFile=/etc/anolis/runtime.env"* ]]
    # The secret itself must never be inlined into the unit.
    [[ "${output}" != *"ANOLIS_API_TOKEN="* ]]
}

# ---------------------------------------------------------------------------
# /etc/hosts — a renamed host must still resolve its own name
# ---------------------------------------------------------------------------

@test "hosts: rewrites the existing 127.0.1.1 entry" {
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n127.0.1.1\traspberrypi\n' > "${ETC_HOSTS}"

    _update_etc_hosts "anolis-f95d50b1"

    grep -qE '^127\.0\.1\.1[[:space:]]+anolis-f95d50b1$' "${ETC_HOSTS}"
    ! grep -q 'raspberrypi' "${ETC_HOSTS}"
    # The loopback entry is untouched.
    grep -qE '^127\.0\.0\.1[[:space:]]+localhost$' "${ETC_HOSTS}"
}

@test "hosts: appends when no 127.0.1.1 entry exists" {
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n' > "${ETC_HOSTS}"

    _update_etc_hosts "anolis-f95d50b1"

    grep -qE '^127\.0\.1\.1[[:space:]]+anolis-f95d50b1$' "${ETC_HOSTS}"
    grep -qE '^127\.0\.0\.1[[:space:]]+localhost$' "${ETC_HOSTS}"
}

@test "hosts: is idempotent across re-installs" {
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n127.0.1.1\traspberrypi\n' > "${ETC_HOSTS}"

    _update_etc_hosts "anolis-f95d50b1"
    _update_etc_hosts "anolis-f95d50b1"

    [ "$(grep -cE '^127\.0\.1\.1' "${ETC_HOSTS}")" -eq 1 ]
}

@test "hosts: repaired even when the hostname is ALREADY correct" {
    # Regression: phase_hostname returned early when the hostname already matched,
    # so /etc/hosts was never repaired. That is the exact state of every device
    # renamed by an install.sh predating the fix — hostname right, hosts entry
    # missing, and the box unable to resolve its own name. Found on the bench.
    export ETC_HOSTS="${TMP}/hosts"
    printf '127.0.0.1\tlocalhost\n127.0.1.1\traspberrypi\n' > "${ETC_HOSTS}"

    ARCH=arm64
    # Stand in for the real hostname/serial lookups.
    hostname() { echo "anolis-f95d50b1"; }
    serial_file_stub="${TMP}/serial"
    printf '00000000f95d50b1' > "${serial_file_stub}"

    # Exercise the already-correct branch directly: it must still repair hosts.
    run bash -c "
        ANOLIS_INSTALL_SH_NO_MAIN=1 source '${INSTALL_SH}'
        ETC_HOSTS='${ETC_HOSTS}'
        _update_etc_hosts 'anolis-f95d50b1'
    "
    [ "$status" -eq 0 ]
    grep -qE '^127\.0\.1\.1[[:space:]]+anolis-f95d50b1$' "${ETC_HOSTS}"
    ! grep -q 'raspberrypi' "${ETC_HOSTS}"
}
