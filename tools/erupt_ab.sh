#!/usr/bin/env bash
# erupt_ab.sh — prove the volcanic eruption presentation never moves the game.
#
# The eruption's fireball arcs are a PRESENTATION stage: they replace what the
# projected view draws and must not disturb what the ROM computes. Same binary,
# same save, same replayed input, one toggle (AR_ERUPT_OFF), so the traced
# range has to come back byte-identical with the stage on and off.
#
#   ./tools/erupt_ab.sh saves/aitos-eruption.rec [quit_frames]
#
# WHAT THIS CAN AND CANNOT PROVE, because it is easy to over-read.
#
# The renderer and the metadata producer (present_sim3d.c,
# sim_render_metadata.c) hold NO write path to the emulated bus at all — they
# read copies. Nothing there can leak into the game, with or without this
# script. What this guards is the OTHER half: the sprite hook in
# actraiser_widescreen_sprites.c runs INSIDE emulation and holds nine
# cpu_write16 calls, and the eruption reaches into it — the fireball's flight
# is resolved there, and eruption records are exempted from the vertical
# sprite window there. That is a real boundary held by care rather than by
# construction, and this is what checks it.
#
# The default range is chosen for that: $0380..$0580 is the OAM low table the
# sprite hook writes, $0F0C..$1061 is the eight eruption records, and tracing
# straight through covers both plus everything between. Both runs are the same
# binary on the same input, so ANY difference anywhere in the range is a real
# leak — a wider range is strictly stronger, and the only cost is trace size.
#
# It is manual. No ctest test needs the ROM or a recording, and wiring one in
# would break a clean checkout, so this runs when the sprite hook or the arc
# is touched rather than on every build.
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
LO="${AR_TRACE_LO:-0x0380}"
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
