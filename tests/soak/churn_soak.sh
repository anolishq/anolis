#!/usr/bin/env bash
# Accelerated restart-churn soak for anolis-runtime.
#
# Two phases, both watching for leaks / latent races / teardown bugs that only
# surface over many lifecycles (a literal >24h soak needs the self-hosted runner
# — see anolishq/.github#57; this is the bounded hosted-CI signal):
#
#   1. CHURN  — start -> wait "Runtime Ready" -> run briefly -> SIGTERM ->
#               assert clean exit (code 0, no hang) -> repeat until time budget.
#               Catches shutdown races, provider-supervisor teardown bugs, and
#               fd/handle leaks (which eventually make a cycle fail).
#   2. SUSTAIN — one long-lived instance; sample VmRSS over time and fail on
#               sustained growth (steady-state leak).
#
# Usage: churn_soak.sh <runtime-bin> <runtime-config> <churn-seconds> [run-seconds] [sustain-seconds]
set -uo pipefail

RUNTIME=${1:?runtime binary}
CONFIG=${2:?runtime config}
CHURN_SEC=${3:?churn duration seconds}
RUN_SEC=${4:-3}
SUSTAIN_SEC=${5:-90}
READY_MARKER="Runtime Ready"
WORK=$(mktemp -d)

wait_ready() { # <pid> <logfile>
  for _ in $(seq 1 150); do # up to 15s
    grep -q "$READY_MARKER" "$2" 2>/dev/null && return 0
    kill -0 "$1" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null; }

# ---- Phase 1: churn ------------------------------------------------------
echo "::group::Phase 1 — restart churn (${CHURN_SEC}s, ${RUN_SEC}s/cycle)"
end=$(( $(date +%s) + CHURN_SEC ))
cycle=0 fails=0
while [ "$(date +%s)" -lt "$end" ]; do
  cycle=$((cycle + 1))
  log="$WORK/cycle-$cycle.log"
  "$RUNTIME" --config="$CONFIG" >"$log" 2>&1 &
  pid=$!
  if ! wait_ready "$pid" "$log"; then
    echo "cycle $cycle: never reached '$READY_MARKER' (see below)"; tail -20 "$log"
    kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; fails=$((fails + 1)); continue
  fi
  sleep "$RUN_SEC"
  kill -TERM "$pid" 2>/dev/null
  # graceful exit within 10s, else it hung
  hung=1
  for _ in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || { hung=0; break; }; sleep 0.1; done
  if [ "$hung" = 1 ]; then
    echo "cycle $cycle: HUNG after SIGTERM"; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    fails=$((fails + 1)); continue
  fi
  wait "$pid"; ec=$?
  if [ "$ec" != 0 ]; then echo "cycle $cycle: non-clean exit ($ec)"; tail -20 "$log"; fails=$((fails + 1)); fi
done
echo "churn: $cycle cycles, $fails failure(s)"
echo "::endgroup::"

# ---- Phase 2: sustained RSS ---------------------------------------------
echo "::group::Phase 2 — sustained instance (${SUSTAIN_SEC}s, VmRSS trend)"
log="$WORK/sustain.log"
"$RUNTIME" --config="$CONFIG" >"$log" 2>&1 &
pid=$!
rss_fail=0
if wait_ready "$pid" "$log"; then
  sleep 5; base=$(rss_kb "$pid"); peak=$base
  samples=$(( SUSTAIN_SEC / 5 ))
  for _ in $(seq 1 "$samples"); do
    sleep 5
    kill -0 "$pid" 2>/dev/null || { echo "sustained: runtime died mid-run"; rss_fail=1; break; }
    r=$(rss_kb "$pid"); [ -n "$r" ] && [ "$r" -gt "$peak" ] && peak=$r
  done
  if [ "${base:-0}" -gt 0 ]; then
    growth=$(( (peak - base) * 100 / base ))
    echo "sustained VmRSS: base=${base}kB peak=${peak}kB growth=${growth}%"
    [ "$growth" -gt 25 ] && { echo "sustained RSS grew ${growth}% (>25%) — possible leak"; rss_fail=1; }
  fi
else
  echo "sustained: never became ready"; tail -20 "$log"; rss_fail=1
fi
kill -TERM "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
echo "::endgroup::"

rm -rf "$WORK"
echo "SUMMARY: churn_cycles=$cycle churn_fails=$fails sustain_fail=$rss_fail"
[ "$fails" = 0 ] && [ "$rss_fail" = 0 ]
