#!/usr/bin/env bash
# Regen pipeline driver for ActRaiserRecomp.
#
# Regenerates src/gen/*.c with the Go recompiler from recomp/bank_*.cfg over a
# verified ar.sfc, then refreshes every generated sidecar. Python is not a
# dependency of this normal regeneration path.
#
# Flags:
#   --no-tests   skip the Go toolchain test suite (default: run it).
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
GO="${GO:-$(command -v go || true)}"
if [ -z "$GO" ]; then
  echo "regen.sh: no Go toolchain found on PATH" >&2
  exit 1
fi

if [ ! -f "$ROM" ]; then
  echo "regen.sh: $ROM not found at repo root — drop a verified ActRaiser ROM there." >&2
  exit 1
fi

step() { echo; echo "=== $* ==="; }

# Build once and reuse the same binary for every pipeline stage. mktemp keeps
# host architecture binaries out of the repository.
GO_TOOL="$(mktemp "${TMPDIR:-/tmp}/actraiser-v2regen.XXXXXX")"
trap 'rm -f "$GO_TOOL"' EXIT
step "Building Go recomp toolchain"
"$GO" -C snesrecomp-go build -o "$GO_TOOL" ./cmd/v2regen

# Per-function workers avoid the old bank-level imbalance where bank $00 held
# 1,512 of 1,748 initial entries. Override with SNESRECOMP_JOBS=N when needed.
SNESRECOMP_JOBS="${SNESRECOMP_JOBS:-$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.physicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

step "Regenerating banks (SNESRECOMP_JOBS=$SNESRECOMP_JOBS)"
"$GO_TOOL" regen --rom "$ROM" --cfg-dir recomp --out-dir src/gen \
    --jobs "$SNESRECOMP_JOBS" --allow-stubs

step "Syncing funcs.h"
"$GO_TOOL" sync-funcs --cfg-dir recomp --out recomp/funcs.h

step "Refreshing gen metadata sidecar (trace_slice --diagnose static join)"
"$GO_TOOL" metadata --gen-dir src/gen --cfg-dir recomp \
    --out saves/gen_meta.json

step "RTS-web census (uncovered pushed-continuations; see DEBUG.md §5)"
# Every cfg round can make NEW code reachable whose continuation pushes were
# always statically visible but irrelevant until now (the $F98A lesson,
# 2026-07-06: the census had flagged it since forever — nobody re-ran it).
# Keep a copy per regen and print the DELTA of newly-uncovered entries so
# they get triaged (shape-check per DEBUG.md §1 ⚠️ — do NOT blind-register).
"$GO_TOOL" rts-webs --rom "$ROM" --cfg-dir recomp \
    > saves/rts_webs.txt 2>/dev/null || true
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

step "Hard stub census"
STUB_STATUS=0
"$GO_TOOL" stub-census --gen-dir src/gen || STUB_STATUS=$?

if [ "$RUN_TESTS" -eq 1 ]; then
  step "Go recomp toolchain tests"
  "$GO" -C snesrecomp-go test ./...
fi

step "Done"
if [ "$STUB_STATUS" -ne 0 ]; then
  echo "regen.sh: generated outputs and sidecars are complete, but the hard stub gate failed" >&2
  exit "$STUB_STATUS"
fi
