#!/usr/bin/env bash
# Helper: wraps install.sh with EUID faked to 0 so dry-run tests
# can bypass the root check without needing sudo.
#
# Usage: run_as_root.sh <install.sh path> [args...]

SCRIPT="$1"; shift

# Sed the script to replace the root check with a no-op, then execute.
# This avoids needing to source a script that calls main "$@" at the end.
exec bash -c "$(sed 's/\$EUID -ne 0/0 -ne 0/' "${SCRIPT}")" -- "$@"
