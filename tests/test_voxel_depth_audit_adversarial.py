"""Adversarial checks for the headless audit, not renderer policy fixtures.

The small synthetic layout follows the shape of the Northwall collision-variant
failure. The tool owner's separate recorded witness/integration test supplies
the actual production-compiler evidence; these cases prevent heuristics from
turning deliberate layering or editor work into high-confidence defects.
"""

from copy import deepcopy
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("voxel_depth_audit", ROOT / "tools/voxel_depth_audit.py")
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


def cell(x, y, front=0.0, *, body=False, collision=15):
    return {
        "x": x, "y": y,
        "art": "85920776068fedbf" if body else "1515793fe212801a",
        "native_id": 145 if body and not collision else 202 if body else 194,
        "id": 145 if body and not collision else 202 if body else 194,
        "native_collision": collision, "collision": collision, "priority": 0,
        "front": [front] * 4, "back": [0.2] * 4,
        "borders": [0xFFFF] * 4,
        # The bad policy splits the two apparent pieces into different groups.
        "semantic": 3 if body and not collision else 4,
        "flags": [], "authored_fields": 0, "stamp": False, "pixel_edit": False,
        "words": [0x10C8, 0x10C9, 0x10D8, 0x10D9],
    }


def teeth():
    cells = [cell(43, 9, body=True)]
    for y in range(10, 19):
        cells.append(cell(43, y, 0.15 if y & 1 else 0.0,
                          body=bool(y & 1), collision=0 if y & 1 else 15))
    return {"room": "0605", "terrain": 0, "section": "room", "status": "ok",
            "depth": 0.2, "cells": cells}


def bad_cells(room):
    return [c for c in room["cells"] if c["collision"] == 0]


def high(result):
    return [f for f in result["findings"] if f["confidence"] == "high"]


def positions(findings, field="cells"):
    return {(c["x"], c["y"]) for finding in findings for c in finding[field]}


def stable_findings(result):
    return sorted((f["confidence"], f["rule"],
                   tuple(sorted((c["x"], c["y"]) for c in f["cells"])),
                   tuple(sorted((c["x"], c["y"]) for c in f["witness_cells"])))
                  for f in result["findings"])


class DepthAuditAdversarialTests(unittest.TestCase):
    def test_analysis_does_not_rewrite_input_records(self):
        room = teeth()
        original = deepcopy(room)
        AUDIT.analyze_room(room)
        self.assertEqual(room, original)

    def test_collision_variant_teeth_are_one_actionable_cluster(self):
        result = AUDIT.analyze_room(teeth())
        self.assertEqual(len(high(result)), 1)
        self.assertEqual(positions(high(result)), {(43, y) for y in (11, 13, 15, 17)})
        self.assertIn((43, 9), positions(high(result), "witness_cells"))

    def test_same_art_collision_variants_at_same_depth_are_clean(self):
        room = teeth()
        for c in bad_cells(room):
            c["front"] = [0.0] * 4
        self.assertEqual(AUDIT.analyze_room(room)["findings"], [])

    def test_semantic_group_mismatch_does_not_hide_the_policy_error(self):
        room = teeth()
        self.assertNotEqual(room["cells"][0]["semantic"], bad_cells(room)[0]["semantic"])
        self.assertEqual(len(high(AUDIT.analyze_room(room))), 1)

    def test_native_priority_changes_are_not_high_confidence(self):
        room = teeth()
        for c in bad_cells(room):
            c["priority"] = 15
            c["words"] = [w | 0x2000 for w in c["words"]]
        self.assertFalse(high(AUDIT.analyze_room(room)))

    def test_non_touching_art_is_not_a_connected_body(self):
        room = teeth()
        for c in bad_cells(room):
            c["borders"][0] = c["borders"][2] = 0
        self.assertFalse(high(AUDIT.analyze_room(room)))

    def test_remote_same_art_reuse_is_review_only(self):
        room = teeth()
        room["cells"][0]["x"] = 200
        room["cells"][0]["y"] = 200
        result = AUDIT.analyze_room(room)
        self.assertFalse(high(result))
        self.assertTrue(any(f["confidence"] == "review" for f in result["findings"]))

    def test_distant_alias_across_a_long_ground_body_is_review_only(self):
        room = teeth()
        # One huge connected floor is not evidence that distant reused artwork
        # belongs to this repeated local object. Only the art alias moves away.
        room["cells"][0]["art"] = "1515793fe212801a"
        room["cells"].extend(cell(x, 9) for x in range(44, 143))
        room["cells"].append(cell(143, 9, body=True))
        result = AUDIT.analyze_room(room)
        self.assertFalse(high(result))
        self.assertTrue(any(f["confidence"] == "review" for f in result["findings"]))

    def test_one_tooth_is_review_only(self):
        room = teeth()
        room["cells"] = room["cells"][:4]
        result = AUDIT.analyze_room(room)
        self.assertFalse(high(result))
        self.assertTrue(any(f["confidence"] == "review" for f in result["findings"]))

    def test_two_deliberate_tiers_are_not_teeth(self):
        room = teeth()
        room["cells"] = [cell(43, y, 0.0 if y < 14 else 0.15,
                               body=True, collision=15 if y < 14 else 0)
                         for y in range(9, 19)]
        self.assertFalse(high(AUDIT.analyze_room(room)))

    def test_different_back_attachment_is_not_the_same_depth_pattern(self):
        room = teeth()
        for c in bad_cells(room):
            c["back"] = [0.17] * 4
        self.assertFalse(high(AUDIT.analyze_room(room)))

    def test_partial_quadrant_slopes_are_not_averaged_into_teeth(self):
        room = teeth()
        for c in bad_cells(room):
            c["front"] = [0.0, 0.15, 0.15, 0.0]
        self.assertFalse(high(AUDIT.analyze_room(room)))

    def test_sub_threshold_float_noise_is_not_a_depth_break(self):
        room = teeth()
        for c in bad_cells(room):
            c["front"] = [0.000001] * 4
        self.assertFalse(high(AUDIT.analyze_room(room)))

    def test_explicit_voxel_edits_are_intentional(self):
        room = teeth()
        for c in bad_cells(room):
            c["authored_fields"] = 1
        result = AUDIT.analyze_room(room)
        self.assertFalse(high(result))
        self.assertGreater(sum(result["exclusions"].values()), 0)

    def test_complex_compiler_owners_are_explained_exclusions(self):
        for reason in ("trunk_curve", "column_curve", "pixel_recess", "gate",
                       "water", "side_profile", "authored_transform", "authored_bend"):
            with self.subTest(reason=reason):
                room = teeth()
                for c in bad_cells(room):
                    c["flags"] = [reason]
                result = AUDIT.analyze_room(room)
                self.assertFalse(high(result))
                self.assertGreater(sum(result["exclusions"].values()), 0)

    def test_negative_stamped_bounds_keep_the_same_detection(self):
        room = teeth()
        for c in room["cells"]:
            c["x"] -= 50
            c["y"] -= 20
            c["stamp"] = True
        result = AUDIT.analyze_room(room)
        self.assertEqual(len(high(result)), 1)
        self.assertEqual(positions(high(result)), {(-7, y) for y in (-9, -7, -5, -3)})

    def test_cell_order_and_metatile_numbers_do_not_define_the_result(self):
        expected = AUDIT.analyze_room(teeth())
        room = teeth()
        room["cells"].reverse()
        for c in room["cells"]:
            c["id"] = (c["id"] + 37) & 255
            c["native_id"] = (c["native_id"] + 37) & 255
        self.assertEqual(stable_findings(AUDIT.analyze_room(room)), stable_findings(expected))

    def test_disconnected_repeated_objects_remain_separate_clusters(self):
        room = teeth()
        shifted = deepcopy(room["cells"])
        for c in shifted:
            c["x"] += 100
        room["cells"].extend(shifted)
        result = AUDIT.analyze_room(room)
        self.assertEqual(len(high(result)), 2)
        self.assertEqual(positions(high(result)),
                         {(x, y) for x in (43, 143) for y in (11, 13, 15, 17)})

    def test_cli_refuses_to_overwrite_its_export_input_or_shared_outputs(self):
        for collision in ("input_as_raw", "input_as_report", "shared_outputs"):
            with self.subTest(collision=collision), tempfile.TemporaryDirectory() as directory:
                base = Path(directory)
                source, report, raw = base / "source.jsonl", base / "report.json", base / "raw.jsonl"
                original = (json.dumps(teeth()) + "\n").encode()
                source.write_bytes(original)
                old_output = b"existing unrelated output\n"
                report.write_bytes(old_output)
                if collision == "input_as_raw":
                    raw = source
                elif collision == "input_as_report":
                    report = source
                else:
                    raw = report
                args = ["--from-export", str(source), "--output", str(report),
                        "--raw-output", str(raw), "--summary-limit", "0"]
                with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
                    status = AUDIT.main(args)
                self.assertNotEqual(status, 0)
                self.assertEqual(source.read_bytes(), original)
                if report != source:
                    self.assertEqual(report.read_bytes(), old_output)

    def test_cli_refuses_hard_link_output_alias(self):
        for collision in ("input_as_raw", "input_as_report", "shared_outputs"):
            with self.subTest(collision=collision), tempfile.TemporaryDirectory() as directory:
                base = Path(directory)
                source, report, raw = base / "source.jsonl", base / "report.json", base / "raw.jsonl"
                original = (json.dumps(teeth()) + "\n").encode()
                source.write_bytes(original)
                old_output = b"existing unrelated output\n"
                report.write_bytes(old_output)
                if collision == "input_as_raw":
                    os.link(source, raw)
                elif collision == "input_as_report":
                    report.unlink()
                    os.link(source, report)
                else:
                    os.link(report, raw)
                with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
                    status = AUDIT.main(["--from-export", str(source), "--output", str(report),
                                         "--raw-output", str(raw)])
                self.assertNotEqual(status, 0)
                self.assertEqual(source.read_bytes(), original)
                if collision != "input_as_report":
                    self.assertEqual(report.read_bytes(), old_output)


if __name__ == "__main__":
    unittest.main()
