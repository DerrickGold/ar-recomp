#!/usr/bin/env bash
# erupt_ab.sh — prove the volcanic eruption presentation never moves a record.
#
# The eruption's fireball arcs are a PRESENTATION stage: they replace what the
# projected view draws and must not disturb what the ROM computes. Same binary,
# same save, same replayed input, one toggle (AR_ERUPT_OFF), so the eight
# eruption records have to trace byte-identical with the stage on and off.
#
#   ./tools/erupt_ab.sh saves/aitos-eruption.rec [quit_frames]
#
# Stricter than the snes9x differential oracle, which has to tolerate
# cross-emulator boot misalignment and compares cumulative written state
# instead of raw traces. A DIVERGED result means the presentation has grown a
# write path, which is the one thing it is never allowed to do.
set -u
cd "$(dirname "$0")/.."

REC="${1:-}"
FRAMES="${2:-4000}"
BIN=./build-release/ActRaiserRecomp
OUT=$(mktemp -d)

[ -n "$REC" ] || { echo "usage: $0 <input.rec> [quit_frames]"; exit 2; }
[ -f "$REC" ] || { echo "[erupt-ab] no such recording: $REC"; exit 2; }
[ -x "$BIN" ] || { echo "[erupt-ab] not built: $BIN"; exit 1; }

# The eight eruption world records, $0F0C..$103C inclusive of the last stride.
# Narrow on purpose: the diff is then the record fields themselves rather than
# a whole-WRAM haystack. Widen to the full range to prove the HLE writes
# nothing outside them.
LO="${AR_TRACE_LO:-0x0F0C}"
HI="${AR_TRACE_HI:-0x1061}"

run() {  # run <name> <presentation_off>
  AR_HEADLESS=1 AR_NO_RUN_DIR=1 \
  AR_QUIT_FRAMES="$FRAMES" \
  AR_INPUT_REPLAY="$REC" \
  AR_TRACE_LO="$LO" AR_TRACE_HI="$HI" \
  AR_WRAM_TRACE="$OUT/$1.jsonl" \
  AR_ERUPT_OFF="$2" \
    "$BIN" ar.sfc >"$OUT/$1.log" 2>&1
  printf '[erupt-ab] %-4s off=%s  %8s lines\n' "$1" "$2" "$(wc -l <"$OUT/$1.jsonl" | tr -d ' ')"
}

echo "[erupt-ab] range $LO..$HI, $FRAMES frames, rec $REC"
run on  ""
run off 1

if diff -q "$OUT/on.jsonl" "$OUT/off.jsonl" >/dev/null; then
  echo "[erupt-ab] IDENTICAL — the presentation moved nothing"
  rm -rf "$OUT"; exit 0
fi

echo "[erupt-ab] DIVERGED. First differing records:"
diff "$OUT/on.jsonl" "$OUT/off.jsonl" | head -40
echo "[erupt-ab] traces kept in $OUT"
exit 1
