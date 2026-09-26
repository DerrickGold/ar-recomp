#!/usr/bin/env python3
"""Bundle exported room data and authored sources into one offline HTML editor."""

import argparse
import json
from pathlib import Path
import re

SOURCES = Path(__file__).resolve().parent
SCRIPT_SOURCE = re.compile(r'<script src="([a-z_]+\.js)"></script>')


def script_json(value, **options):
    """JSON inside a script must not let text such as </script> end its element."""
    return json.dumps(value, **options).replace("<", "\\u003c")


def build_document(rooms_path: Path, layers_path: Path) -> str:
    data = script_json(json.loads(rooms_path.read_text()), separators=(",", ":"))
    layers = layers_path.read_text() if layers_path.exists() else ""
    body = (SOURCES / "editor.body.html").read_text()
    # Keep classic script order and shared bindings for file:// use. The source
    # template is the sole load-order list; the artifact needs no adjacent files.
    body = SCRIPT_SOURCE.sub(
        lambda match: "<script>" + (SOURCES / match[1]).read_text() + "</script>", body)
    return ((SOURCES / "editor.head.html").read_text()
            + "\n<script>window.__ACTION_BG__=" + data + ";</script>\n"
            + "<script>window.__DIORAMA_LAYERS__=" + script_json(layers) + ";"
            + "window.__DIORAMA_LAYERS_NAME__=" + script_json(layers_path.name) + ";</script>\n"
            + body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rooms", type=Path, help="JSON from action_bg_export")
    parser.add_argument("output", type=Path)
    parser.add_argument("layers", type=Path)
    args = parser.parse_args()
    html = build_document(args.rooms, args.layers)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(html)
    print(f"[action-editor] {args.output}  {len(html) / 1048576:.2f} MiB")


if __name__ == "__main__":
    main()
