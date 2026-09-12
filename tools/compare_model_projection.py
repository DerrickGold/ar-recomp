#!/usr/bin/env python3
"""Repeated isolated model-path experiment, not a whole-game FPS comparison.

Run serially on an otherwise idle host. Captures are separate from benchmarks.
The existing reference caches projection for held cameras. Geometry and LODs
are identical; pixel differences are reported, never treated as approval.
The GPU path deliberately remains experimental.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=480)
    parser.add_argument("--hardware-clipping", action="store_true",
                        help="compare whole-object hardware clipping; moving cameras only")
    args = parser.parse_args()
    if not 16 <= args.frames <= 10000:
        parser.error("frames must be between 16 and 10000")
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    original_hash = digest(binary)
    report = {"binary": str(binary), "sha256": original_hash,
              "hardware_clipping": args.hardware_clipping,
              "frames": args.frames, "verification": [], "comparisons": [],
              "scope": "isolated model CPU preparation/submission; not game FPS or GPU timestamps"}
    environment = os.environ.copy()
    environment.pop("AR_RENDER_WORKERS", None)
    environment["SDL_AUDIODRIVER"] = "dummy"

    def run(name, mode, camera, helpers, capture=False):
        if digest(binary) != original_hash:
            raise RuntimeError("benchmark binary changed")
        command = [str(binary), mode, camera, str(helpers), str(args.frames)]
        if capture:
            images = output / name
            images.mkdir()
            command.append(str(images))
        result = subprocess.run(command, text=True, capture_output=True, env=environment, timeout=300)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode not in ((0, 1) if capture else (0,)):
            raise RuntimeError(f"{name}: execution failed/skipped ({result.returncode}); inspect log")
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        if len(records) != 1 or records[0].get("mode") != mode:
            raise RuntimeError(f"{name}: missing/invalid measurement")
        record = records[0]
        record["exit_code"] = result.returncode
        record["log"] = name + ".log"
        print(name, json.dumps(record), flush=True)
        return record

    # Differences are evidence, not permission to relax the strict oracle.
    suffix = "-clipped" if args.hardware_clipping else ""
    for helpers in (0, 3):
        report["verification"].append(run(f"verify-{helpers}", "verify" + suffix, "moving", helpers, True))
    report["strict_visual_parity"] = all(r["exit_code"] == 0 for r in report["verification"])
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    # The legacy harness retains projected vertices, not post-clip polygons.
    # Do not claim a held-camera CPU comparison against a different cache.
    for camera in (("moving",) if args.hardware_clipping else ("moving", "held")):
        for helpers in (0, 3):
            runs = {"cpu": [], "gpu": []}
            for index, mode in enumerate(("cpu", "gpu", "gpu", "cpu", "cpu", "gpu", "gpu", "cpu")):
                record = run(f"{camera}-{helpers}-{index}-{mode}", mode + suffix, camera, helpers)
                if record.get("frames") != args.frames or record.get("camera") != camera:
                    raise RuntimeError("mismatched benchmark configuration")
                runs[mode].append(record)
            vertices = {r["vertices"] for records in runs.values() for r in records}
            if len(vertices) != 1:
                raise RuntimeError("CPU/GPU geometry counts differ")
            summary = {"camera": camera, "helpers_requested": helpers, "runs": runs}
            for field in ("prepare_ms", "scene_cpu_ms", "frame_ms", "frame_p95_ms",
                          "geometry_bytes_per_frame", "draws_per_frame"):
                summary[field] = {}
                for mode, records in runs.items():
                    values = [r[field] for r in records]
                    summary[field][mode] = {"median": statistics.median(values),
                                           "min": min(values), "max": max(values)}
            report["comparisons"].append(summary)
            (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    if digest(binary) != original_hash:
        raise RuntimeError("benchmark binary changed")
    report["strict_visual_parity"] = all(r["exit_code"] == 0 for r in report["verification"])
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Results:", output / "results.json")
    if not report["strict_visual_parity"]:
        print("Strict image oracle differs: prototype is NOT approved for production.")


if __name__ == "__main__":
    main()
