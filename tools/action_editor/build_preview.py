#!/usr/bin/env python3
"""Build the offline shared-C baseline module with the pinned Emscripten SDK."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
EMSCRIPTEN_VERSION = "5.0.7"
EXPORTS = ["ActionPreview_" + name for name in
           ("Version", "Input", "Capacity", "Hash", "Reset", "Load", "Render")]


def compiler_environment(compiler):
    env = dict(os.environ)
    env.setdefault("EM_CACHE", str(ROOT / "build/action-editor/emscripten-cache"))
    version = subprocess.check_output([compiler, "--version"], env=env, text=True)
    if not re.search(r"\b" + re.escape(EMSCRIPTEN_VERSION) + r"\b", version.splitlines()[0]):
        raise RuntimeError(f"Shared preview requires Emscripten {EMSCRIPTEN_VERSION}: {version.splitlines()[0]}")
    return env


def build(output, compiler="emcc"):
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    env = compiler_environment(compiler)
    command = [compiler, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-I", str(ROOT / "src"), "-I", str(ROOT / "recomp"),
               "-I", str(ROOT / "snesrecomp-go/runtime/include"),
               str(ROOT / "tools/action_editor/action_preview.c"),
               str(ROOT / "src/action/action_scene_snapshot.c"),
               str(ROOT / "src/action/action_room_scene.c"),
               "--no-entry", "-sSTANDALONE_WASM=1", "-sFILESYSTEM=0",
               "-sSTACK_SIZE=1048576", "-sINITIAL_MEMORY=16777216",
               "-sALLOW_MEMORY_GROWTH=0", "-sMALLOC=none",
               *["-Wl,--export=" + name for name in EXPORTS], "-o", str(output)]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    print(f"[action-editor] shared C baseline {output} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default=os.environ.get("EMCC", "emcc"))
    args = parser.parse_args()
    build(args.output, args.compiler)
