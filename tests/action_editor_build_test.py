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
from action_editor.build import SOURCES, SCRIPT_SOURCE, build_document, preview_settings


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
    def test_regional_terrain_switching_and_scoped_ini(self):
        # Compare the editor against matrices emitted by the game's actual
        # camera math, including tilt/depth/aspect combinations.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, executable, golden = root / 'camera.c', root / 'camera', root / 'camera.json'
            source.write_text(r'''
#include <stdio.h>
#include "render/scene3d_math.h"
int main(void) {
  const float tilts[][2]={{0,0},{.15f,-.2f},{-.2f,.3f}};
  const float distances[]={3.25f,5};
  const float depths[]={-.45f,0,.15f};
  int first=1;
  printf("[");
  for(int a=0;a<2;a++)for(int t=0;t<3;t++)for(int d=0;d<2;d++)for(int z=0;z<3;z++) {
    int w=a?1920:1600,h=a?1080:1000;
    Scene3DCamera camera={tilts[t][0],tilts[t][1],distances[d],.4f};
    float matrix[16];Scene3D_BuildViewProjection(&camera,w,h,matrix);
    printf("%s{\"tiltX\":%.9g,\"tiltY\":%.9g,\"distance\":%.9g,\"width\":%d,\"height\":%d,\"depth\":%.9g,\"matrix\":[",
      first?"":",",camera.tilt_x,camera.tilt_y,camera.distance,w,h,depths[z]);
    first=0;
    for(int i=0;i<16;i++)printf("%s%.9g",i?",":"",matrix[i]);
    printf("]}");
  }
  printf("]");return 0;
}
''')
            subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'src'), str(source),
                            str(ROOT / 'src/render/scene3d_math.c'), '-lm', '-o', str(executable)], check=True)
            golden.write_bytes(subprocess.check_output([str(executable)]))
            subprocess.run(["node", str(ROOT / "tests/action_editor_terrain.test.mjs"),
                            "", str(golden)], check=True)

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
            self.assertEqual(len(scripts.bodies),
                             len(SCRIPT_SOURCE.findall((SOURCES / "editor.body.html").read_text())) + 2)
            program = ('const vm=require("node:vm");const context={window:{}};'
                       'vm.runInNewContext(JSON.parse(process.argv[1]),context);'
                       'process.stdout.write(JSON.stringify(context.window));')
            result = subprocess.check_output(
                ["node", "-e", program, json.dumps("\n".join(scripts.bodies[:2]))], text=True)
            self.assertEqual(json.loads(result), {
                "__ACTION_BG__": data, "__DIORAMA_LAYERS__": text,
                "__DIORAMA_LAYERS_NAME__": layers.name,
                "__ACTION_VIEW__": preview_settings(root / "settings.ini")})

    def test_preview_preferences_use_selected_camera_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = Path(directory) / "settings.ini"
            settings.write_text('extended_aspect = 16:10\npixel_aspect = Square pixels\n'
                                'diorama_camera_mode = Dynamic Cam\n'
                                'diorama_dyncam_baseline_distance_x100 = 325\n'
                                'diorama_dyncam_baseline_tilt_x_mrad = 0\n'
                                'diorama_dyncam_baseline_tilt_y_mrad = 25\n'
                                'diorama_distance_x100 = 550\ndiorama_tilt_x_mrad = 75\n')
            dynamic = preview_settings(settings)
            self.assertEqual(dynamic['distance'], 3.25)
            self.assertEqual(dynamic['pixelAspect'], 'square')
            self.assertEqual(dynamic['tiltX'], 0)
            self.assertEqual(dynamic['tiltY'], .025)
            settings.write_text(settings.read_text().replace('Dynamic Cam', 'Free Cam'))
            free = preview_settings(settings)
            self.assertEqual(free['distance'], 5.5)
            self.assertEqual(free['tiltX'], .075)
            self.assertEqual(free['cameraMode'], 'free')
            settings.write_text(settings.read_text() + 'pixel_aspect = 4:3 CRT\nextended_aspect = off\n')
            self.assertEqual(preview_settings(settings)['pixelAspect'], 'crt')
            self.assertEqual(preview_settings(settings)['aspect'], '4:3')


if __name__ == "__main__":
    unittest.main()
