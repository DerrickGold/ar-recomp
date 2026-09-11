#!/usr/bin/env python3
"""Repeated, isolated CPU pipeline comparisons using existing replay fixtures.

The game still performs real GPU presentation. These CPU wall scopes are not
GPU timestamps or estimates of performance on a different machine.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RENDER_STAGES = (
    "PPU + capture", "world map build", "SIM metadata", "town canvas",
    "frame snapshot", "upload", "presentation",
)


def summarize_log(log: str, scene: str, windows: int = 5) -> dict:
    """Weight samples by presents; reject short/missing or malformed evidence."""
    samples = []
    for block in log.split("[pipeline-perf]")[1:]:
        if not block.startswith(f" scene={scene} "):
            continue
        count = re.search(r" frames=(\d+)(?:\s|$)", block.splitlines()[0])
        if not count or not int(count[1]):
            continue
        stages = {match[0]: float(match[1]) for match in re.findall(
            r"\[pipeline-stage\] (.*?) mean-ms=(\d+(?:\.\d+)?)(?:\s|$)", block)}
        # Absent optional stages (e.g. town canvas in a menu) are zero, but
        # missing top-level render instrumentation is not a zero-cost frame.
        if not all(stage in stages for stage in ("PPU + capture", "presentation")):
            raise ValueError("Missing top-level pipeline stages")
        stages["render CPU"] = sum(stages.get(stage, 0) for stage in RENDER_STAGES)
        samples.append((int(count[1]), stages))
    # Scene entry can include shader/assets/cache warmup. Even short captures
    # must have three samples after their first matching reporting window.
    samples = samples[1:][-windows:]
    if len(samples) < 3:
        raise ValueError(f"Need at least three settled {scene!r} sample windows")
    count = sum(n for n, _ in samples)
    stages = set().union(*(values for _, values in samples))
    return {"frames": count, "windows": len(samples), "stages": {
        stage: sum(n * values.get(stage, 0) for n, values in samples) / count
        for stage in sorted(stages)
    }}


def aggregate(results: list[dict]) -> dict:
    summary = {}
    for variant in ("control", "candidate"):
        values = [r["stages"] for r in results if r["variant"] == variant]
        stages = set().union(*(v for v in values))
        summary[variant] = {
            stage: {"median_ms": statistics.median(v.get(stage, 0) for v in values),
                    "min_ms": min(v.get(stage, 0) for v in values),
                    "max_ms": max(v.get(stage, 0) for v in values)}
            for stage in sorted(stages)
        }
    return summary


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolve(path: str) -> Path:
    candidate = Path(path)
    return (candidate if candidate.is_absolute() else ROOT / candidate).resolve()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--config", type=Path, default=ROOT / "config.ini")
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "tests/fixtures/sim3d/checkpoints.json")
    parser.add_argument("--checkpoint", default="D7-voxel-town")
    parser.add_argument("--scene", default="Town 3D")
    parser.add_argument("--quit-frames", type=int, default=1800)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--control-workers", type=int, choices=range(4), default=3)
    parser.add_argument("--candidate-workers", type=int, choices=range(4), default=3)
    parser.add_argument("--set", action="append", default=[], metavar="AR_NAME=VALUE",
                        help="Presentation overrides shared by both variants")
    parser.add_argument("--output", type=Path,
                        help="New output directory; existing paths are refused")
    args = parser.parse_args()
    if args.quit_frames < 1 or args.timeout < 1:
        parser.error("Frame limit and timeout must be positive")
    checkpoint = json.loads(args.manifest.read_text())["checkpoints"][args.checkpoint]
    replay = resolve(checkpoint["replay"])
    settings = resolve(checkpoint["settings"])
    seed_path = resolve(checkpoint.get("sram_base64") or checkpoint["sram"])
    seed = seed_path.read_bytes()
    if checkpoint.get("sram_base64"):
        seed = base64.b64decode(b"".join(seed.split()), validate=True)
    seed_hash = hashlib.sha256(seed).hexdigest()
    if len(seed) != 8192 or seed_hash != checkpoint["sram_sha256"]:
        raise ValueError("SRAM fixture length/hash mismatch")
    binaries = {"control": args.control.resolve(), "candidate": args.candidate.resolve()}
    inputs = [*binaries.values(), args.rom.resolve(), args.config.resolve(),
              args.manifest.resolve(), replay, settings, seed_path]
    hashes = {str(path): digest(path) for path in inputs}
    overrides = {}
    for item in args.set:
        name, separator, value = item.partition("=")
        if not separator or not name.startswith("AR_"):
            parser.error("Overrides must use AR_NAME=VALUE")
        overrides[name] = value
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="actraiser-pipeline-"))
    # Keep private fixture copies even though replay mode also protects saves.
    isolated_seed = output / "seed.srm"
    isolated_settings = output / "settings.ini"
    isolated_seed.write_bytes(seed)
    isolated_settings.write_bytes(settings.read_bytes())
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("AR_", "SNESRECOMP_"))}
    env.update(checkpoint.get("env", {}))
    env.update(overrides)
    # Safety and measurement invariants cannot be overridden through --set.
    env.update(AR_HEADLESS="1", AR_HEADLESS_VIDEO="1", AR_ENABLE_RUN_DIR="1",
               AR_INPUT_REPLAY=str(replay), AR_SAVE_NATIVE_PATH=str(isolated_seed),
               AR_SETTINGS_PATH=str(isolated_settings), AR_PIPELINE_PERF="1",
               AR_PERFORMANCE_OVERLAY="Off", AR_REFRESH_MODE="Unlimited",
               AR_QUIT_FRAMES=str(args.quit_frames))
    for name in ("AR_INPUT_RECORD", "AR_SHOT_EVERY", "AR_SHOT_FRAMES",
                 "AR_PERF", "AR_SIM3D_PERF", "AR_DUMP_EVERY"):
        env.pop(name, None)
    results = []
    report = {"schema": "actraiser-pipeline-comparison-v1", "scene": args.scene,
              "input_sha256": hashes, "seed_sha256": seed_hash,
              "environment": {k: v for k, v in env.items() if k.startswith("AR_")},
              "workers": {"control": args.control_workers,
                          "candidate": args.candidate_workers}, "runs": results}
    print(f"Evidence: {output}", flush=True)
    for index, variant in enumerate(("control", "candidate", "candidate", "control") * 2):
        if any(digest(Path(path)) != expected for path, expected in hashes.items()):
            raise RuntimeError("Benchmark inputs changed; discard this comparison")
        env["AR_RENDER_WORKERS"] = str(report["workers"][variant])
        isolated_seed.write_bytes(seed)
        isolated_settings.write_bytes(settings.read_bytes())
        log_path = output / f"{index}-{variant}.log"
        with log_path.open("w") as log:
            subprocess.run([str(binaries[variant]), str(args.rom.resolve()),
                            "--config", str(args.config.resolve())], cwd=ROOT, env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True,
                           timeout=args.timeout)
        if any(digest(Path(path)) != expected for path, expected in hashes.items()):
            raise RuntimeError("Benchmark inputs changed during run")
        result = summarize_log(log_path.read_text(), args.scene)
        result.update(variant=variant, log=str(log_path))
        results.append(result)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"{index + 1}/8 {variant}: {result['stages']['render CPU']:.4f} ms render CPU",
              flush=True)
    report["summary"] = aggregate(results)
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    before = report["summary"]["control"]["render CPU"]["median_ms"]
    after = report["summary"]["candidate"]["render CPU"]["median_ms"]
    print(f"Render CPU median: {before:.4f} -> {after:.4f} ms")
    print("CPU wall scopes only; review ranges and stages, not just the medians.")


if __name__ == "__main__":
    main()
