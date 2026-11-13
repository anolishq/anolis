#!/usr/bin/env bash
# One self-contained Groot2 experiment on a private virtual display.
# usage: shot.sh <name> <actions-file|-> [groot2 args...]
# The actions file is sourced after startup; it may call `snap <label>`,
# xdotool, sleep. A final screenshot <name>-final.png is always taken.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
NAME="$1"; ACTIONS="$2"; shift 2
export XDG_CONFIG_HOME="$S/xdg/config" XDG_DATA_HOME="$S/xdg/data" XDG_CACHE_HOME="$S/xdg/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$S/shots" "$S/out"
export DISPLAY=:99
Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp >"$S/out/xvfb.log" 2>&1 &
XVFB=$!
sleep 1.5
(cd "$S/squashfs-root" && QT_QPA_PLATFORM=xcb ./AppRun "$@" >"$S/out/$NAME.groot.log" 2>&1) &
GROOT=$!
trap 'kill $GROOT 2>/dev/null; sleep 0.5; kill $XVFB 2>/dev/null' EXIT
snap() { import -window root "$S/shots/$NAME-$1.png" && echo "snap $NAME-$1.png"; }
sleep "${STARTUP_WAIT:-8}"
echo "windows:"; xdotool search --onlyvisible --name '.' 2>/dev/null | while read -r w; do echo "  $w $(xdotool getwindowname "$w" 2>/dev/null) $(xdotool getwindowgeometry "$w" 2>/dev/null | tr '\n' ' ')"; done
# shellcheck source=/dev/null
if [ "$ACTIONS" != "-" ]; then . "$ACTIONS"; fi
snap final
echo "--- groot log tail"; tail -15 "$S/out/$NAME.groot.log"
