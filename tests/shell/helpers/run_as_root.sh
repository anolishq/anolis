#!/usr/bin/env bash
# Helper: wraps install.sh with EUID faked to 0 so dry-run tests
# can bypass the root check without needing sudo.
#
# Usage: run_as_root.sh <install.sh path> [args...]

SCRIPT="$1"; shift

# The caller (a bats test) exports ANOLIS_INSTALL_SH_NO_MAIN=1 so it can *source*
# install.sh for function-level tests without running main. This helper does the
# opposite — it *runs* install.sh — so we must clear that guard, or the spawned
# install.sh skips main and produces no output, silently making every dry-run
# assertion here vacuous (anolishq/anolis#207).
unset ANOLIS_INSTALL_SH_NO_MAIN

# Sed the script to replace the root check with a no-op, then execute.
# This avoids needing to source a script that calls main "$@" at the end.
# The patched copy is run from a file, not passed to `bash -c`: Linux caps one
# argument at 128 KiB (MAX_ARG_STRLEN), and install.sh has grown past it.
patched=$(mktemp "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/install-as-root.XXXXXX")
# shellcheck disable=SC2016  # single quotes intentional: match the literal $EUID
sed 's/\$EUID -ne 0/0 -ne 0/' "${SCRIPT}" > "${patched}"
exec bash "${patched}" "$@"
