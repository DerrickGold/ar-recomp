#!/usr/bin/env python3
"""Build and compare native/WASM scene replay; optionally cover a local US ROM."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from build_preview import ROOT, build


def check(rom=None):
    with tempfile.TemporaryDirectory(prefix="action-preview-") as directory:
        work = Path(directory)
        common = [os.environ.get("CC", "cc"), "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                  "-I", str(ROOT / "src"), "-I", str(ROOT / "recomp"),
                  "-I", str(ROOT / "snesrecomp-go/runtime/include")]
        scene = ["src/action/action_room_scene.c", "src/actraiser/quintet_lzss.c"]
        targets = {
            "codec": ["tests/action_scene_snapshot_test.c", "src/action/action_scene_snapshot.c"],
            "replay": ["tools/action_editor/replay_scene.c", "src/action/action_scene_snapshot.c", *scene],
        }
        if rom:
            targets["export"] = ["tools/action_editor/action_bg_export.c", *scene,
                                 "src/action/action_room_terrain.c", "src/regional/action/regional_terrain.c"]
        for name, sources in targets.items():
            subprocess.run([*common, *sources, "-o", str(work / name)], cwd=ROOT, check=True)
        fixture = work / "native.arscene"
        subprocess.run([str(work / "codec"), str(fixture)], check=True)
        wasm = work / "preview.wasm"
        build(wasm, os.environ.get("EMCC", "emcc"))
        command = ["node", str(ROOT / "tests/action_editor_preview.test.mjs"),
                   str(wasm), str(work / "replay"), str(fixture), str(work)]
        if rom:
            rooms = work / "rooms.json"
            subprocess.run([str(work / "export"), str(rom.resolve()), str(rooms)], check=True)
            command.append(str(rooms))
        subprocess.run(command, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", nargs="?", type=Path, help="Optional local US ROM for all 49 x 3 scenes")
    check(parser.parse_args().rom)
