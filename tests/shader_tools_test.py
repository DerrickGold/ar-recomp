"""Shader freshness permits compiler provenance changes, never program changes."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_shaders


class ShaderVerificationTest(unittest.TestCase):
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
