#!/usr/bin/env python3
"""Bundle exported room data and authored sources into one offline HTML editor."""

import argparse
import base64
import html
import json
from pathlib import Path
import re

SOURCES = Path(__file__).resolve().parent
SCRIPT_SOURCE = re.compile(r'<script src="([a-z0-9_]+\.js)"></script>')


def script_json(value, **options):
    """JSON inside a script must not let text such as </script> end its element."""
    return json.dumps(value, **options).replace("<", "\\u003c")


def preview_settings(settings_path: Path):
    """Embed presentation preferences; the offline editor never writes settings."""
    values = {}
    if settings_path.exists():
        for line in settings_path.read_text().splitlines():
            key, separator, value = line.partition("=")
            if separator and not key.lstrip().startswith(("#", ";")):
                values[key.strip()] = value.strip()
    dynamic = values.get("diorama_camera_mode", "Dynamic Cam") == "Dynamic Cam"
    prefix = "diorama_dyncam_baseline_" if dynamic else "diorama_"

    def number(key, fallback, scale):
        try:
            return int(values[key]) / scale
        except (KeyError, ValueError):
            return fallback

    return {
        "aspect": values.get("extended_aspect", "16:10")
        if values.get("extended_aspect", "16:10") in ("16:9", "16:10") else "4:3",
        "pixelAspect": "crt" if values.get("pixel_aspect") in ("4:3 CRT", "CRT (4:3)") else "square",
        "distance": number(prefix + "distance_x100", 3.25, 100),
        "tiltX": number(prefix + "tilt_x_mrad", 0, 1000),
        "tiltY": number(prefix + "tilt_y_mrad", 0, 1000),
        "cameraMode": "dynamic" if dynamic else "free",
        "source": settings_path.name if settings_path.exists() else "defaults",
    }


def build_document(rooms_path: Path, layers_path: Path, settings_path=None, wasm_path=None, compositor_href=None, compositor_wasm=None) -> str:
    data = script_json(json.loads(rooms_path.read_text()), separators=(",", ":"))
    layers = layers_path.read_text() if layers_path.exists() else ""
    effects_path = layers_path.parent / 'action-effects.ini'
    effects = effects_path.read_text() if effects_path.exists() else '[effects]\nversion=1\n'
    preview = preview_settings(settings_path or layers_path.parent / "settings.ini")
    wasm = base64.b64encode(wasm_path.read_bytes()).decode('ascii') if wasm_path else None
    shared = base64.b64encode(compositor_wasm.read_bytes()).decode('ascii') if compositor_wasm else None
    shaders = []
    if shared:
        from build_compositor import shader_variants
        shaders = shader_variants()
    body = (SOURCES / "editor.body.html").read_text()
    link = ('<p class="hint"><a href="' + html.escape(compositor_href, quote=True) +
            '" target="_blank" rel="noopener">Open captured scene renderer</a>'
            ' · Shared C compositor; separate from live map edits.</p>') if compositor_href else ''
    body = body.replace('<!-- SHARED_COMPOSITOR -->', link)
    # Keep classic script order and shared bindings for file:// use. The source
    # template is the sole load-order list; the artifact needs no adjacent files.
    body = SCRIPT_SOURCE.sub(
        lambda match: "<script>" + (SOURCES / match[1]).read_text() + "</script>", body)
    return ((SOURCES / "editor.head.html").read_text()
            + "\n<script>window.__ACTION_BG__=" + data + ";</script>\n"
            + "<script>window.__DIORAMA_LAYERS__=" + script_json(layers) + ";"
            + "window.__DIORAMA_LAYERS_NAME__=" + script_json(layers_path.name) + ";"
            + "window.__ACTION_EFFECTS__=" + script_json(effects) + ";"
            + "window.__ACTION_VIEW__=" + script_json(preview) + ";"
            + "window.__ACTION_PREVIEW_WASM__=" + script_json(wasm) + ";"
            + "window.__ROOM_PREVIEW_WASM__=" + script_json(shared) + ";"
            + "window.__ROOM_PREVIEW_SHADERS__=" + script_json(shaders) + ";</script>\n"
            + body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rooms", type=Path, help="JSON from action_bg_export")
    parser.add_argument("output", type=Path)
    parser.add_argument("layers", type=Path)
    parser.add_argument("--settings", type=Path, help="Presentation settings for coverage guides")
    parser.add_argument("--wasm", type=Path, help="Optional shared C baseline module to embed offline")
    parser.add_argument("--compositor-href", help="Optional adjacent captured-scene viewer")
    parser.add_argument("--compositor-wasm", type=Path, help="Whole-room shared renderer to embed")
    args = parser.parse_args()
    html = build_document(args.rooms, args.layers, args.settings, args.wasm, args.compositor_href, args.compositor_wasm)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(html)
    print(f"[action-editor] {args.output}  {len(html) / 1048576:.2f} MiB")


if __name__ == "__main__":
    main()
