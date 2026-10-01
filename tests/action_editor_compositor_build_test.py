"""Keep the browser backend tied to production shader inputs and offline packing."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/action_editor'))
from build_compositor import shader_variants, SHADERS
from build import build_document


class CompositorBuildTest(unittest.TestCase):
    def test_generated_shaders(self):
        sources = shader_variants()
        self.assertEqual(len(sources), 4)
        for name, source in zip(SHADERS, sources):
            self.assertTrue(source.startswith('#version 300 es\nprecision highp float;'))
            self.assertNotIn('layout(set', source)
            self.assertNotRegex(source, r'layout\(location = \d+\) in')
            self.assertIn('layout(std140) uniform Context', source)
            original = (ROOT / f'src/shaders/{name}.frag.glsl').read_text()
            # The adaptation only adds a coordinate shim; production shader
            # math after the first function is unchanged.
            body = original[original.index('void '):].replace('texture(u_texture,', 'sample_source(')
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
