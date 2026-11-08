"""Historical scalar witness plus optional actual production room fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('depth_audit', ROOT / 'tools/voxel_depth_audit.py')
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)
TARGETS = {(43, y) for y in (11, 13, 15, 17)}


class NativeDepthAuditTests(unittest.TestCase):
    def test_recorded_pre_fix_trunk_has_all_four_teeth(self):
        room = json.loads((ROOT / 'tests/fixtures/voxel-depth-northwall-before.json').read_text())
        high = [f for f in AUDIT.analyze_room(room)['findings'] if f['confidence'] == 'high']
        self.assertEqual(len(high), 1)
        self.assertEqual({(c['x'], c['y']) for c in high[0]['cells']}, TARGETS)
        self.assertEqual({c['collision'] for c in high[0]['cells']}, {0})
        self.assertTrue(any(c['collision'] == 15 for c in high[0]['witness_cells']))

    @unittest.skipUnless(os.environ.get('AR_VOXEL_DEPTH_FIXTURES'), 'requires locally exported native scenes')
    def test_current_production_trunk_is_clear_in_all_regions(self):
        directory = Path(os.environ['AR_VOXEL_DEPTH_FIXTURES'])
        binary = Path(os.environ.get('AR_VOXEL_DEPTH_BINARY', ROOT / 'build-tests/actraiser_voxel_depth_audit'))
        paths = [directory / str(region) / '0605.arscene' for region in range(3)]
        output = subprocess.check_output([str(binary), '--ini', str(ROOT / 'diorama-layers.ini'),
                                          *map(str, paths)], text=True)
        rooms = [json.loads(line) for line in output.splitlines()]
        self.assertEqual(len(rooms), 3)
        for room in rooms:
            with self.subTest(terrain=room['terrain']):
                selected = [c for c in room['cells'] if (c['x'], c['y']) in TARGETS]
                self.assertEqual(len(selected), 4)
                for cell in selected:
                    self.assertEqual(cell['front'], [0] * 4)
                    self.assertEqual(cell['back'], [.2] * 4)
                    self.assertEqual(cell['native_collision'], 0)
                findings = AUDIT.analyze_room(room)['findings']
                self.assertFalse(any((c['x'], c['y']) in TARGETS
                                     for finding in findings for c in finding['cells']))
        # An arbitrary editor INI must reach the same production resolver,
        # including negative map stamps and intentionally authored depths.
        source = rooms[0]
        bark = next(c for c in source['cells'] if (c['x'], c['y']) == (43, 11))
        words = ','.join(f'{w:04X}' for w in bark['words'])
        with tempfile.TemporaryDirectory() as directory:
            ini = Path(directory) / 'editor.ini'
            text = ('[layers:06:05]\n'
                    f"bg1-map = bounds:-1,0,{source['bounds'][2]},{source['bounds'][3]}\n"
                    f'bg1-stamp = cell:-1,0 metatile:91 words:{words} bands:0,0,0,0\n'
                    'voxel-cell = cell:43,11 front:.07 back:.18\n')
            ini.write_text(text)
            edited = json.loads(subprocess.check_output(
                [str(binary), '--ini', str(ini), str(paths[0])], text=True))
            self.assertEqual(ini.read_text(), text)
            cells = {(c['x'], c['y']): c for c in edited['cells']}
            stamp = cells[-1, 0]
            self.assertTrue(stamp['stamp'])
            self.assertEqual(stamp['native_id'], -1)
            self.assertEqual(stamp['id'], 0x91)
            self.assertEqual(stamp['collision'], 0)
            self.assertEqual(stamp['words'], bark['words'])
            self.assertEqual(stamp['art'], bark['art'])
            self.assertEqual(stamp['front'], [0] * 4)
            self.assertEqual(cells[43, 11]['front'], [.07] * 4)
            self.assertEqual(cells[43, 11]['back'], [.18] * 4)
            self.assertIn('authored_override', cells[43, 11]['flags'])
            self.assertEqual(cells[43, 12]['front'], [0] * 4)
            self.assertGreater(AUDIT.analyze_room(edited)['exclusions']['authored_override'], 0)


    @unittest.skipUnless(os.environ.get('AR_VOXEL_DEPTH_FIXTURES'), 'requires locally exported native scenes')
    def test_surface_inventory_retains_unclassified_and_specialized_routes(self):
        directory = Path(os.environ['AR_VOXEL_DEPTH_FIXTURES'])
        binary = Path(os.environ.get('AR_VOXEL_DEPTH_BINARY', ROOT / 'build-tests/actraiser_voxel_depth_audit'))
        command = [str(binary), '--surfaces', '--ini', str(ROOT / 'diorama-layers.ini')]
        paths = [directory / '0' / (room + '.arscene') for room in ('0201', '0606')]
        rooms = [json.loads(line) for line in subprocess.check_output(command + list(map(str, paths)), text=True).splitlines()]
        marsh, trunk = rooms
        inventory = marsh['surface_audit']
        self.assertFalse(inventory['complete_visual_review'])
        self.assertEqual(len(inventory['source_cells']), (marsh['bounds'][2]-marsh['bounds'][0]) *
                         (marsh['bounds'][3]-marsh['bounds'][1]))
        cells = [dict(zip(inventory['cell_fields'], row)) for row in inventory['source_cells']]
        material_at = {(c['x'], c['y']): inventory['materials'][c['material_id']] for c in cells}
        edges = [dict(zip(inventory['edge_fields'], row)) for row in inventory['edges']]
        unclassified = [e for e in edges if not e['side'] and
                        not material_at[e['x']//16, e['y']//16]['top_policy']]
        self.assertTrue(unclassified, 'No-policy top edges must remain inventoried, never skipped')
        behind = [e for e in edges if e['behind_span_valid']]
        self.assertTrue(behind, 'Native-high walking decks must include their behind-actor span')
        for edge in edges:
            for phase in ('native', 'behind', 'rear'):
                if edge[phase + '_span_valid']:
                    self.assertTrue(edge['physical_span_valid'])
                    self.assertLessEqual(edge['physical_near'], edge[phase + '_near'])
                    self.assertGreaterEqual(edge['physical_far'], edge[phase + '_far'])
            if edge['owned_art']:
                self.assertLess(edge['owned_art']-1, len(inventory['owned_art']))
        self.assertTrue(all(len(m['rgba555_hex']) == 1024 and len(m['pixels_hex']) == 256
                            for m in inventory['materials']))
        specialized = trunk['surface_audit']
        self.assertFalse(specialized['complete_visual_review'])
        self.assertTrue(specialized['source_cells'])
        self.assertTrue(specialized['trunk']['built'])
        self.assertEqual(len(specialized['trunk']['core_masks']), 3)
        self.assertEqual(len(specialized['trunk']['side_masks']), 5)
        self.assertTrue(any(int(mask, 16) for mask in specialized['trunk']['side_masks']))
        self.assertTrue(any(r['route'] == 'trunk' and r['status'] == 'mask_inventory_render_review_required'
                            for r in specialized['routes']))


if __name__ == '__main__':
    unittest.main()
