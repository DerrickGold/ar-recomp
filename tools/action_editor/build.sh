#!/bin/sh
# Builds the standalone action-mode tile classification editor.
#
#   sh tools/action_editor/build.sh [rom] [out.html] [diorama-layers.ini] [settings.ini]
#
# Default build needs a C compiler, python3 and the pinned Emscripten compiler.
# ACTION_EDITOR_WASM=off skips shared previews; auto includes them when available.
# The exporter links the shared immutable
# ActionRoomScene decoder used by the game, so it owns no separate ROM logic.
set -e
cd "$(dirname "$0")/../.."
ROM="${1:-ar.sfc}"
OUT="${2:-build/action-editor/ar-action-layer-editor.html}"
LAYERS="${3:-diorama-layers.ini}"
VIEW_SETTINGS="${4:-settings.ini}"
[ -f "$ROM" ] || { echo "[action-editor] no ROM at $ROM"; exit 1; }
WASM="${ACTION_EDITOR_WASM:-on}"
case "$WASM" in
  auto|on|off) ;;
  *) echo "[action-editor] ACTION_EDITOR_WASM must be auto, on or off" >&2; exit 1 ;;
esac
if [ "$WASM" = on ] && ! command -v "${EMCC:-emcc}" >/dev/null 2>&1; then
  echo "[action-editor] shared previews require Emscripten; put emcc on PATH or set EMCC to its path" >&2
  echo "[action-editor] use ACTION_EDITOR_WASM=off for a JavaScript-only build, or auto to build shared previews when available" >&2
  exit 1
fi
"${PYTHON:-python3}" tools/generate_effect_defaults.py --check

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"${CC:-cc}" -O2 -std=c11 -Wall -Wextra -Wpedantic -I src -I recomp \
   -I snesrecomp-go/runtime/include \
   tools/action_editor/action_bg_export.c \
   src/action/action_room_scene.c src/action/action_room_terrain.c \
   src/regional/action/regional_terrain.c src/actraiser/quintet_lzss.c \
   -o "$TMP/export"
"$TMP/export" "$ROM" "$TMP/rooms.json"

if [ "$WASM" = on ] || { [ "$WASM" = auto ] && command -v "${EMCC:-emcc}" >/dev/null 2>&1; }; then
  "${PYTHON:-python3}" tools/action_editor/build_preview.py "$TMP/action-preview.wasm"
  "${PYTHON:-python3}" tools/action_editor/build_compositor.py "$(dirname "$OUT")/ar-renderer-preview.html"
  "${PYTHON:-python3}" tools/action_editor/build.py "$TMP/rooms.json" "$OUT" "$LAYERS" --settings "$VIEW_SETTINGS" --wasm "$TMP/action-preview.wasm" --compositor-href ar-renderer-preview.html --compositor-wasm "$(dirname "$OUT")/ar-renderer-preview.wasm"
else
  "${PYTHON:-python3}" tools/action_editor/build.py "$TMP/rooms.json" "$OUT" "$LAYERS" --settings "$VIEW_SETTINGS"
fi
