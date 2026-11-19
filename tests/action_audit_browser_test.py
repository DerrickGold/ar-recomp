"""The audit artifact carries its data/renderer offline and identifies reports."""
import base64
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/action_editor'))
from build_browser import build_document


class Resources(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.external = []
        self.scripts = []
        self.active = False
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag in ('script', 'link', 'iframe', 'img', 'source'):
            for key in ('src', 'href'):
                if attrs.get(key):
                    self.external.append(attrs[key])
        if tag == 'script':
            self.active = True
            self.scripts.append('')

    def handle_endtag(self, tag):
        if tag == 'script':
            self.active = False

    def handle_data(self, text):
        if self.active:
            self.scripts[-1] += text


class AuditBrowserTest(unittest.TestCase):
    def test_gestures_and_phone_coordinate_mapping(self):
        subprocess.run(['node', str(ROOT / 'tests/action_audit_camera.test.mjs')], check=True)

    def test_offline_artifact_and_report_fingerprint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rooms, layers, wasm = (root / name for name in ('rooms.json', 'layers.ini', 'preview.wasm'))
            data = {'rooms': [], 'blobs': [], 'note': '</script>\n雪'}
            rooms.write_text(json.dumps(data))
            layers.write_text('; </script>\n[layers:01:01]\n')
            wasm.write_bytes(b'\0asm\x01\0\0\0')
            html = build_document(rooms, layers, wasm)
            parsed = Resources(html)
            self.assertEqual(parsed.external, [])
            self.assertEqual(len(parsed.scripts), 5)
            bundle = json.loads(re.fullmatch(r'window.__ACTION_AUDIT__=(.*);', parsed.scripts[0]).group(1))
            self.assertEqual(bundle['data'], data)
            self.assertEqual(bundle['scenery'], layers.read_text())
            self.assertEqual(base64.b64decode(bundle['wasm']), wasm.read_bytes())
            self.assertEqual(len(bundle['build']['fingerprint']), 16)
            self.assertIn('width=device-width', html)
            layers.write_text('[layers:01:01]\nbg1 = z:0.5\n')
            second = Resources(build_document(rooms, layers, wasm))
            rebuilt = json.loads(re.fullmatch(r'window.__ACTION_AUDIT__=(.*);', second.scripts[0]).group(1))
            self.assertNotEqual(bundle['build']['fingerprint'], rebuilt['build']['fingerprint'])
            self.assertEqual(bundle['build']['revision'], rebuilt['build']['revision'])


if __name__ == '__main__':
    unittest.main()
