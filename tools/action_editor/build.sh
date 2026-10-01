#!/bin/sh
# Builds the standalone action-mode tile classification editor.
#
#   sh tools/action_editor/build.sh [rom] [out.html] [diorama-layers.ini] [settings.ini]
#
# Base editor needs a C compiler and python3; shared preview also uses the pinned
# Emscripten compiler when available (ACTION_EDITOR_WASM=on/off/auto).
# The exporter links the shared immutable
# ActionRoomScene decoder used by the game, so it owns no separate ROM logic.
set -e
cd "$(dirname "$0")/../.."
ROM="${1:-ar.sfc}"
OUT="${2:-build/action-editor/ar-action-layer-editor.html}"
LAYERS="${3:-diorama-layers.ini}"
VIEW_SETTINGS="${4:-settings.ini}"
[ -f "$ROM" ] || { echo "[action-editor] no ROM at $ROM"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"${CC:-cc}" -O2 -std=c11 -Wall -Wextra -Wpedantic -I src -I recomp \
   -I snesrecomp-go/runtime/include \
   tools/action_editor/action_bg_export.c \
   src/action/action_room_scene.c src/action/action_room_terrain.c \
   src/regional/action/regional_terrain.c src/actraiser/quintet_lzss.c \
   -o "$TMP/export"
"$TMP/export" "$ROM" "$TMP/rooms.json"

WASM="${ACTION_EDITOR_WASM:-auto}"
case "$WASM" in
  auto|on|off) ;;
  *) echo "[action-editor] ACTION_EDITOR_WASM must be auto, on or off" >&2; exit 1 ;;
esac
if [ "$WASM" = on ] || { [ "$WASM" = auto ] && command -v "${EMCC:-emcc}" >/dev/null 2>&1; }; then
  "${PYTHON:-python3}" tools/action_editor/build_preview.py "$TMP/action-preview.wasm"
  "${PYTHON:-python3}" tools/action_editor/build_compositor.py "$(dirname "$OUT")/ar-renderer-preview.html"
  "${PYTHON:-python3}" tools/action_editor/build.py "$TMP/rooms.json" "$OUT" "$LAYERS" --settings "$VIEW_SETTINGS" --wasm "$TMP/action-preview.wasm" --compositor-href ar-renderer-preview.html --compositor-wasm "$(dirname "$OUT")/ar-renderer-preview.wasm"
else
  "${PYTHON:-python3}" tools/action_editor/build.py "$TMP/rooms.json" "$OUT" "$LAYERS" --settings "$VIEW_SETTINGS"
fi
