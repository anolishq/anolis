#!/usr/bin/env bats
# Unit tests for do_stage() in tools/install.sh — build an offline bundle from a
# local config. These source install.sh (main() is guarded) and call do_stage
# with _stage_fetch mocked, so no network/root is needed. The fixture project
# declares providers "foo"/"bar" (not the real bread/ezo) to prove --stage is
# generic over whatever the machine-profile declares — nothing is hardcoded.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"
PROJECT="${BATS_TEST_DIRNAME}/fixtures/stage-project"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u  # errexit+pipefail stay on so a non-final assertion fails the test (anolishq/anolis#207)

    STAGE_DIR=""
    PROJECT_DIR="${PROJECT}"
    TARGET_ARCH="x86_64"
    PREFIX="/opt/anolis"
    GITHUB_TOKEN=""
    OUT="$(mktemp -d)"

    # Mock the release download: fabricate tarballs with the expected bin/ layout.
    _stage_fetch() {
        local asset="$3" dir="$4" t
        case "${asset}" in
            install.sh)
                printf '#!/bin/bash\n' > "${dir}/install.sh" ;;
            anolis-provider-*-linux-*.tar.gz)
                local name
                name=$(printf '%s' "${asset}" | sed -E 's/^anolis-provider-([a-z0-9-]+)-.*/\1/')
                t=$(mktemp -d); mkdir -p "${t}/bin"; printf 'bin' > "${t}/bin/anolis-provider-${name}"
                tar -czf "${dir}/${asset}" -C "${t}" bin; rm -rf "${t}" ;;
            anolis-*-linux-*.tar.gz)
                t=$(mktemp -d); mkdir -p "${t}/bin"; printf 'bin' > "${t}/bin/anolis-runtime"
                tar -czf "${dir}/${asset}" -C "${t}" bin; rm -rf "${t}" ;;
        esac
    }
}

teardown() {
    rm -rf "${OUT:-}" "${STAGE_TMP:-}"
}

@test "--stage requires --project" {
    STAGE_DIR="${OUT}"; PROJECT_DIR=""
    run do_stage
    [ "$status" -ne 0 ]
    [[ "$output" == *"--project"* ]]
}

@test "--stage fails on missing machine-profile.yaml" {
    STAGE_DIR="${OUT}"; PROJECT_DIR="$(mktemp -d)"
    run do_stage
    [ "$status" -ne 0 ]
    [[ "$output" == *"machine-profile.yaml"* ]]
}

@test "--stage builds a bundle generic over the profile's providers (foo/bar)" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]

    local bundle="${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz"
    [ -f "${bundle}" ]

    local x; x="$(mktemp -d)"
    tar -xzf "${bundle}" -C "${x}" --strip-components=1

    # Binaries for BOTH declared providers — proves generic, no hardcoding.
    [ -f "${x}/bin/anolis-runtime" ]
    [ -f "${x}/bin/anolis-provider-foo" ]
    [ -f "${x}/bin/anolis-provider-bar" ]

    # Bundle layout install.sh --local consumes.
    [ -f "${x}/config/runtime.yaml" ]
    # Each provider config ships once, under projects/, where the rendered
    # runtime config points; config/providers/ was never read (#515).
    [ ! -e "${x}/config/providers" ]
    [ -f "${x}/projects/stage-project/config/provider-foo.yaml" ]
    [ -f "${x}/projects/stage-project/config/provider-bar.yaml" ]
    [ -f "${x}/systemd/anolis-runtime.service" ]
    [ -f "${x}/manifest.json" ]
    [ -f "${x}/checksums.sha256" ]
    [ -f "${x}/install.sh" ]

    # Render: dev-relative → production absolute + bind rewrite.
    grep -q "/opt/anolis/bin/anolis-provider-foo" "${x}/config/runtime.yaml"
    grep -q "bind: 0.0.0.0" "${x}/config/runtime.yaml"

    # Manifest carries both providers.
    grep -q '"foo"' "${x}/manifest.json"
    grep -q '"bar"' "${x}/manifest.json"

    # Checksums verify.
    ( cd "${x}" && sha256sum -c checksums.sha256 >/dev/null )

    rm -rf "${x}"
}

# Copy the fixture project into a writable temp dir so a test can mutate it.
# Keeps the "stage-project" basename so the produced bundle name is stable
# (the profile is derived from the project dir's basename).
_copy_fixture() {
    local d; d="$(mktemp -d)/stage-project"
    cp -r "${PROJECT}" "${d}"
    printf '%s' "${d}"
}

# --- #223: consume components.optional.telemetry_export pin -------------------

@test "#223 stage bakes the profile telemetry_export pin into manifest.json" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]
    local x; x="$(mktemp -d)"
    tar -xzf "${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz" -C "${x}" --strip-components=1
    grep -q '"telemetry_export"' "${x}/manifest.json"
    grep -q '"0.9.9"' "${x}/manifest.json"
    ( cd "${x}" && sha256sum -c checksums.sha256 >/dev/null )
    rm -rf "${x}"
}

@test "#223 profile pin beats an explicit env override at assemble time" {
    export TELEMETRY_EXPORT_VERSION=8.8.8
    TELEMETRY_EXPORT_VERSION_ENV=8.8.8
    # Call assemble_bundle directly (not via run) so the global is observable.
    assemble_bundle "${OUT}/b" x86_64
    [ "${ASSEMBLED_TELEMETRY_EXPORT_PIN}" = "0.9.9" ]
    grep -q '"0.9.9"' "${OUT}/b/manifest.json"
    ! grep -q '"8.8.8"' "${OUT}/b/manifest.json"
}

@test "#223 no optional pin → no telemetry_export block in manifest" {
    PROJECT_DIR="$(_copy_fixture)"
    # Drop the whole optional: block.
    sed -i '/^  optional:/,/version: "0.9.9"/d' "${PROJECT_DIR}/machine-profile.yaml"
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]
    local x; x="$(mktemp -d)"
    tar -xzf "${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz" -C "${x}" --strip-components=1
    ! grep -q '"telemetry_export"' "${x}/manifest.json"
    rm -rf "${x}" "${PROJECT_DIR}"
}

# --- #222: record the staged prefix in the manifest --------------------------

@test "#222 manifest records the staged prefix (default)" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]
    local x; x="$(mktemp -d)"
    tar -xzf "${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz" -C "${x}" --strip-components=1
    grep -q '"prefix": "/opt/anolis"' "${x}/manifest.json"
    rm -rf "${x}"
}

@test "#222 manifest + render reflect a custom staged prefix" {
    PREFIX="/usr/local/anolis"
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]
    local x; x="$(mktemp -d)"
    tar -xzf "${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz" -C "${x}" --strip-components=1
    grep -q '"prefix": "/usr/local/anolis"' "${x}/manifest.json"
    grep -q "/usr/local/anolis/bin/anolis-provider-foo" "${x}/config/runtime.yaml"
    rm -rf "${x}"
}

# --- #226: cross-validate configured providers against pinned ----------------

@test "#226 a config referencing an unpinned provider fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    # Point bar's command at an unpinned provider 'baz'.
    sed -i 's#anolis-provider-bar#anolis-provider-baz#g' \
        "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -ne 0 ]
    [[ "$output" == *"baz"* ]]
    [[ "$output" == *"components.providers"* ]]
    [ ! -f "${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz" ]
    rm -rf "${PROJECT_DIR}"
}

@test "#226 an unpinned reference in a non-manual variant also fails" {
    PROJECT_DIR="$(_copy_fixture)"
    sed -i 's#anolis-provider-bar#anolis-provider-baz#g' \
        "${PROJECT_DIR}/config/anolis-runtime.telemetry.yaml"
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -ne 0 ]
    [[ "$output" == *"baz"* ]]
    rm -rf "${PROJECT_DIR}"
}

@test "#226 a non-provider-shaped command fails closed" {
    PROJECT_DIR="$(_copy_fixture)"
    # Replace bar's command with something the rewrite never maps to a bundled bin.
    sed -i 's#command: .*anolis-provider-bar#command: /usr/bin/env#' \
        "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -ne 0 ]
    rm -rf "${PROJECT_DIR}"
}

@test "#226 a pinned-but-unreferenced provider warns, not fails" {
    PROJECT_DIR="$(_copy_fixture)"
    # Add a third pinned provider 'qux' under components.providers that no config
    # references (write a fresh profile so indentation is unambiguous).
    cat > "${PROJECT_DIR}/machine-profile.yaml" <<'YAML'
runtime_profiles:
  manual: config/anolis-runtime.manual.yaml
  telemetry: config/anolis-runtime.telemetry.yaml
components:
  runtime:
    repo: anolishq/anolis
    version: "9.9.9"
  providers:
    foo:
      repo: anolishq/anolis-provider-foo
      version: "1.0.0"
    bar:
      repo: anolishq/anolis-provider-bar
      version: "2.0.0"
    qux:
      repo: anolishq/anolis-provider-qux
      version: "3.0.0"
YAML
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]
    [[ "$output" == *"qux"* ]]
    rm -rf "${PROJECT_DIR}"
}

# --- #268: the sidecar must be consumable by sha256sum -c ---------------------

@test "#268 the .sha256 sidecar verifies with sha256sum -c" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]

    local bundle="anolis-stage-project-9.9.9-x86_64.tar.gz"
    [ -f "${OUT}/${bundle}.sha256" ]

    # The air-gap operator's first command, run from the directory they carried
    # both files into. A bare digest fails here with "no properly formatted
    # checksum lines found".
    ( cd "${OUT}" && sha256sum -c "${bundle}.sha256" >/dev/null )
}

@test "#268 the sidecar names the bundle relatively, not by staging path" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]

    local bundle="anolis-stage-project-9.9.9-x86_64.tar.gz"
    # An absolute staging path would not exist on the target machine, so
    # sha256sum -c would look for the wrong file after the USB handoff. The
    # two-space separator is what rules a path out: `<digest>  /abs/…/x.tar.gz`
    # does not end in two spaces followed by the bare name.
    [[ "$(cat "${OUT}/${bundle}.sha256")" == *"  ${bundle}" ]]
}

@test "#268 --stage tells the operator how to verify" {
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -eq 0 ]
    [[ "$output" == *"sha256sum -c"* ]]
}

# --- #529: a literal http.auth_token never reaches a bundle -------------------

TOKEN="shared-literal-token-123"

# Give the fixture's <file> an http block that sets auth_token to <value>.
_set_token() {
    local file="$1" value="$2"
    printf 'http:\n  bind: 127.0.0.1\n  auth_enabled: true\n  auth_token: %s\n' "${value}" >> "${file}"
}

# Staging must fail naming <file>, point at the per-device token, never print
# the token, and leave no file holding it, in the output dir or the staging dir.
_assert_token_refused() {
    local file="$1"
    STAGE_DIR="${OUT}"
    run assemble_bundle "${OUT}/staging" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"${file}"* ]]
    [[ "$output" == *"http.auth_token"* ]]
    [[ "$output" == *"ANOLIS_API_TOKEN"* ]]
    [[ "$output" == *"runtime.env"* ]]
    [[ "$output" != *"${TOKEN}"* ]]
    ! grep -rqF -- "${TOKEN}" "${OUT}"
}

@test "#529 a literal auth_token in the manual runtime config fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    _set_token "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "\"${TOKEN}\""
    _assert_token_refused anolis-runtime.manual.yaml
    rm -rf "${PROJECT_DIR}"
}

@test "#529 a literal auth_token in another variant fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    _set_token "${PROJECT_DIR}/config/anolis-runtime.telemetry.yaml" "${TOKEN}"
    _assert_token_refused anolis-runtime.telemetry.yaml
    rm -rf "${PROJECT_DIR}"
}

@test "#529 a literal auth_token in a provider config fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    _set_token "${PROJECT_DIR}/config/provider-foo.yaml" "'${TOKEN}'"
    _assert_token_refused provider-foo.yaml
    rm -rf "${PROJECT_DIR}"
}

@test "#529 do_stage writes no bundle when a config carries a token" {
    PROJECT_DIR="$(_copy_fixture)"
    _set_token "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "\"${TOKEN}\""
    STAGE_DIR="${OUT}"
    run do_stage
    [ "$status" -ne 0 ]
    [ ! -e "${OUT}/anolis-stage-project-9.9.9-x86_64.tar.gz" ]
    ! grep -rqF -- "${TOKEN}" "${OUT}"
    rm -rf "${PROJECT_DIR}"
}

@test "#529 a config that does not parse fails staging, without quoting it" {
    PROJECT_DIR="$(_copy_fixture)"
    # An unclosed quote: the parser's own message quotes this line in full.
    printf 'http:\n  auth_token: "%s\n' "${TOKEN}" >> "${PROJECT_DIR}/config/provider-bar.yaml"
    run assemble_bundle "${OUT}/staging" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"provider-bar.yaml: not valid YAML"* ]]
    [[ "$output" != *"${TOKEN}"* ]]
    ! grep -rqF -- "${TOKEN}" "${OUT}"
    rm -rf "${PROJECT_DIR}"
}

@test "#529 a quoted empty auth_token is not a token: staging succeeds" {
    local v
    for v in '""' "''"; do
        PROJECT_DIR="$(_copy_fixture)"
        _set_token "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "${v}"
        run assemble_bundle "${OUT}/staging-${#v}" x86_64
        [ "$status" -eq 0 ] || { echo "refused: ${v}"; return 1; }
        rm -rf "${PROJECT_DIR}"
    done
}

# yaml-cpp, which the runtime parses with, reads a null value as the string
# "null": the runtime would use "null" as the token, not ANOLIS_API_TOKEN.
@test "#529 a null auth_token is a token: staging fails" {
    local v
    for v in '' '~' 'null' 'Null' '!!null'; do
        PROJECT_DIR="$(_copy_fixture)"
        _set_token "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "${v}"
        run assemble_bundle "${OUT}/staging" x86_64
        [ "$status" -ne 0 ] || { echo "staged: [${v}]"; return 1; }
        [[ "$output" == *"anolis-runtime.manual.yaml: sets http.auth_token"* ]]
        rm -rf "${PROJECT_DIR}" "${OUT}/staging"
    done
}

# Where PyYAML's loaded data and yaml-cpp disagree, the check goes by what
# yaml-cpp reads (tests/shell can't run yaml-cpp; each case names its reading).
@test "#529 a token PyYAML would overwrite with a later empty duplicate key fails staging" {
    # yaml-cpp reads the first auth_token; PyYAML's load keeps the last.
    PROJECT_DIR="$(_copy_fixture)"
    printf 'http:\n  auth_token: %s\n  auth_token: ""\n' "${TOKEN}" >> "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    _assert_token_refused anolis-runtime.manual.yaml
}

@test "#529 a token in the first of two http blocks fails staging" {
    # yaml-cpp reads the first http block; PyYAML's load keeps the last.
    PROJECT_DIR="$(_copy_fixture)"
    printf 'http:\n  auth_token: %s\nhttp:\n  port: 8080\n' "${TOKEN}" >> "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    _assert_token_refused anolis-runtime.manual.yaml
}

@test "#529 a token with an explicit !!null tag fails staging" {
    # PyYAML loads it as None; yaml-cpp keeps the scalar's text.
    PROJECT_DIR="$(_copy_fixture)"
    _set_token "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "!!null ${TOKEN}"
    _assert_token_refused anolis-runtime.manual.yaml
}

@test "#529 a token brought into http by a merge key fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    printf 'base: &b {auth_token: %s}\nhttp:\n  <<: *b\n' "${TOKEN}" >> "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    _assert_token_refused anolis-runtime.manual.yaml
}

@test "#529 a token in machine-profile.yaml fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    _set_token "${PROJECT_DIR}/machine-profile.yaml" "${TOKEN}"
    _assert_token_refused machine-profile.yaml
}

# The bundle's config/runtime.yaml is rendered from whatever file
# runtime_profiles.manual names, which need not match anolis-runtime.*.yaml.
@test "#529 a token in a manual variant outside anolis-runtime.*.yaml fails staging" {
    local name
    for name in manual-runtime.yaml anolis-runtime.manual.yml; do
        PROJECT_DIR="$(_copy_fixture)"
        cp "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "${PROJECT_DIR}/config/${name}"
        _set_token "${PROJECT_DIR}/config/${name}" "${TOKEN}"
        sed -i "s#manual: config/anolis-runtime.manual.yaml#manual: config/${name}#" "${PROJECT_DIR}/machine-profile.yaml"
        _assert_token_refused "${name}"
        rm -rf "${PROJECT_DIR}" "${OUT}/staging"
    done
}

@test "#529 a manual variant outside anolis-runtime.*.yaml that does not parse fails without quoting it" {
    PROJECT_DIR="$(_copy_fixture)"
    cp "${PROJECT_DIR}/config/anolis-runtime.manual.yaml" "${PROJECT_DIR}/config/manual-runtime.yaml"
    printf 'http:\n  auth_token: "%s\n' "${TOKEN}" >> "${PROJECT_DIR}/config/manual-runtime.yaml"
    sed -i "s#manual: config/anolis-runtime.manual.yaml#manual: config/manual-runtime.yaml#" "${PROJECT_DIR}/machine-profile.yaml"
    run assemble_bundle "${OUT}/staging" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"manual-runtime.yaml: not valid YAML"* ]]
    [[ "$output" != *"${TOKEN}"* ]]
}

@test "#529 a config that is not UTF-8 fails staging cleanly, without quoting it" {
    PROJECT_DIR="$(_copy_fixture)"
    printf 'http:\n  auth_token: \xff%s\n' "${TOKEN}" >> "${PROJECT_DIR}/config/provider-foo.yaml"
    run assemble_bundle "${OUT}/staging" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"provider-foo.yaml: not valid UTF-8"* ]]
    [[ "$output" != *"Traceback"* ]]
    [[ "$output" != *"${TOKEN}"* ]]
}

# --- #694: a project copied under a new name installs under that name ---------

# Copy the fixture project into a temp dir named <name>. Its paths still name
# stage-project, as a copied project's do.
_copy_fixture_as() {
    local d; d="$(mktemp -d)/$1"
    cp -r "${PROJECT}" "${d}"
    printf '%s' "${d}"
}

# Every --config path in the rendered runtime configs, one per line.
_config_paths() {
    grep -ho -- '"--config", "[^"]*"' "$@" | sed 's/^"--config", "//; s/"$//' | sort -u
}

@test "#694 a project copied under a new name: provider paths point at its own directory" {
    PROJECT_DIR="$(_copy_fixture_as my-reactor)"
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -eq 0 ]
    local paths p
    paths=$(_config_paths "${OUT}/b/config/runtime.yaml" "${OUT}/b/projects/my-reactor/config/"anolis-runtime.*.yaml)
    [ "$(wc -l <<< "${paths}")" -eq 2 ]
    while IFS= read -r p; do
        [[ "${p}" == /opt/anolis/projects/my-reactor/config/* ]]
        # The file it names is in the bundle, where the install puts it.
        [ -f "${OUT}/b/${p#/opt/anolis/}" ]
    done <<< "${paths}"
    ! grep -rq 'projects/stage-project' "${OUT}/b/config" "${OUT}/b/projects"
    [[ "$output" == *"note: paths naming project 'stage-project' point at projects/my-reactor/"* ]]
    rm -rf "${PROJECT_DIR}"
}

@test "#694 a project under its own name stages without a rename note" {
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -eq 0 ]
    [[ "$output" != *"note: paths naming project"* ]]
    grep -q '/opt/anolis/projects/stage-project/config/provider-foo.yaml' "${OUT}/b/config/runtime.yaml"
}

@test "#694 a path to a file the project does not have fails staging, naming it" {
    PROJECT_DIR="$(_copy_fixture)"
    sed -i 's#config/provider-bar.yaml#config/provider-gone.yaml#' \
        "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"anolis-runtime.manual.yaml"* ]]
    [[ "$output" == *"config/provider-gone.yaml"* ]]
}

@test "#694 an unquoted flow list keeps its commas and brackets" {
    PROJECT_DIR="$(_copy_fixture_as my-reactor)"
    local old='../anolis-projects/projects/stage-project/config'
    sed -i "s#args: \[\"--config\", \"${old}/provider-foo.yaml\"\]#args: [--config, ${old}/provider-foo.yaml, --verbose]#; s#args: \[\"--config\", \"${old}/provider-bar.yaml\"\]#args: [--config, ${old}/provider-bar.yaml]#" \
        "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    grep -q 'args: \[--config, \.\./anolis-projects' "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -eq 0 ]
    grep -qF 'args: [--config, /opt/anolis/projects/my-reactor/config/provider-foo.yaml, --verbose]' "${OUT}/b/config/runtime.yaml"
    grep -qF 'args: [--config, /opt/anolis/projects/my-reactor/config/provider-bar.yaml]' "${OUT}/b/config/runtime.yaml"
}

@test "#694 a behaviour tree path is rewritten and checked like a config path" {
    PROJECT_DIR="$(_copy_fixture_as my-reactor)"
    printf 'automation:\n  enabled: false\n  behavior_tree: ../anolis-projects/projects/stage-project/behaviors/tree.xml\n' \
        >> "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -eq 0 ]
    grep -qx '  behavior_tree: /opt/anolis/projects/my-reactor/behaviors/tree.xml' "${OUT}/b/config/runtime.yaml"
    [ -f "${OUT}/b/projects/my-reactor/behaviors/tree.xml" ]
    sed -i 's#behaviors/tree.xml#behaviors/gone.xml#' "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b2" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"behaviors/gone.xml"* ]]
}

@test "#694 a path inside a comment is rewritten but not checked" {
    PROJECT_DIR="$(_copy_fixture)"
    printf '# formerly ../anolis-projects/projects/old-name/config/notes.txt\nnote: x  # ../anolis-projects/projects/x/config/gone.yaml\n' \
        >> "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -eq 0 ]
    grep -qx '# formerly /opt/anolis/projects/stage-project/config/notes.txt' "${OUT}/b/config/runtime.yaml"
    # A '#' inside quotes does not start a comment: this path is still checked.
    sed -i 's%^note: x .*%note: "a # b ../anolis-projects/projects/x/config/gone.yaml"%' "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    grep -q '^note: "a # b' "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b2" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"config/gone.yaml"* ]]
}

@test "#694 a path that climbs out of the project fails staging" {
    PROJECT_DIR="$(_copy_fixture)"
    sed -i 's#config/provider-bar.yaml#config/../../other/config/provider-bar.yaml#' \
        "${PROJECT_DIR}/config/anolis-runtime.manual.yaml"
    run assemble_bundle "${OUT}/b" x86_64
    [ "$status" -ne 0 ]
    [[ "$output" == *"config/../../other/config/provider-bar.yaml"* ]]
}
