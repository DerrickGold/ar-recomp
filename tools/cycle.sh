#!/usr/bin/env bash
# cycle.sh — one-command debug loop for the ActRaiser recomp (DEBUG.md §1/§2).
#
#   tools/cycle.sh              regen-if-needed -> build -> run -> auto-triage
#   tools/cycle.sh --no-run     just regen-if-needed + build
#   tools/cycle.sh --triage     skip regen/build/run; triage runs/latest only
#
# The loop:
#   1. If any recomp/*.cfg is newer than the generated sources -> tools/regen.sh
#   2. cmake --build build -j8
#   3. Run via tools/run.sh: every run gets its own timestamped runs/<ts>/
#      folder holding console.log (stdout+stderr), anomaly captures, and any
#      F2/F9/exit dumps — so parallel analysis of older runs never gets
#      clobbered. Play/repro, then quit (or F9).
#   4. Post-run: for every anom_*.jsonl in the run dir (and its dispatch log),
#      run trace_slice --diagnose + resolve_miss dry-run -> <run>/cycle_report.txt
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
  # 1. regen iff cfg newer than the last successful generation. Large banks
  # may be split into bankXX_partNN_v2.c files, so do not key freshness to a
  # monolithic bank00_v2.c filename.
  GEN_STAMP=src/gen/.v2_regen_stamp
  if [ ! -f "$GEN_STAMP" ] || [ -n "$(find recomp -name '*.cfg' -newer "$GEN_STAMP" 2>/dev/null)" ]; then
    echo "[cycle] cfg changed -> regen"
    bash tools/regen.sh || exit 1
  else
    echo "[cycle] cfg unchanged -> skipping regen"
  fi
  # 2. build
  cmake --build build -j8 || exit 1
fi

if [ "$RUN" = 1 ]; then
  # 3. run (run.sh creates runs/<ts>/, captures console, sweeps artifacts)
  echo "[cycle] running — repro the bug, then quit (F9 dumps state)"
  bash tools/run.sh
fi

# 4. post-run triage of the newest run's artifacts
RUN_DIR=$(readlink runs/latest 2>/dev/null)
RUN_DIR="runs/${RUN_DIR:-latest}"
[ -d "$RUN_DIR" ] || { echo "[cycle] no runs/ to triage"; exit 1; }
REPORT="$RUN_DIR/cycle_report.txt"
ANOMS=$(ls "$RUN_DIR"/anom_*.jsonl 2>/dev/null | sort)
DISPLOG=$(ls "$RUN_DIR"/dump_*dispatch_log.json 2>/dev/null | head -1)

{
  echo "=== cycle report $(date '+%F %T')  [$RUN_DIR] ==="
  if [ -z "$ANOMS" ]; then
    echo "no anomaly captures in this run"
  else
    for f in $ANOMS; do
      echo; echo "--- $f ---"
      python3 tools/trace_slice.py "$f" --diagnose 2>&1
    done
  fi
  if [ -n "$ANOMS" ] || [ -n "$DISPLOG" ]; then
    echo; echo "--- resolve_miss (dry run) ---"
    # shellcheck disable=SC2086
    python3 tools/resolve_miss.py $ANOMS $DISPLOG 2>&1
  fi
} | tee "$REPORT"

echo
echo "[cycle] report -> $REPORT"
echo "[cycle] to apply SAFE lines:  python3 tools/resolve_miss.py <files> --apply && tools/cycle.sh"
