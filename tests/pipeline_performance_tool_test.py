#!/usr/bin/env python3
"""ROM-free regression tests for benchmark evidence reduction."""
import importlib.util
from pathlib import Path
import unittest
import sys
import tempfile
import json
import struct
import hashlib

spec = importlib.util.spec_from_file_location(
    "compare_pipeline", Path(__file__).resolve().parents[1] /
    "tools/compare_pipeline_performance.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
sys.modules["compare_pipeline_performance"] = module
retained_spec = importlib.util.spec_from_file_location(
    "compare_retained", Path(__file__).resolve().parents[1] /
    "tools/compare_retained_solids.py")
retained = importlib.util.module_from_spec(retained_spec)
retained_spec.loader.exec_module(retained)


def sample(frames, cost, scene="Town 3D"):
    return (f"[pipeline-perf] scene={scene} frames={frames} fps=120\n"
            f"[pipeline-stage] PPU + capture mean-ms={cost:.5f} peak-ms=5\n"
            f"[pipeline-stage] PPU scanout mean-ms={cost:.5f} peak-ms=5\n"
            "[pipeline-stage] presentation mean-ms=1.0 peak-ms=2\n")


class PipelinePerformanceTest(unittest.TestCase):
    def test_capture_view_must_cover_each_image_not_just_appear_in_log(self):
        def transition(frame, view):
            return f"[sim3d-view] gf={frame} old -> new (inactive, view={view}, integrity=$0)\n"
        images = {f"shot_{frame}.ppm": "hash" for frame in (1200, 1300, 1800)}
        entered = transition(353, "world_navigation")
        module.validate_capture_view(entered + transition(1900, "sky_palace"),
                                     images, "world_navigation")
        for log in ("", transition(1201, "world_navigation"),
                    entered + transition(411, "sky_palace"),
                    entered + transition(1300, "authentic_fallback"),
                    entered + transition(0, "world_navigation")):
            with self.subTest(log=log), self.assertRaises(ValueError):
                module.validate_capture_view(log, images, "world_navigation")

    def test_missing_or_empty_capture_pairs_are_not_evidence(self):
        empty = {"final_wram_sha256": "a", "images": {}}
        for runs in ([], [empty], [empty, empty]):
            with self.assertRaises(ValueError):
                module.verify_runs(runs, captures=True)

    def test_truncated_images_cannot_match_as_valid_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for bad in (b"", b"P6\n1 1\n255\n", b"P6\n0 0\n255\n"):
                (path / "shot_20.ppm").write_bytes(bad)
                with self.assertRaises(ValueError):
                    module.capture_evidence(path, 20, 20, 10)
            with self.assertRaises(ValueError):
                module.capture_evidence(path, 20, 20, 0)

    def test_text_replays_release_buttons_and_reject_overlapping_pulses(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "inputs.json"
            path.write_text(json.dumps({"frames": 5, "pulses": [[2, 2, 32]]}))
            self.assertEqual(list(struct.iter_unpack("<II", module.replay_bytes(path))),
                             [(0, 0), (1, 0), (2, 32), (3, 32), (4, 0), (5, 0)])
            for pulses in ([[-1, 1, 1]], [[5, 2, 1]], [[2, 2, 1], [3, 1, 32]]):
                path.write_text(json.dumps({"frames": 5, "pulses": pulses}))
                with self.assertRaises(ValueError):
                    module.replay_bytes(path)

    def test_render_suite_replays_are_self_contained_and_preserve_legacy_inputs(self):
        root = Path(__file__).resolve().parents[1]
        suite = json.loads((root / "tests/fixtures/benchmark/render-regression.json").read_text())
        for name, case in suite["cases"].items():
            checkpoint = json.loads((root / case["manifest"]).read_text())["checkpoints"][case["checkpoint"]]
            path = root / case.get("replay", checkpoint["replay"])
            with self.subTest(case=name):
                self.assertTrue(path.is_relative_to(root / "tests/fixtures"))
                recipe = json.loads(path.read_text())
                table = [0] * 65536
                for frame, buttons in struct.iter_unpack("<II", module.replay_bytes(path)):
                    table[frame] = buttons
                self.assertTrue(any(table))
                if "input_table_sha256" in recipe:
                    self.assertEqual(hashlib.sha256(struct.pack("<65536I", *table)).hexdigest(),
                                     recipe["input_table_sha256"])

    def test_numbered_run_directory_collision_suffix(self):
        original_root = module.ROOT
        try:
            with tempfile.TemporaryDirectory() as directory:
                module.ROOT = Path(directory)
                for name in ('20260912-163604', '20260912-163604-1', '20260912-163604-12'):
                    path = module.ROOT / 'runs' / name
                    path.mkdir(parents=True)
                    (path / 'dump_wram.bin').write_bytes(bytes(0x20000))
                    result = module.run_evidence(f'[run-dir] runs/{name} (console.log)\n')
                    self.assertEqual(result['run_dir'], str(path))
                for name in ('20260912-163604-junk', '20260912-163604-1suffix', '20260912-163604/../escape'):
                    with self.assertRaises(ValueError):
                        module.run_evidence(f'[run-dir] runs/{name} (console.log)\n')
        finally:
            module.ROOT = original_root

    def test_graphics_failure_and_partial_run_cannot_be_timing_evidence(self):
        complete = "[present-cadence] tick-presents=1800 re-presents=0 max-represent-alpha=0\n"
        self.assertEqual(module.validate_run_completion(complete, 1800),
                         {"tick_presents": 1800, "re_presents": 0})
        for invalid in ("", complete.replace("1800", "1110"),
                        complete.replace("1800", "18000"),
                        complete.replace("re-presents=0", "re-presents=1"),
                        "ERROR: Wayland display connection closed by server (fatal)\n" + complete,
                        "[sim3d-depth] vkQueueSubmit VK_ERROR_DEVICE_LOST\n" + complete,
                        "[upload] VK_ERROR_OUT_OF_DEVICE_MEMORY\n" + complete):
            with self.subTest(log=invalid), self.assertRaises(ValueError):
                module.validate_run_completion(invalid, 1800)

    def test_capture_schedule_requires_exact_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for frame in (20, 30):
                (path / f"shot_{frame}.ppm").write_bytes(b"P6\n1 1\n255\n\0\0\0")
            images = module.capture_evidence(path, 11, 31, 10)
            self.assertEqual(set(images), {"shot_20.ppm", "shot_30.ppm"})
            for start, end in ((0, 30), (11, 41), (11, 19)):
                with self.assertRaises(ValueError):
                    module.capture_evidence(path, start, end, 10)

    def test_parity_checks_state_and_images(self):
        a = {"final_wram_sha256": "a", "images": {"shot_1.ppm": "image-a"}}
        module.verify_runs([a, dict(a)], captures=True)
        with self.assertRaises(ValueError):
            module.verify_runs([a, dict(a, final_wram_sha256="b")])
        with self.assertRaises(ValueError):
            module.verify_runs([a, dict(a, images={"shot_1.ppm": "image-b"})], captures=True)
        with self.assertRaises(ValueError):
            module.verify_runs([])
        with self.assertRaises(ValueError):
            module.run_evidence("[run-dir] missing")

    def test_exact_action_room_filter_excludes_boot_and_other_rooms(self):
        def room(map_id, cost):
            return sample(10, cost, "Native/menu").replace(" fps=", f" map={map_id} fps=")
        log = room("00/00", 999) * 8 + room("04/04", 999)
        log += room("04/04", 2) * 4 + room("07/01", 999) * 8
        result = module.summarize_log(log, "Native/menu", map_id="04/04")
        self.assertEqual(result["frames"], 40)
        self.assertEqual(result["stages"]["render CPU"], 3)
        with self.assertRaises(ValueError):
            module.summarize_log(log, "Native/menu", map_id="04/0")

    def test_retained_comparison_uses_precise_pipeline_scope(self):
        log = "".join(
            "[present-perf] frames=120 present-ms avg=2.3 max=8\n"
            "[sim3d-perf] presents=120 depth-project=0.123ms depth-submit=0.234ms\n"
            "[sim3d-work] vertices/present=800 draws/present=12 vertex-upload-MiB/present=1.25\n"
            "[pipeline-perf] scene=World 3D frames=120 fps=120\n"
            "[pipeline-stage] PPU + capture mean-ms=1.2 peak-call-ms=2\n"
            "[pipeline-stage] presentation mean-ms=2.3456 peak-call-ms=8\n"
            for _ in range(8))
        result = retained.measurements(log, "World 3D")
        self.assertAlmostEqual(result["present_ms"], 2.3)
        self.assertAlmostEqual(result["pipeline_present_ms"], 2.3456)
        self.assertAlmostEqual(result["render_cpu_ms"], 3.5456)
        with self.assertRaises(ValueError):
            retained.measurements(log, "Sky Palace")

    def test_frame_weighting_and_nested_stages(self):
        result = module.summarize_log(
            sample(99, 999) + sample(1, 1) + sample(2, 2) + sample(7, 3), "Town 3D")
        self.assertEqual(result["frames"], 10)
        self.assertAlmostEqual(result["stages"]["render CPU"], 3.6)
        self.assertAlmostEqual(result["stages"]["PPU scanout"], 2.6)

    def test_settled_tail_and_scene_filter(self):
        log = sample(1, 999) + sample(1, 999) + sample(5, 999, "Sky Palace")
        log += "".join(sample(1, n) for n in range(5))
        result = module.summarize_log(log, "Town 3D")
        self.assertEqual(result["windows"], 5)
        self.assertEqual(result["stages"]["render CPU"], 3)

    def test_missing_evidence_is_not_zero_cost(self):
        for log in ("", sample(1, 2) * 3, sample(0, 2) * 4,
                    sample(1, 2).replace("presentation", "unknown") * 3):
            with self.assertRaises(ValueError):
                module.summarize_log(log, "Town 3D")

    def test_run_medians_and_ranges(self):
        runs = [{"variant": name, "stages": {"render CPU": value}}
                for name in ("control", "candidate") for value in (1, 2, 4, 99)]
        result = module.aggregate(runs)["control"]["render CPU"]
        self.assertEqual(result, {"median_ms": 3, "min_ms": 1, "max_ms": 99})


if __name__ == "__main__":
    unittest.main()
