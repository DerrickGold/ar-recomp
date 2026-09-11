#!/usr/bin/env python3
"""ROM-free regression tests for benchmark evidence reduction."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "compare_pipeline", Path(__file__).resolve().parents[1] /
    "tools/compare_pipeline_performance.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample(frames, cost, scene="Town 3D"):
    return (f"[pipeline-perf] scene={scene} frames={frames} fps=120\n"
            f"[pipeline-stage] PPU + capture mean-ms={cost:.5f} peak-ms=5\n"
            f"[pipeline-stage] PPU scanout mean-ms={cost:.5f} peak-ms=5\n"
            "[pipeline-stage] presentation mean-ms=1.0 peak-ms=2\n")


class PipelinePerformanceTest(unittest.TestCase):
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
