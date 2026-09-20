#!/usr/bin/env python3
"""Actual SDL font-stack author-report integration, without ROMs or a window."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_language_fonts import BUILTIN_FONT, check_pack_fonts, main
from language_pack_v1 import manifest_text

PROBE = Path(sys.argv.pop(1)).resolve()
RASTER_TEST = Path(sys.argv.pop(1)).resolve()
JP_FONT = ROOT / 'tests/fixtures/unicode-fonts/fonts/NotoSansJP-Bold.otf'


class FontCoverageTest(unittest.TestCase):
    def test_runtime_warnings_are_bounded_and_reset(self):
        result = subprocess.run([str(RASTER_TEST), '--missing-glyph-warnings'],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count(' has no glyph for U+'), 128)
        self.assertEqual(result.stderr.count('warnings for font stack'), 2)
        self.assertEqual(result.stderr.count('no glyph for U+F0000 ('), 2)
        self.assertEqual(result.stderr.count('no glyph for U+F003F ('), 2)
        self.assertNotIn('no glyph for U+F0040 (', result.stderr)
        # A runtime warning must not send a player to development tooling.
        self.assertNotIn('check_language_fonts', result.stderr)
        # Nor to a font-management panel that the shipping editor lacks.
        self.assertNotIn('under Fonts', result.stderr)
        self.assertIn("affected role's fallback list", result.stderr)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ar-font-coverage-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'text').mkdir()
        self.manifest = manifest_text({
            'id': 'test.font-coverage', 'locale': 'en-US', 'name': 'Font test',
            'autonym': 'Test', 'author': 'Test', 'license': 'MIT',
            'direction': 'ltr', 'target': 'us-runtime', 'source_profile': 'us',
            'coverage': 'partial'})
        (self.root / 'pack.ini').write_text(self.manifest, encoding='utf-8')
        (self.root / 'text/source.artext').write_text(
            '# Not text: \U0010ffff\n:: author.sample\n'
            'Café e\u0301 日 日 {master_name}\n@end\n', encoding='utf-8')

    def test_missing_and_fallback_with_locations(self):
        report = check_pack_fonts(self.root, PROBE)
        self.assertFalse(report['scalar_coverage_complete'])
        self.assertEqual([m['codepoint'] for m in report['missing']], ['U+65E5'])
        self.assertEqual(report['missing'][0]['locations'], [{
            'source': 'text/source.artext', 'message_id': 'author.sample', 'line': 3}])
        self.assertEqual(report['dynamic_placeholders_not_resolved'], ['master_name'])
        shutil.copyfile(JP_FONT, self.root / 'japanese.otf')
        (self.root / 'pack.ini').write_text(self.manifest.replace(
            '[scripts]', 'fallback = japanese.otf\n\n[scripts]'), encoding='utf-8')
        report = check_pack_fonts(self.root, PROBE)
        self.assertTrue(report['scalar_coverage_complete'])
        self.assertEqual(len(report['fonts']), 2)
        self.assertEqual(len(report['fonts'][0]['sha256']), 64)

    def test_samples_and_nonrendering_scalars(self):
        report = check_pack_fonts(self.root, PROBE, samples=[
            '\U0010ffff\u200d\ufe0f\U000e0100\u034f\ufffc\t\n'])
        self.assertEqual([m['codepoint'] for m in report['missing']],
                         ['U+65E5', 'U+10FFFF'])
        self.assertEqual(report['missing'][1]['locations'][0]['source'], '<sample:1>')

    def test_cli_exit_and_errors(self):
        output = self.root / 'report.json'
        self.assertEqual(main(['--pack', str(self.root), '--probe', str(PROBE),
                               '--out', str(output)]), 1)
        self.assertIn('U+65E5', output.read_text(encoding='utf-8'))
        with self.assertRaises(ValueError):
            check_pack_fonts(self.root, self.root / 'missing-probe')
        with self.assertRaises(ValueError):
            check_pack_fonts(self.root, PROBE, self.root / 'missing-font')
        bad_font = self.root / 'bad.ttf'
        bad_font.write_bytes(b'not a font')
        with self.assertRaisesRegex(ValueError, 'font backend failed'):
            check_pack_fonts(self.root, PROBE, bad_font)

    def test_path_escape_and_scalar_protocol(self):
        outside = self.root.parent / 'outside.ttf'
        (self.root / 'escape.ttf').symlink_to(outside)
        (self.root / 'pack.ini').write_text(self.manifest.replace(
            'builtin:actraiser-sans', 'escape.ttf'), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'escapes pack directory'):
            check_pack_fonts(self.root, PROBE)
        for invalid in ('D800\n', '110000\n', '+0041\n', '0x41\n', '0000000\n'):
            result = subprocess.run([str(PROBE), str(BUILTIN_FONT)], input=invalid,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 2, invalid)


if __name__ == '__main__':
    unittest.main()
