"""The metadata validator distinguishes authored arcs from plane transitions."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from sim3d.metadata_checks import (
    MetadataValidator, effect_anchor_valid, is_ballistic_effect, is_ballistic_object,
)
from sim3d.artifacts import changed_pixel_counts
from sim3d.checkpoints import check_stage_pinning, load_manifest
from sim3d.metadata import read_metadata
from sim3d.metadata_report import validate_expectations


class BallisticMetadataTest(unittest.TestCase):
    def setUp(self):
        self.source = dict(tier=1, type=0x0E01, composition=0xE7D0,
                           record=0x0F0C, x=144, y=128)
        self.effect = dict(kind=7, kind_name="volcano_fireball", record=0x0F0C,
                           composition=0xE7D0, world=[145, 160])
        self.obj = dict(tier=1, record=0x0F0C, composition=0xE7D0, source_index=0)

    def test_airborne_arc_can_move_off_native_anchor(self):
        for composition in (0xE7D0, 0xE7A6):
            source = dict(self.source, composition=composition)
            effect = dict(self.effect, composition=composition)
            obj = dict(self.obj, composition=composition)
            self.assertTrue(effect_anchor_valid(effect, source))
            self.assertTrue(is_ballistic_object(obj, [source]))

    def test_no_exception_for_wrong_source_identity(self):
        for mutation in ({"tier": 0}, {"type": 0x0101}, {"composition": 0xDD9F},
                         {"composition": 0xE6CA}, {"record": 0x0F32}):
            source = dict(self.source, **mutation)
            self.assertFalse(is_ballistic_effect(self.effect, source))
            self.assertFalse(is_ballistic_object(self.obj, [source]))
            self.assertFalse(effect_anchor_valid(self.effect, source))

    def test_no_exception_for_wrong_effect_identity(self):
        for mutation in ({"kind": 5}, {"kind_name": "ground_fire"},
                         {"record": 0x0F32}, {"composition": 0xDD9F}):
            self.assertFalse(effect_anchor_valid(dict(self.effect, **mutation), self.source))

    def test_other_effects_remain_exactly_source_anchored(self):
        source = dict(self.source, type=0x0A01, composition=0xDD9F)
        effect = dict(self.effect, kind=8, kind_name="volcano_ground_fire",
                      composition=0xDD9F, world=[144, 128])
        self.assertTrue(effect_anchor_valid(effect, source))
        self.assertFalse(effect_anchor_valid(dict(effect, world=[145, 128]), source))

    def test_ballistic_anchor_still_obeys_coordinate_contract(self):
        for world in (None, "12", [], [1], [1, 2, 3], [-1, 1], [0x10000, 1], [float("nan"), 1], [True, 1]):
            self.assertFalse(effect_anchor_valid(dict(self.effect, world=world), self.source))
        self.assertTrue(effect_anchor_valid(dict(self.effect, world=[0xFFFF, 0]), self.source))

    def test_object_exception_requires_matching_source(self):
        for mutation in ({"source_index": -1}, {"source_index": 1},
                         {"source_index": True}, {"tier": 0}, {"record": 0x0F32},
                         {"composition": 0xDD9F}):
            self.assertFalse(is_ballistic_object(dict(self.obj, **mutation), [self.source]))


class MetadataStreamTest(unittest.TestCase):
    def test_full_summary_and_diagnostic_contract(self):
        fixture = Path(__file__).parent / "fixtures/sim3d/metadata-contract.json"
        # Synthetic streams freeze the observable contract before extraction,
        # including error ordering, the diagnostic cap and cross-frame state.
        cases = json.loads(fixture.read_text())["cases"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metadata.jsonl"
            for case in cases:
                with self.subTest(case=case["name"]):
                    path.write_text("".join(json.dumps(frame) + "\n" for frame in case["frames"]))
                    self.assertEqual(read_metadata(path, case["allowed"]),
                                     (case["summary"], case["errors"]))
            # Every call owns a fresh validation session, even after a bad stream.
            path.write_text("")
            self.assertEqual(read_metadata(path)[0]["frame_count"], 0)

    def test_inactive_capture_does_not_hide_broken_active_metadata(self):
        inactive = dict(view="none", separated_status=0, master_enabled=False,
                        metadata_valid=False, effect_metadata_valid=False,
                        effect_visible_count=0, effect_overflow_count=0)
        validator = MetadataValidator()
        validator.observe(inactive, 1)
        summary, _ = validator.finish()
        self.assertEqual(summary["inactive_frame_count"], 1)
        self.assertEqual(summary["invalid_frame_count"], 0)
        self.assertEqual(summary["effect_metadata_invalid_frame_count"], 0)
        for mutation in ({"view": "enhanced"}, {"master_enabled": True},
                         {"separated_status": 12}, {"effective": 1},
                         {"integrity_flags": 1}):
            validator = MetadataValidator()
            validator.observe(dict(inactive, **mutation), 1)
            summary, _ = validator.finish()
            self.assertEqual(summary["inactive_frame_count"], 0)
            self.assertEqual(summary["invalid_frame_count"], 1)
            self.assertEqual(summary["effect_metadata_invalid_frame_count"], 1)

    def test_scene_flash_budget_keeps_exact_liveness_and_local_change_limit(self):
        original = bytes([20, 30, 40, 90, 80, 70, 0, 0, 0])
        flashed = bytes([23, 35, 49, 93, 85, 79, 0, 0, 0])
        self.assertEqual(changed_pixel_counts(original, flashed, 12), (2, 0))
        moved = bytes([23, 35, 49, 180, 20, 40, 0, 0, 0])
        self.assertEqual(changed_pixel_counts(original, moved, 12), (2, 1))
        self.assertEqual(changed_pixel_counts(original, flashed), (2, 2))
        self.assertEqual(changed_pixel_counts(original, original, 12), (0, 0))
        with self.assertRaises(ValueError):
            changed_pixel_counts(original, flashed, 256)

    def test_all_profiles_pin_the_connected_globe_stage(self):
        manifest = load_manifest(Path(__file__).parent / "fixtures/sim3d/checkpoints.json")
        for name, checkpoint in manifest["checkpoints"].items():
            check_stage_pinning(name, checkpoint)
        checkpoint = manifest["checkpoints"]["D6a-lightning-miracle"]
        del checkpoint["env"]["AR_SIM3D_GLOBE_UNDERLAY"]
        with self.assertRaisesRegex(ValueError, "AR_SIM3D_GLOBE_UNDERLAY"):
            check_stage_pinning("lightning", checkpoint)

    def test_parse_error_names_file_and_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "malformed.jsonl"
            path.write_text("{broken\n")
            with self.assertRaisesRegex(ValueError, r"malformed\.jsonl:1:"):
                read_metadata(path)

    def test_expectations_still_reject_failed_counts_and_picker_contract(self):
        fixture = Path(__file__).parent / "fixtures/sim3d/metadata-contract.json"
        case = json.loads(fixture.read_text())["cases"][1]
        self.assertEqual(validate_expectations(case["summary"], {"picker_contract": False}), [])
        errors = validate_expectations(case["summary"], {"frame_count_min": 4})
        self.assertEqual(errors, [
            "D1 frame_count: expected at least 4, got 3",
            "D1 picker_flag_frame_count: replay entered no picker, so neither picker contract was exercised",
        ])


if __name__ == "__main__":
    unittest.main()
