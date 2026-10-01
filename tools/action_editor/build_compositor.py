#!/usr/bin/env python3
"""Build the standalone captured-scene viewer, embedding C WASM and generated shaders."""
import argparse
import base64
import gzip
import json
import os
from pathlib import Path
import re
import subprocess
from build_preview import ROOT, compiler_environment

SHADERS = ('blur', 'rim', 'dof_edge', 'priority_surface')
# Matches the semantic parameter bridge in compositor_preview.c. Refuse a
# silent uniform-layout drift if production shader interfaces change.
UNIFORMS = (
    [('vec2', 'texel'), ('float', 'radius'), ('float', 'pad0')],
    [('vec2', 'texel'), ('float', 'strength'), ('float', 'pad0')],
    [('float', name) for name in ('texel_w', 'texel_h', 'blur_radius', 'u_min',
                                  'u_max', 'v_min', 'v_max', 'edge_feather', 'lower_content_v_max')],
    [('vec2', 'size'), ('float', 'high_band'), ('float', 'additive')],
)


def shader_variants():
    result = []
    for name, expected in zip(SHADERS, UNIFORMS):
        text = (ROOT / f'src/shaders/{name}.frag.glsl').read_text()
        block = re.search(r'uniform Context \{(.*?)\};', text, re.S)
        if not block or re.findall(r'(vec2|float)\s+(\w+)\s*;', block[1]) != expected:
            raise ValueError(f'Update the WASM uniform bridge for {name}')
        text = text.replace('#version 450', '#version 300 es\nprecision highp float;')
        text = re.sub(r'layout\(location = \d+\) in ', 'in ', text)
        text = re.sub(r'layout\(set = 2, binding = 0\) ', '', text)
        text = text.replace('layout(set = 3, binding = 0)', 'layout(std140)')
        if 'layout(set' in text:
            raise ValueError(f'Unsupported shader binding in {name}')
        # Same shader math, with a sampling shim for GL render-target orientation.
        # Uploaded pixels have v=0 at the top; FBOs have v=0 at the bottom.
        text = text.replace('texture(u_texture,', 'sample_source(')
        pos = text.index('void ')
        text = text[:pos] + '''uniform bool u_flip_source;
vec4 sample_source(vec2 uv) {
    return texture(u_texture, vec2(uv.x, u_flip_source ? 1.0-uv.y : uv.y));
}
''' + text[pos:]
        result.append(text)
    return result


def build(output, compiler='emcc', scene=None):
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    wasm = output.with_suffix('.wasm')
    sources = (ROOT / 'tools/action_editor/compositor_sources.txt').read_text().splitlines()
    exports = ('Version', 'Init', 'Input', 'Capacity', 'Reset', 'Load', 'Render', 'Width', 'Height')
    command = [compiler, '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I', str(ROOT / 'src'), '-I', str(ROOT / 'snesrecomp-go/runtime/include'),
               '-I', str(ROOT / 'snesrecomp-go/runtime/src'),
               '-I', str(ROOT / 'snesrecomp-go/runtime/src/runner'),
               '-I', str(ROOT / 'snesrecomp-go/runtime/src/core'),
               'tools/action_editor/compositor_preview.c',
               'tools/action_editor/room_preview.c', 'tools/action_editor/room_scene.c',
               'src/action/action_scene_snapshot.c', 'src/action/action_room_scene.c',
               'src/action/action_bg_world.c', 'src/diorama/diorama_capture_blend.c',
               'src/diorama/diorama_rom_backdrop.c',
               'snesrecomp-go/runtime/src/scene_renderer.c',
               'snesrecomp-go/runtime/src/snes/ppu.c',
               'tools/action_editor/compositor_profile_stub.c', 'src/diorama/diorama_snapshot.c',
               *sources, '--no-entry', '-sSTANDALONE_WASM=1', '-sFILESYSTEM=0',
               '-sSTACK_SIZE=1048576', '-sINITIAL_MEMORY=67108864', '-sALLOW_MEMORY_GROWTH=0',
               *['-Wl,--export=DioramaPreview_' + x for x in exports],
               *['-Wl,--export=RoomPreview_' + x for x in
                 ('Load', 'Configure', 'Render', 'Reset', 'Width', 'Height', 'Uploads', 'Hash', 'SkyboxSource', 'SkyboxRoom', 'LoadSkybox')],
               '-o', str(wasm)]
    subprocess.run(command, cwd=ROOT, env=compiler_environment(compiler), check=True)
    template = (ROOT / 'tools/action_editor/compositor_preview.html').read_text()
    template = template.replace('/*__BACKEND__*/', (ROOT / 'tools/action_editor/webgl2_backend.js').read_text())
    template = template.replace('/*__SHADERS__*/', json.dumps(shader_variants()))
    sample = base64.b64encode(gzip.compress(scene.read_bytes(), mtime=0)).decode() if scene else ''
    template = template.replace('__SCENE__', sample)
    template = template.replace('__WASM__', base64.b64encode(wasm.read_bytes()).decode())
    output.write_text(template)
    print(f'[action-editor] captured-scene viewer {output} ({output.stat().st_size} bytes)')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--compiler', default=os.environ.get('EMCC', 'emcc'))
    p.add_argument('--scene', type=Path, help='Embed a gzip-compressed capture for an offline demonstration')
    args = p.parse_args()
    build(args.output, args.compiler, args.scene)
