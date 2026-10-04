#!/usr/bin/env python3
"""Compare whole-room PPU surfaces in native C and WASM using a local editor bundle."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from build_preview import ROOT


def check(html, wasm, sanitize):
    subprocess.run(['node', str(ROOT / 'tests/action_editor_policy_transport.test.mjs'),
                    str(html.resolve()), str(wasm.resolve())], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='action-room-') as directory:
        work = Path(directory)
        compiler = os.environ.get('CC', 'cc')
        command = [compiler, '-O1' if sanitize else '-O2', '-g', '-std=c11',
                   '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
                   '-I', 'src', '-I', 'tools/action_editor',
                   '-I', 'snesrecomp-go/runtime/include', '-I', 'snesrecomp-go/runtime/src',
                   '-I', 'snesrecomp-go/runtime/src/core', '-I', 'snesrecomp-go/runtime/src/runner',
                   'tests/action_editor_room_test.c', 'tools/action_editor/room_scene.c',
                   *(ROOT / 'tools/action_editor/environment_sources.txt').read_text().splitlines(),
                   'src/action/action_scene_snapshot.c', 'src/action/action_room_scene.c',
                   'src/action/action_bg_world.c', 'src/action/action_bg_plan.c',
                   'src/diorama/diorama_capture_blend.c', 'src/diorama/diorama_layer_order.c',
                   'src/diorama/diorama_projection.c', 'src/diorama/diorama_skybox_uv.c',
                   'src/diorama/diorama_depth_shapes.c',
                   'src/render/scene3d_math.c', 'src/render/presentation_layout.c',
                   'snesrecomp-go/runtime/src/scene_renderer.c', 'snesrecomp-go/runtime/src/snes/ppu.c',
                   '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                   '-lm', '-o', str(work / 'replay')]
        if sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run(['node', str(ROOT / 'tests/action_editor_room.test.mjs'),
                        str(html.resolve()), str(wasm.resolve()), str(work / 'replay'), str(work)],
                       cwd=ROOT, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--html', type=Path, default=ROOT / 'build/action-editor/ar-action-layer-editor.html')
    parser.add_argument('--wasm', type=Path, default=ROOT / 'build/action-editor/ar-renderer-preview.wasm')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    check(args.html, args.wasm, args.sanitize)
