"""Shader freshness permits compiler provenance changes, never program changes."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_shaders


class ShaderVerificationTest(unittest.TestCase):
    def test_compute_bindings_include_shared_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'test.comp.glsl'
            path.write_text('#include "context.glsl"\nlayout(local_size_x=64) in;\n'
                'layout(std430,set=0,binding=0) readonly buffer Data {uint v[];} data;')
            header = root / 'context.glsl'
            header.write_text('layout(std140,set=2,binding=0) uniform Context {vec4 x;} settings;')
            msl = 'kernel void main0(device Data& data [[buffer(0)]], constant Context& settings [[buffer(1)]]) {}'
            fixed = build_shaders.verify_compute_bindings(path, msl, metal=True, remap=True)
            self.assertIn('settings [[buffer(0)]]', fixed)
            self.assertIn('data [[buffer(1)]]', fixed)
            header.write_text('#include "test.comp.glsl"')
            with self.assertRaises(SystemExit):
                build_shaders.shader_source(path)

    def test_graphics_motion_buffer_follows_uniform_in_metal(self):
        source = '''layout(std430, set = 0, binding = 0) readonly buffer Motion { ivec4 values[]; } motions;
layout(std140, set = 1, binding = 0) uniform View { mat4 matrix; } settings;'''
        msl = 'vertex main0_out main0(const device Motion& motions [[buffer(0)]], constant View& settings [[buffer(1)]]) {}'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.vert.glsl'
            path.write_text(source)
            with self.assertRaises(SystemExit):
                build_shaders.verify_graphics_buffers(path, msl, metal=True)
            fixed = build_shaders.verify_graphics_buffers(path, msl, metal=True, remap=True)
            self.assertIn('settings [[buffer(0)]]', fixed)
            self.assertIn('motions [[buffer(1)]]', fixed)
            self.assertEqual(build_shaders.verify_graphics_buffers(path, fixed, metal=True), fixed)
            hlsl = 'ByteAddressBuffer motions : register(t0, space0); cbuffer View : register(b0, space1);'
            build_shaders.verify_graphics_buffers(path, hlsl, metal=False)
            for broken in (hlsl.replace('t0', 't1'), hlsl.replace('space1', 'space0')):
                with self.assertRaises(SystemExit):
                    build_shaders.verify_graphics_buffers(path, broken, metal=False)
            with self.assertRaises(SystemExit):
                build_shaders.verify_graphics_buffers(path, msl + msl, metal=True, remap=True)
            path.write_text(source.replace('readonly buffer', 'buffer'))
            with self.assertRaises(SystemExit):
                build_shaders.verify_graphics_buffers(path, fixed, metal=True)

    def test_vertex_pulling_does_not_require_stage_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'pull.vert.glsl'
            path.write_text('void main() { gl_Position = vec4(float(gl_VertexIndex)); }')
            metal = 'vertex main0_out main0(uint id [[vertex_id]]) {}'
            build_shaders.verify_msl_bindings(path, metal)
            path.write_text('layout(location=0) in vec2 position;')
            with self.assertRaises(SystemExit):
                build_shaders.verify_msl_bindings(path, metal)

    def test_metal_sampler_names_keep_declared_slots(self):
        source = '''layout(set = 2, binding = 0) uniform sampler2D previous_texture;
layout(set = 2, binding = 1) uniform sampler2D current_texture;'''
        msl = 'previous_texture [[texture(1)]], previous_textureSmplr [[sampler(1)]], current_texture [[texture(0)]], current_textureSmplr [[sampler(0)]]'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.frag.glsl'
            path.write_text(source)
            with self.assertRaises(SystemExit):
                build_shaders.verify_msl_samplers(path, msl)
            fixed = build_shaders.verify_msl_samplers(path, msl, remap=True)
            self.assertIn('previous_texture [[texture(0)]]', fixed)
            self.assertIn('current_textureSmplr [[sampler(1)]]', fixed)
            self.assertEqual(build_shaders.verify_msl_samplers(path, fixed), fixed)
            with self.assertRaises(SystemExit):
                build_shaders.verify_msl_samplers(path, msl + msl, remap=True)

    def test_compute_metal_uniforms_precede_storage_buffers(self):
        source = '''#version 450
layout(local_size_x = 8) in;
layout(std140, set = 2, binding = 0) uniform Settings { vec4 phase; } settings;
layout(std430, set = 0, binding = 0) readonly buffer Values { uint values[]; } input_values;
layout(std430, set = 1, binding = 0) writeonly buffer Output { uint values[]; } output_values;
'''
        msl = 'kernel void main0(const device Values& input_values [[buffer(0)]], device Output& output_values [[buffer(1)]], constant Settings& settings [[buffer(2)]]) {}'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.comp.glsl'
            path.write_text(source)
            with self.assertRaises(SystemExit):
                build_shaders.verify_compute_bindings(path, msl, metal=True)
            fixed = build_shaders.verify_compute_bindings(path, msl, metal=True, remap=True)
            self.assertIn('settings [[buffer(0)]]', fixed)
            self.assertIn('input_values [[buffer(1)]]', fixed)
            self.assertIn('output_values [[buffer(2)]]', fixed)
            self.assertEqual(build_shaders.verify_compute_bindings(path, fixed, metal=True), fixed)
            path.write_text(source.replace('readonly buffer', 'readonly\n  buffer'))
            self.assertEqual(build_shaders.verify_compute_bindings(path, fixed, metal=True), fixed)
            path.write_text(source)
            with self.assertRaises(SystemExit):
                build_shaders.verify_compute_bindings(path, msl + msl, metal=True, remap=True)
            path.write_text(source.replace('set = 1, binding = 0', 'set = 1, binding = 2'))
            with self.assertRaises(SystemExit):
                build_shaders.verify_compute_bindings(path, fixed, metal=True)

    def test_compute_hlsl_sampler_and_storage_namespaces(self):
        source = '''#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D previous_frame;
layout(std430, set = 0, binding = 1) readonly buffer Values { uint values[]; } input_values;
layout(std430, set = 1, binding = 0) writeonly buffer Output { uint values[]; } output_values;
'''
        hlsl = 'Texture2D previous_frame : register(t0, space0); SamplerState sampler0 : register(s0, space0); ByteAddressBuffer input_values : register(t1, space0); RWByteAddressBuffer output_values : register(u0, space1); [numthreads(64, 1, 1)] void main() {}'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.comp.glsl'
            path.write_text(source)
            build_shaders.verify_compute_bindings(path, hlsl, metal=False)
            for changed in (hlsl.replace('space1', 'space0'), hlsl.replace('s0, space0', 's1, space0'), hlsl.replace('[numthreads(64, 1, 1)]', '')):
                with self.assertRaises(SystemExit):
                    build_shaders.verify_compute_bindings(path, changed, metal=False)

    def test_compiler_identity_is_the_only_ignored_dxil_metadata(self):
        before = '; shader hash: a123\n!0 = !{!"dxc(private) old"}\n%v = fadd float %a, %b\n'
        after = before.replace('a123', 'b456').replace('old', 'new')
        self.assertEqual(build_shaders.dxil_program(before), build_shaders.dxil_program(after))
        for changed in (after.replace('fadd', 'fsub'), after + '!dx.valver = !{i32 1, i32 9}\n',
                        after + '; Resource Bindings: cb1,space3\n'):
            self.assertNotEqual(build_shaders.dxil_program(before), build_shaders.dxil_program(changed))

    def test_header_checks_other_backends_and_sizes_before_dxil_identity(self):
        source = Path('blur.frag.glsl')
        current = build_shaders.render_header(source, b'spirv', 'metal', b'old')
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(build_shaders, 'run_tool', return_value='same program') as dump, \
                patch.dict(build_shaders.TOOL_COMMANDS, {'dxc': ['dxc']}):
            path = Path(directory)
            def check(header, spv, msl):
                return build_shaders.check_header(source, header, spv, msl, b'new', path)
            self.assertEqual(check(current, b'spirv', 'metal'), 'OK (DXC build identity differs)')
            self.assertEqual(dump.call_count, 2)  # Parse both committed and regenerated containers.
            self.assertEqual(check(current, b'changed', 'metal'), 'STALE')
            self.assertEqual(check(current, b'spirv', 'changed'), 'STALE')
            self.assertEqual(check(current.replace('DXILSize = 3u', 'DXILSize = 4u'),
                                   b'spirv', 'metal'), 'STALE')
            dump.side_effect = ['old instructions', 'new instructions']
            self.assertEqual(check(current, b'spirv', 'metal'), 'STALE')


if __name__ == "__main__":
    unittest.main()
