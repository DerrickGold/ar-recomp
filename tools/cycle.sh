#!/usr/bin/env bash
# cycle.sh — one-command debug loop for the ActRaiser recomp (DEBUG.md §1/§2).
#
#   tools/cycle.sh              regen-if-needed -> build -> run -> auto-triage
#   tools/cycle.sh --no-run     just regen-if-needed + build
#   tools/cycle.sh --triage     skip regen/build/run; triage newest artifacts only
#
# The loop:
#   1. If any recomp/*.cfg is newer than the generated sources -> tools/regen.sh
#   2. cmake --build build -j8
#   3. Run the game with dev-config (AR_TRACE_WATCH always-on anomaly capture),
#      teeing stderr+stdout to dump_cycle.txt. Play/repro, then quit (or F9).
#   4. Post-run: for every NEW saves/anom_*.jsonl (and the exit dispatch log),
#      run trace_slice --diagnose + resolve_miss dry-run -> saves/cycle_report.txt
#      ending with a PROPOSED CFG PATCH (apply via resolve_miss --apply, review
#      with git diff, then re-run cycle.sh).
set -u
cd "$(dirname "$0")/.."

RUN=1; BUILD=1
for a in "$@"; do
  case "$a" in
    --no-run) RUN=0 ;;
    --triage) RUN=0; BUILD=0 ;;
    *) echo "unknown arg: $a"; exit 2 ;;
  esac
done

if [ "$BUILD" = 1 ]; then
  # 1. regen iff cfg newer than generated code
  if [ -n "$(find recomp -name '*.cfg' -newer src/gen/bank00_v2.c 2>/dev/null)" ]; then
    echo "[cycle] cfg changed -> regen"
    bash tools/regen.sh || exit 1
  else
    echo "[cycle] cfg unchanged -> skipping regen"
  fi
  # 2. build
  cmake --build build -j8 || exit 1
fi

MARKER=$(mktemp)
if [ "$RUN" = 1 ]; then
  # 3. run (dev-config carries AR_TRACE_WATCH; env still overrides per-run)
  echo "[cycle] running — repro the bug, then quit (F9 dumps state)"
  ./build/ActRaiserRecomp ar.sfc --config dev-config.ini 2>&1 | tee dump_cycle.txt
fi

# 4. post-run triage of everything new
REPORT=saves/cycle_report.txt
NEW_ANOMS=$(find saves -maxdepth 1 -name 'anom_*.jsonl' -newer "$MARKER" 2>/dev/null | sort)
rm -f "$MARKER"
[ "$RUN" = 0 ] && NEW_ANOMS=$(ls -t saves/anom_*.jsonl 2>/dev/null | head -8 | sort)

{
  echo "=== cycle report $(date '+%F %T') ==="
  if [ -z "$NEW_ANOMS" ]; then
    echo "no new anomaly captures"
  else
    for f in $NEW_ANOMS; do
      echo; echo "--- $f ---"
      python3 tools/trace_slice.py "$f" --diagnose 2>&1
    done
  fi
  if [ -n "$NEW_ANOMS" ] || [ -f saves/dump_dispatch_log.json ]; then
    echo; echo "--- resolve_miss (dry run) ---"
    # shellcheck disable=SC2086
    python3 tools/resolve_miss.py $NEW_ANOMS saves/dump_dispatch_log.json 2>&1
  fi
} | tee "$REPORT"

echo
echo "[cycle] report -> $REPORT"
echo "[cycle] to apply SAFE lines:  python3 tools/resolve_miss.py <files> --apply && tools/cycle.sh"
