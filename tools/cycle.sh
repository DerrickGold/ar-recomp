#!/usr/bin/env bash
# cycle.sh — one-command debug loop for the ActRaiser recomp.
#
#   tools/cycle.sh              regen-if-needed -> build -> run -> auto-triage
#   tools/cycle.sh --no-run     just regen-if-needed + build
#   tools/cycle.sh --triage     skip regen/build/run; triage runs/latest only
#
# The loop:
#   1. If any recomp/*.cfg is newer than the generated sources -> tools/regen.sh
#   2. reapply the default dev preset, then build
#   3. Run via tools/run.sh: every run gets its own timestamped runs/<ts>/
#      folder holding console.log (stdout+stderr), anomaly captures, and any
#      F2/F9/exit dumps — so parallel analysis of older runs never gets
#      clobbered. Play/repro, then quit (or F9).
#   4. Post-run: for every anom_*.jsonl in the run dir (and its dispatch log),
#      run the Go trace inspector in read-only diagnostic mode and write
#      <run>/cycle_report.txt. Candidate cfg lines remain review-only.
set -u
cd "$(dirname "$0")/.."

run_snesbuild() {
  if command -v snesbuild >/dev/null 2>&1; then
    snesbuild "$@"
  elif [ -x snesrecomp-go/build/snesbuild ]; then
    snesrecomp-go/build/snesbuild "$@"
  else
    go -C snesrecomp-go run ./cmd/snesbuild "$@"
  fi
}

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
  # 2. Reconfigure before the incremental build so current runtime/build
  # defaults replace values retained by an older CMake cache.
  cmake --preset dev || exit 1
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
      run_snesbuild trace-inspect "$f" --root . --diagnose --rom ar.sfc 2>&1
    done
  fi
  if [ -n "$DISPLOG" ]; then
    echo; echo "--- legacy dispatch log ---"
    run_snesbuild trace-inspect "$DISPLOG" --root . --diagnose --rom ar.sfc 2>&1
  fi
} | tee "$REPORT"

echo
echo "[cycle] report -> $REPORT"
echo "[cycle] candidate cfg lines are read-only evidence; review before editing authored config"
