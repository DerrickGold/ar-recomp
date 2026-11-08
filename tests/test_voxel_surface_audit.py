"""Independent surface-audit controls; all artwork here is synthetic.

The first two shapes reproduce the *failure mechanisms* of the retained
Fillmore/Bloodpool reports. They do not copy native pixel art, tile IDs or
material policies. A detector must inventory unknown treatments, preserve
source openings, and distinguish a review lead from visual acceptance.
"""
from copy import deepcopy
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
from io import StringIO
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
# Match the CLI's script-directory import path when calling main() in-process.
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("voxel_surface_audit", ROOT / "tools/voxel_surface_audit.py")
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)
NORMALIZE_SPEC = importlib.util.spec_from_file_location(
    "voxel_surface_normalize", ROOT / "tools/voxel_audit/normalize.py")
NORMALIZE = importlib.util.module_from_spec(NORMALIZE_SPEC)
NORMALIZE_SPEC.loader.exec_module(NORMALIZE)


def pattern(width=8, height=4):
    # Opaque RGB555 values, with useful variation along *both* texture axes.
    return [0x8000 | (1 + x % 3) | ((y + 1) << 5)
            for y in range(height) for x in range(width)]


def surface(identity, x=0, *, mapping="assigned", orientation="top", policy=1,
            source=None, rendered=None, semantic="stone", route="generic"):
    source = pattern() if source is None else source
    rendered = list(source) if rendered is None and mapping == "assigned" else rendered
    return {
        "id": identity, "route": route, "orientation": orientation,
        "bounds": [x, 32, 8, 1], "source_art": source,
        "source_size": [8, len(source) // 8],
        "rendered_art": rendered,
        "rendered_size": [8, len(rendered) // 8] if rendered is not None else None,
        "mapping": mapping, "material_key": f"synthetic-{identity}",
        "top_policy": policy, "depth": [0, 0.2], "priority": 0,
        "colliding": True, "semantic": semantic,
    }


def scene(surfaces, *, route="generic", expected=None):
    return {
        "scene_id": "synthetic-room-us", "room": "synthetic", "terrain": 0,
        "surfaces": surfaces,
        "expected_routes": [{"route": route, "count": len(surfaces),
                             "status": "inventoried", "reason": "synthetic exact inventory"}],
        "expected_surface_count": len(surfaces) if expected is None else expected,
    }


def rules(result):
    return {finding["rule"] for finding in result["findings"]}


def fillmore_shape():
    # One connected platform: its first half accidentally lacks assignments.
    return scene([surface("a", 0, mapping="missing"),
                  surface("b", 8, mapping="missing"),
                  surface("c", 16), surface("d", 24)])


def bloodpool_shape():
    # Entire family lacks a policy. Existing-policy-only scans saw zero cells.
    result = scene([surface("beam-left", 0, mapping="native_extrusion", policy=0,
                            semantic="wood"),
                    surface("beam-right", 8, mapping="native_extrusion", policy=0,
                            semantic="wood")])
    return result


def hex_art(art):
    return "".join(f"{pixel:04x}" for pixel in art)


def exported_scene():
    """Minimal production-export shape; independently authored pixel values."""
    art = pattern(16, 16)
    edge = dict(x=0, y=0, length=8, side=0, near_rank=0, far_rank=2,
                priority=0, colliding=0, sample_x=0, sample_y=0, sample_size=16,
                surface_texture=1, owned_art=1, derived=1, native_x=0,
                soft=0, column=0, trunk=0, contour_skin=0,
                surface_top_material=0, fold_top=0, fold_bottom=0,
                flat_rear_join=0, native_actor_priority=0,
                native_near=0.0, native_far=0.2, native_span_valid=True)
    audit = {
        "cell_fields": ["x", "y", "native_id", "native_material_id", "material_id",
                        "edit_id", "authored_index"],
        "source_cells": [[0, 0, 0, 0, 0, -1, -1]],
        "materials": [{"key": "synthetic-rock", "rgba555_hex": hex_art(art),
                       "cutouts": [0] * 16, "top_policy": 1, "flags": 0,
                       "collision": 0}],
        "owned_art": [hex_art(art)], "pixel_edits": [],
        "edge_fields": list(edge), "edges": [list(edge.values())],
        "authored": [], "profile_surfaces": [], "patches": [], "water": [],
        "rear_fills": [], "gates": [], "waterfalls": [], "rear_trims": [],
        "routes": [{"route": "generic_edges", "items": 1,
                    "status": "inventoried_not_visually_accepted"}],
    }
    return {"room": "synthetic", "terrain": 0, "section": "room", "depth": 0.2,
            "cells": [{"x": 0, "y": 0, "semantic": 4}], "surface_audit": audit}


def set_edge(data, **fields):
    audit = data["surface_audit"]
    for key, value in fields.items():
        audit["edges"][0][audit["edge_fields"].index(key)] = value


class SurfaceAuditTests(unittest.TestCase):
    def test_analysis_does_not_mutate_inventory_or_artwork(self):
        data = fillmore_shape()
        before = deepcopy(data)
        AUDIT.analyze_scene(data)
        self.assertEqual(data, before)

    def test_half_textured_connected_platform_is_detected(self):
        result = AUDIT.analyze_scene(fillmore_shape())
        self.assertIn("mixed_adjacent_mapping", rules(result))
        self.assertEqual({s["id"] for s in result["surfaces"]}, {"a", "b", "c", "d"})

    def test_missing_policy_cannot_make_whole_wood_family_disappear(self):
        result = AUDIT.analyze_scene(bloodpool_shape())
        self.assertIn("untextured_source_detail", rules(result))
        self.assertEqual(len(result["surfaces"]), 2)
        self.assertTrue(all(s["disposition"] != "accepted" for s in result["surfaces"]))
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_cross_policy_boundary_does_not_split_visual_continuity(self):
        data = fillmore_shape()
        data["surfaces"][0]["top_policy"] = 0
        data["surfaces"][1]["top_policy"] = 0
        self.assertIn("mixed_adjacent_mapping", rules(AUDIT.analyze_scene(data)))

    def test_native_priority_and_depth_rank_do_not_whitelist_away_detail(self):
        data = bloodpool_shape()
        data["surfaces"][0]["priority"] = 1
        data["surfaces"][0]["depth"] = [0.05, 0.2]
        result = AUDIT.analyze_scene(data)
        self.assertIn("untextured_source_detail", rules(result))
        self.assertEqual(len(result["surfaces"]), 2)

    def test_assigned_texture_does_not_prove_source_detail_survived(self):
        data = scene([surface("flattened", rendered=[0x8421] * 32)])
        self.assertIn("source_detail_flattened", rules(AUDIT.analyze_scene(data)))

    def test_side_faces_are_subject_to_the_same_detail_check(self):
        data = scene([surface("side", orientation="left", rendered=[0x8421] * 32)])
        self.assertIn("source_detail_flattened", rules(AUDIT.analyze_scene(data)))

    def test_uniform_water_is_not_automatically_retextured_or_accepted(self):
        data = scene([surface("water", source=[0x9001] * 32,
                              rendered=[0x9001] * 32, semantic="water")])
        result = AUDIT.analyze_scene(data)
        self.assertNotIn("source_detail_flattened", rules(result))
        self.assertNotIn("untextured_source_detail", rules(result))
        self.assertEqual(result["surfaces"][0]["disposition"], "pending")
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_uniform_fog_native_extrusion_is_not_misclassified_as_missing_art(self):
        data = scene([surface("fog", source=[0x8A21] * 32,
                              mapping="native_extrusion", policy=0, semantic="fog")])
        result = AUDIT.analyze_scene(data)
        self.assertNotIn("untextured_source_detail", rules(result))
        self.assertEqual(len(result["surfaces"]), 1)
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_coherent_three_row_native_band_keeps_its_limited_detail(self):
        source = [0x8001] * 8 + [0x8021] * 8 + [0x8401] * 8
        # Stretching a genuinely narrow native band does not invent a defect.
        rendered = [0x8001] * 40 + [0x8021] * 48 + [0x8401] * 40
        result = AUDIT.analyze_scene(scene([surface("native-band", source=source,
                                                    rendered=rendered)]))
        self.assertNotIn("source_detail_flattened", rules(result))
        self.assertEqual(result["surfaces"][0]["disposition"], "pending")

    def test_non_touching_objects_do_not_create_mixed_adjacency(self):
        result = AUDIT.analyze_scene(scene([surface("a", mapping="missing"),
                                           surface("b", 32)]))
        self.assertNotIn("mixed_adjacent_mapping", rules(result))

    def test_vertical_side_faces_receive_the_same_continuity_check(self):
        for orientation in ("left", "right"):
            with self.subTest(orientation=orientation):
                a = surface("upper", mapping="native_extrusion", orientation=orientation)
                b = surface("lower", orientation=orientation)
                a["bounds"], b["bounds"] = [32, 0, 1, 8], [32, 8, 1, 8]
                self.assertIn("mixed_adjacent_mapping", rules(AUDIT.analyze_scene(scene([a, b]))))

    def test_legitimate_source_holes_are_not_filled_by_the_audit(self):
        art = pattern()
        for y in range(4):
            art[y * 8 + 3] = 0
        data = scene([surface("post-gap", source=art, rendered=list(art))])
        before = deepcopy(data)
        result = AUDIT.analyze_scene(data)
        self.assertEqual(data, before)
        self.assertNotIn("source_detail_flattened", rules(result))
        self.assertEqual(len(result["surfaces"]), 1)

    def test_authored_and_curved_surfaces_are_retained_for_review(self):
        for route in ("authored", "trunk", "profile", "ground_cover"):
            with self.subTest(route=route):
                data = scene([surface(route, route=route)], route=route)
                result = AUDIT.analyze_scene(data)
                self.assertEqual(len(result["surfaces"]), 1)
                self.assertEqual(result["surfaces"][0]["disposition"], "pending")
                self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_absent_expected_geometry_is_a_finding_not_an_empty_pass(self):
        result = AUDIT.analyze_scene(scene([], expected=1))
        self.assertIn("missing_expected_surfaces", rules(result))
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_pending_specialized_route_prevents_empty_scene_acceptance(self):
        data = scene([])
        data["expected_routes"] = [{"route": "tree_interior", "count": None,
                                     "status": "pending", "reason": "specialized renderer"}]
        result = AUDIT.analyze_scene(data)
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_unknown_route_count_cannot_substantiate_completeness(self):
        data = scene([])
        data["expected_routes"][0]["count"] = None
        self.assertFalse(AUDIT.summarize([AUDIT.analyze_scene(data)])["complete"])

    def test_missing_source_art_is_unresolved_even_when_texture_is_assigned(self):
        data = scene([surface("unresolved")])
        data["surfaces"][0]["source_art"] = None
        data["surfaces"][0]["source_size"] = None
        result = AUDIT.analyze_scene(data)
        self.assertEqual(result["surfaces"][0]["disposition"], "pending")
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_missing_source_does_not_hide_an_explicit_missing_mapping(self):
        data = scene([surface("missing", mapping="missing")])
        data["surfaces"][0]["source_art"] = None
        data["surfaces"][0]["source_size"] = None
        result = AUDIT.analyze_scene(data)
        self.assertIn("missing_surface_mapping", rules(result))
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_assigned_detailed_art_still_requires_continuity_review(self):
        # Reordering rows retains color/entropy statistics. An assignment and
        # a texture-complexity check cannot alone establish correct grain.
        art = pattern()
        rendered = art[16:] + art[:16]
        result = AUDIT.analyze_scene(scene([surface("wrong-phase", rendered=rendered)]))
        self.assertEqual(result["surfaces"][0]["disposition"], "pending")
        self.assertFalse(AUDIT.summarize([result])["complete"])

    def test_surface_order_does_not_change_detected_rules(self):
        data = fillmore_shape()
        expected = rules(AUDIT.analyze_scene(data))
        data["surfaces"].reverse()
        self.assertEqual(rules(AUDIT.analyze_scene(data)), expected)

    def test_duplicate_ids_cannot_reduce_the_coverage_denominator(self):
        data = scene([surface("duplicate"), surface("duplicate", 8)])
        with self.assertRaises(ValueError):
            AUDIT.analyze_scene(data)

    def test_art_size_mismatch_is_rejected(self):
        data = scene([surface("invalid")])
        data["surfaces"][0]["rendered_size"] = [8, 3]
        with self.assertRaises(ValueError):
            AUDIT.analyze_scene(data)

    def test_nonfinite_geometry_is_rejected(self):
        data = scene([surface("invalid")])
        data["surfaces"][0]["depth"][0] = float("nan")
        with self.assertRaises(ValueError):
            AUDIT.analyze_scene(data)

    def test_cli_refuses_to_overwrite_input_or_place_reports_in_input_tree(self):
        for kind in ("same_file", "symlink_alias", "inside_tree"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                base = Path(directory)
                source = base / "scene.json"
                original = (json.dumps(fillmore_shape()) + "\n").encode()
                source.write_bytes(original)
                if kind == "same_file":
                    input_path, output_path = source, source
                elif kind == "symlink_alias":
                    output_path = base / "alias.json"
                    output_path.symlink_to(source)
                    input_path = source
                else:
                    input_path, output_path = base, base / "report.json"
                with patch.object(sys, "argv", ["voxel_surface_audit.py", "--input",
                                                str(input_path), "--output", str(output_path)]):
                    with redirect_stderr(StringIO()), self.assertRaises(SystemExit) as failure:
                        AUDIT.main()
                self.assertEqual(failure.exception.code, 2)
                self.assertEqual(source.read_bytes(), original)
                if kind == "inside_tree":
                    self.assertFalse(output_path.exists())

    def test_invalid_input_does_not_replace_an_existing_report(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, report = base / "scene.json", base / "report.json"
            source.write_text("{invalid json\n")
            previous = b"previous verified report\n"
            report.write_bytes(previous)
            with patch.object(sys, "argv", ["voxel_surface_audit.py", "--input",
                                            str(source), "--output", str(report)]):
                with redirect_stdout(StringIO()), self.assertRaises(json.JSONDecodeError):
                    AUDIT.main()
            self.assertEqual(report.read_bytes(), previous)


class SurfaceExportAdapterTests(unittest.TestCase):
    def test_live_inward_reads_only_emitted_columns_and_depth_rows(self):
        for side, length, width in ((0, 2, 2), (1, 2, 1)):
            with self.subTest(side=side):
                data = exported_scene()
                pixels = [0xffff] * 256  # Unused neighbor sentinel.
                for y in range(3):
                    for x in range(width):
                        pixels[y*16+x] = 0x8001+y
                data['surface_audit']['materials'][0]['rgba555_hex'] = hex_art(pixels)
                set_edge(data, side=side, length=length, sample_size=3,
                         sample_x=0, sample_y=0, owned_art=0, surface_texture=3)
                normalized = NORMALIZE.normalize_export(data)
                surface = normalized['surfaces'][0]
                self.assertEqual(surface['rendered_size'], [width, 3])
                self.assertEqual(surface['rendered_art'],
                                 [0x8001+y for y in range(3) for x in range(width)])
                self.assertNotIn('source_detail_flattened', rules(AUDIT.analyze_scene(normalized)))

    def test_unclassified_edges_are_retained_and_source_is_immutable(self):
        data = exported_scene()
        data["surface_audit"]["materials"][0]["top_policy"] = 0
        set_edge(data, sample_size=0, surface_texture=0, owned_art=0)
        before = deepcopy(data)
        normalized = NORMALIZE.normalize_export(data, "shipped")
        self.assertEqual(data, before)
        self.assertEqual(len(normalized["surfaces"]), 1)
        self.assertEqual(normalized["surfaces"][0]["mapping"], "native_extrusion")
        self.assertIn("untextured_source_detail", rules(AUDIT.analyze_scene(normalized)))

    def test_material_cutouts_and_explicit_pixel_edits_reach_effective_source(self):
        data = exported_scene()
        audit = data["surface_audit"]
        audit["materials"][0]["cutouts"][0] = 0xA000  # x0 and x2.
        audit["pixel_edits"] = [{"transparent": [0x4000] + [0] * 15,
                                 "black": [0x8000] + [0] * 15}]
        audit["source_cells"][0][5] = 0
        normalized = NORMALIZE.normalize_export(data)
        art = normalized["surfaces"][0]["source_art"]
        self.assertEqual(art[:3], [0x8000, 0, 0])
        self.assertEqual(art[3:], pattern(16, 16)[3:])

    def test_owned_edge_art_indices_are_one_based(self):
        data = exported_scene()
        audit = data["surface_audit"]
        first, second = [0x8001] * 256, [0x8421] * 256
        audit["owned_art"] = [hex_art(first), hex_art(second)]
        for slot, expected in ((1, first), (2, second)):
            with self.subTest(slot=slot):
                set_edge(data, owned_art=slot)
                self.assertEqual(NORMALIZE.normalize_export(data)["surfaces"][0]["rendered_art"], expected)

    def test_rear_matte_mask_does_not_erase_front_art(self):
        data = exported_scene()
        audit = data["surface_audit"]
        audit["materials"][0]["flags"] = 2097152  # Exported RearMatte ownership contract.
        audit["materials"][0]["cutouts"][0] = 0xC000
        audit["pixel_edits"] = [{"transparent": [0x4000] + [0] * 15,
                                 "black": [0] * 16}]
        audit["source_cells"][0][5] = 0
        art = NORMALIZE.normalize_export(data)["surfaces"][0]["source_art"]
        self.assertEqual(art[0], pattern(16, 16)[0])
        self.assertEqual(art[1], 0)  # Explicit erasure still wins over native matte ownership.

    def test_authored_front_and_top_art_use_zero_based_owned_indices(self):
        data = exported_scene()
        audit = data["surface_audit"]
        front, top = [0x8004] * 256, [0x8427] * 256
        audit["owned_art"] = [hex_art(front), hex_art(top)]
        audit["source_cells"][0][6] = 0
        audit["edges"] = []
        audit["authored"] = [{"x": 0, "y": 0, "width": 8, "offset_x": 2,
                              "surface": True, "automatic_art": 0, "faces": 63,
                              "front": [0] * 4, "back": [0.2] * 4,
                              "priority": [0] * 4, "art_ids": [0, 1, 0, 0, 0, 0, 0],
                              "samples": [[0, 0, 16, 0, 0] for _ in range(7)],
                              "contours": [[0, 0, 8, 0]]}]
        result = NORMALIZE.normalize_export(data)["surfaces"][0]
        self.assertEqual(result["source_art"], front)
        self.assertEqual(result["rendered_art"], top)
        self.assertEqual(result["source_bounds"], [0, 0, 8, 1])
        self.assertEqual(result["bounds"], [6, 0, 4, 1])
        self.assertEqual(result["world_bounds_status"], "requires_renderer_transform_and_bend")

    def test_high_priority_behind_pass_survives_invalid_native_pass(self):
        data = exported_scene()
        audit = data["surface_audit"]
        set_edge(data, priority=1, native_near=0.0, native_far=0.0, native_span_valid=False)
        for key, value in (("physical_near", 0.05), ("physical_far", 0.2), ("physical_span_valid", True)):
            audit["edge_fields"].append(key)
            audit["edges"][0].append(value)
        result = NORMALIZE.normalize_export(data)["surfaces"]
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["depth"], [0.05, 0.2])

    def test_trunk_mask_coverage_counts_bits_not_nonzero_bytes(self):
        data = exported_scene()
        data["surface_audit"]["edges"] = []
        data["surface_audit"]["trunk"] = {
            "core_masks": ["ff01"], "side_masks": ["80"],
            "side_width": 16, "height": 1, "wall_rgba": "ff001122",
        }
        result = NORMALIZE.normalize_export(data)["surfaces"]
        self.assertEqual([s["mask_pixels"] for s in result], [9, 1])

    def test_long_edge_is_split_without_losing_either_material_cell(self):
        data = exported_scene()
        audit = data["surface_audit"]
        second = deepcopy(audit["materials"][0])
        second["key"], second["top_policy"] = "synthetic-unclassified", 0
        audit["materials"].append(second)
        audit["source_cells"].append([1, 0, 1, 1, 1, -1, -1])
        set_edge(data, x=8, length=24)
        normalized = NORMALIZE.normalize_export(data)
        self.assertEqual(len(normalized["surfaces"]), 2)
        self.assertEqual([s["bounds"] for s in normalized["surfaces"]], [[8, 0, 8, 1], [16, 0, 16, 1]])
        self.assertEqual([s["material_key"] for s in normalized["surfaces"]],
                         ["synthetic-rock", "synthetic-unclassified"])
        self.assertEqual(sum(s["source_length"] for s in normalized["surfaces"]), 24)

    def test_no_emitted_edges_does_not_erase_specialized_pending_routes(self):
        data = exported_scene()
        data["surface_audit"]["edges"] = []
        data["surface_audit"]["routes"] = [{"route": "ground_cover_and_effect_receivers",
                                            "status": "renderer_inventory_required"}]
        normalized = NORMALIZE.normalize_export(data)
        self.assertEqual(normalized["compiled_counts"]["source_cells"], 1)
        routes = [r for r in normalized["expected_routes"]
                  if r["route"] == "ground_cover_and_effect_receivers"]
        self.assertEqual(len(routes), 1)
        self.assertEqual(routes[0]["status"], "pending")
        self.assertFalse(AUDIT.summarize([AUDIT.analyze_scene(normalized)])["complete"])

    def test_source_collision_floor_is_inventoried_without_any_emitted_edge(self):
        data = exported_scene()
        audit = data["surface_audit"]
        audit["edges"] = []
        audit["materials"][0]["collision"] = 15
        audit["materials"][0]["top_policy"] = 0
        normalized = NORMALIZE.normalize_export(data)
        witnesses = [s for s in normalized["surfaces"] if s["route"] == "native_floor_witness"]
        self.assertTrue(witnesses)
        self.assertTrue(all(s["source_art"] for s in witnesses))
        self.assertEqual(normalized["compiled_counts"]["edges"], 0)
        self.assertFalse(AUDIT.summarize([AUDIT.analyze_scene(normalized)])["complete"])

    def test_edited_collision_keeps_native_and_effective_denominators_separate(self):
        data = exported_scene()
        audit = data['surface_audit']
        audit['edges'] = []
        original = audit['materials'][0]
        original['collision'] = 15
        edited = deepcopy(original)
        edited.update(collision=0, key='edited-air', rgba555_hex=hex_art([0x8421]*256))
        audit['materials'].append(edited)
        audit['source_cells'][0][4] = 1
        normalized = NORMALIZE.normalize_export(data)
        native = [s for s in normalized['surfaces'] if s['route']=='native_floor_witness']
        effective = [s for s in normalized['surfaces'] if s['route']=='effective_floor_witness']
        self.assertEqual(len(native),2)
        self.assertEqual(effective,[])
        self.assertEqual(native[0]['source_art'],pattern(16,16))
        original['collision'],edited['collision'] = 0,15
        normalized = NORMALIZE.normalize_export(data)
        native = [s for s in normalized['surfaces'] if s['route']=='native_floor_witness']
        effective = [s for s in normalized['surfaces'] if s['route']=='effective_floor_witness']
        self.assertEqual(native,[])
        self.assertEqual(len(effective),2)
        self.assertEqual(effective[0]['source_art'],[0x8421]*256)


if __name__ == "__main__":
    unittest.main()
