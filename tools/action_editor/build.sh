#!/bin/sh
# Builds the standalone action-mode tile classification editor.
#
#   sh tools/action_editor/build.sh [rom] [out.html] [diorama-layers.ini]
#
# Needs only a C compiler and python3. The exporter links the shared immutable
# ActionRoomScene decoder used by the game, so it owns no separate ROM logic.
set -e
cd "$(dirname "$0")/../.."
ROM="${1:-ar.sfc}"
OUT="${2:-build/action-editor/ar-action-layer-editor.html}"
LAYERS="${3:-diorama-layers.ini}"
[ -f "$ROM" ] || { echo "[action-editor] no ROM at $ROM"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"${CC:-cc}" -O2 -std=c11 -Wall -Wextra -Wpedantic -I src -I recomp \
   -I snesrecomp-go/runtime/src -I snesrecomp-go/runtime/src/snes \
   tools/action_editor/action_bg_export.c \
   src/action/action_room_scene.c src/quintet_lzss.c \
   -o "$TMP/export"
"$TMP/export" "$ROM" "$TMP/rooms.json"

python3 - "$TMP/rooms.json" "$OUT" "$LAYERS" <<'PY'
import json, sys, pathlib
rooms_path, out_path, layers_path = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
here = pathlib.Path("tools/action_editor")
data = json.dumps(json.load(open(rooms_path)), separators=(",", ":"))
layers = layers_path.read_text() if layers_path.exists() else ""
html = (here / "editor.head.html").read_text() \
     + "\n<script>window.__ACTION_BG__=" + data + ";</script>\n" \
     + "<script>window.__DIORAMA_LAYERS__=" + json.dumps(layers) + ";" \
     + "window.__DIORAMA_LAYERS_NAME__=" + json.dumps(layers_path.name) + ";</script>\n" \
     + (here / "editor.body.html").read_text()
out_path.parent.mkdir(parents=True, exist_ok=True)
out_path.write_text(html)
print(f"[action-editor] {out_path}  {len(html)/1048576:.2f} MiB")
PY
