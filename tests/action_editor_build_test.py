"""The action editor remains a single, offline artifact with lossless input data."""
from html.parser import HTMLParser
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from action_editor.build import SOURCES, SCRIPT_SOURCE, build_document


class Scripts(HTMLParser):
    def __init__(self, html):
        super().__init__()
        self.sources = []
        self.bodies = []
        self.active = False
        self.feed(html)

    def handle_starttag(self, tag, attrs):
        if tag == "script":
            self.sources.append(dict(attrs).get("src"))
            self.bodies.append("")
            self.active = True

    def handle_endtag(self, tag):
        if tag == "script":
            self.active = False

    def handle_data(self, data):
        if self.active:
            self.bodies[-1] += data


class ActionEditorBuildTest(unittest.TestCase):
    def test_bundle_has_no_external_scripts_and_preserves_source_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rooms = root / "rooms.json"
            rooms.write_text('{"rooms":[],"blobs":[]}')
            scripts = Scripts(build_document(rooms, root / "absent.ini"))
            names = SCRIPT_SOURCE.findall((SOURCES / "editor.body.html").read_text())
            self.assertEqual(len(scripts.bodies), len(names) + 2)
            self.assertTrue(all(source is None for source in scripts.sources))
            self.assertEqual(scripts.bodies[2:], [(SOURCES / name).read_text() for name in names])
            self.assertIn('window.__DIORAMA_LAYERS__="";', scripts.bodies[1])

    def test_embedded_json_round_trips_unicode_and_script_endings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rooms, layers = root / "rooms.json", root / "层.ini"
            data = {"rooms": [{"name": "</ScRiPt><script>oops</script>"}], "blobs": []}
            text = '# セーブ </script> <!-- <script>\n[layers:01:00]\nbg1 = z:0.5\n'
            rooms.write_text(json.dumps(data))
            layers.write_text(text)
            scripts = Scripts(build_document(rooms, layers))
            self.assertEqual(len(scripts.bodies), 8)
            program = ('const vm=require("node:vm");const context={window:{}};'
                       'vm.runInNewContext(JSON.parse(process.argv[1]),context);'
                       'process.stdout.write(JSON.stringify(context.window));')
            result = subprocess.check_output(
                ["node", "-e", program, json.dumps("\n".join(scripts.bodies[:2]))], text=True)
            self.assertEqual(json.loads(result), {
                "__ACTION_BG__": data, "__DIORAMA_LAYERS__": text,
                "__DIORAMA_LAYERS_NAME__": layers.name})


if __name__ == "__main__":
    unittest.main()
