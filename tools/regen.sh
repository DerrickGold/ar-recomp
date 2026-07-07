#!/usr/bin/env bash
# Regen pipeline driver for ActRaiserRecomp.
#
# Regenerates src/gen/*.c from the recomp/bank_*.cfg configs over a verified
# ar.sfc, then syncs recomp/funcs.h.
#
# Flags:
#   --no-tests   skip the framework test suite (default: run it).
#   -h | --help  this message.
#
# Run from anywhere — paths resolve relative to this script's location.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RUN_TESTS=1
for arg in "$@"; do
  case "$arg" in
    --no-tests) RUN_TESTS=0 ;;
    -h|--help)  sed -n '2,/^set -euo/p' "$0" | sed -n '/^# /p' | sed 's/^# //'; exit 0 ;;
    *) echo "regen.sh: unknown flag: $arg (try --help)" >&2; exit 2 ;;
  esac
done

cd "$ROOT"

ROM="ar.sfc"
TESTS="snesrecomp/tests/run_tests.py"

PYTHON="${PYTHON:-$(command -v python3 || command -v python || true)}"
if [ -z "$PYTHON" ]; then
  echo "regen.sh: no python3/python interpreter found on PATH" >&2
  exit 1
fi

if [ ! -f "$ROM" ]; then
  echo "regen.sh: $ROM not found at repo root — drop a verified ActRaiser ROM there." >&2
  exit 1
fi

step() { echo; echo "=== $* ==="; }

# Parallel emit (v2_regen --jobs, reads SNESRECOMP_JOBS): the bank-emit Pool
# has existed upstream but defaulted to 1 worker, so regens ran sequential.
# Default to the physical core count; override with SNESRECOMP_JOBS=N (1 = old
# sequential behavior, useful when bisecting emit-order-sensitive bugs).
export SNESRECOMP_JOBS="${SNESRECOMP_JOBS:-$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.physicalcpu 2>/dev/null || echo 4)}"

step "Regenerating banks (SNESRECOMP_JOBS=$SNESRECOMP_JOBS)"
"$PYTHON" snesrecomp/tools/v2_regen.py --rom "$ROM" \
    --cfg-dir recomp --out-dir src/gen --prefix actraiser

step "Syncing funcs.h"
"$PYTHON" snesrecomp/tools/v2_sync_funcs_h.py --cfg-dir recomp \
    --out recomp/funcs.h

step "Refreshing gen metadata sidecar (trace_slice --diagnose static join)"
"$PYTHON" tools/gen_metadata.py

step "RTS-web census (uncovered pushed-continuations; see DEBUG.md §5)"
# Every cfg round can make NEW code reachable whose continuation pushes were
# always statically visible but irrelevant until now (the $F98A lesson,
# 2026-07-06: the census had flagged it since forever — nobody re-ran it).
# Keep a copy per regen and print the DELTA of newly-uncovered entries so
# they get triaged (shape-check per DEBUG.md §1 ⚠️ — do NOT blind-register).
"$PYTHON" tools/find_rts_webs.py > saves/rts_webs.txt 2>/dev/null || true
if [ -f saves/rts_webs.prev.txt ]; then
  NEW_UNC=$(grep '\[UNC\]' saves/rts_webs.txt | grep -Fxv -f <(grep '\[UNC\]' saves/rts_webs.prev.txt) || true)
  if [ -n "$NEW_UNC" ]; then
    echo "!! NEW uncovered continuations since last regen (triage before next play):"
    echo "$NEW_UNC" | sed 's/^/  /'
  else
    echo "no new uncovered continuations since last regen"
  fi
else
  echo "first census run — baseline written; full UNC list in saves/rts_webs.txt"
fi
cp saves/rts_webs.txt saves/rts_webs.prev.txt

if [ "$RUN_TESTS" -eq 1 ]; then
  step "Framework tests"
  "$PYTHON" "$TESTS"
fi

step "Done"
