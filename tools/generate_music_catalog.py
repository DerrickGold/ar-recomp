#!/usr/bin/env python3
"""Regenerate only the shipped template's music entries, never player assets."""

import argparse
from music_catalog import ROOT, load_music_catalog

MARKER = "# BEGIN GENERATED MUSIC CATALOG\n"
TEMPLATE = ROOT / "installer/internal/builder/assets/manifest.ini"


def generated_manifest():
    prefix, _ = TEMPLATE.read_text().split(MARKER, 1)
    entries = [MARKER.rstrip(), "# From assets/music.json; run tools/generate_music_catalog.py."]
    for track in load_music_catalog():
        entries += ["", f"# {track['name']}", f"[music:{track['id']}]",
                    f"src = {track['src']}", f"file = {track['file']}"]
        if "loop" in track:
            entries.append(f"loop = {int(track['loop'])}")
    return prefix + "\n".join(entries) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generated = generated_manifest()
    if args.check:
        if TEMPLATE.read_text() != generated:
            parser.error("music template is stale; run tools/generate_music_catalog.py")
    else:
        TEMPLATE.write_text(generated)
