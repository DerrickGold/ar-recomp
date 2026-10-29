"""Keep the browser backend tied to production shader inputs and offline packing."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/action_editor'))
from build_compositor import shader_variants, SHADERS
from build_preview import compiler_environment
from build import build_document


class CompositorBuildTest(unittest.TestCase):
    def test_supported_compilers_and_workspace_cache(self):
        for version in ('5.0.7', '6.0.12', '6.0.12-git'):
            with self.subTest(version=version), patch.dict('os.environ', {}, clear=True), \
                    patch('build_preview.subprocess.check_output', return_value=
                          f'emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) {version}\n') as probe:
                env = compiler_environment('/sdk/emcc')
                self.assertEqual(env['EM_CACHE'], str(ROOT / 'build/action-editor/emscripten-cache'))
                probe.assert_called_once_with(['/sdk/emcc', '--version'], env=env, text=True)

        with patch.dict('os.environ', {'EM_CACHE': '/custom-cache'}), \
                patch('build_preview.subprocess.check_output', return_value='emcc (Emscripten) 6.0.12\n'):
            self.assertEqual(compiler_environment('emcc')['EM_CACHE'], '/custom-cache')

    def test_unvalidated_compilers_report_how_to_select_supported_sdk(self):
        for banner in ('emcc (Emscripten) 6.0.13\n', 'emcc (Emscripten) 6.0.12.1\n',
                       'clang version 6.0.12\n', ''):
            with self.subTest(banner=banner), \
                    patch('build_preview.subprocess.check_output', return_value=banner):
                with self.assertRaisesRegex(RuntimeError, r'5\.0\.7 or 6\.0\.12.*Set EMCC'):
                    compiler_environment('emcc')

    def test_generated_shaders(self):
        sources = shader_variants()
        self.assertEqual(len(sources), 7)
        for name, source in zip(SHADERS, sources):
            self.assertTrue(source.startswith('#version 300 es\nprecision highp float;'))
            self.assertNotIn('layout(set', source)
            self.assertNotRegex(source, r'layout\(location = \d+\) in')
            self.assertIn('layout(std140) uniform Context', source)
            self.assertLess(source.index('uniform sampler2D u_texture'),
                            source.index('vec4 sample_source('))
            self.assertNotIn('#include', source)
            original = (ROOT / f'src/shaders/{name}.frag.glsl').read_text()
            # The adaptation only adds a coordinate shim; production shader
            # math after the first function is unchanged.
            first = re.search(r'^(?:void|vec[234]|float)\s+\w+\s*\(', original, re.M).start()
            body = original[first:].replace('texture(u_texture,', 'sample_source(')
            body = body.replace('textureLod(u_texture,', 'sample_source_lod(')
            self.assertTrue(source.endswith(body))

    def test_optional_viewer_link_is_escaped_and_independent(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'rooms.json').write_text('{"rooms":[],"blobs":[]}')
            plain = build_document(p / 'rooms.json', p / 'absent.ini')
            self.assertNotIn('Open captured scene renderer', plain)
            linked = build_document(p / 'rooms.json', p / 'absent.ini', compositor_href='view&".html')
            self.assertIn('href="view&amp;&quot;.html"', linked)
            self.assertIn('separate from live map edits', linked)

    def test_viewer_javascript_syntax_and_no_external_dependencies(self):
        template = (ROOT / 'tools/action_editor/compositor_preview.html').read_text()
        template = template.replace('/*__BACKEND__*/',
                                    (ROOT / 'tools/action_editor/webgl2_backend.js').read_text())
        template = template.replace('/*__SHADERS__*/', '[]')
        self.assertNotRegex(template, r'<script[^>]+src=')
        source = re.search(r'<script>(.*?)</script>', template, re.S)[1]
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / 'viewer.js'
            p.write_text(source)
            subprocess.run(['node', '--check', str(p)], check=True)


if __name__ == '__main__':
    unittest.main()
