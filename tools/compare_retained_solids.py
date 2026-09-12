#!/usr/bin/env python3
"""Serial, same-binary comparison of held-view geometry caches.

Uses isolated copies of saves/settings. Timing and image readback never overlap.
Reported stage times are CPU wall time, not GPU timestamps or unconstrained FPS.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time
from compare_pipeline_performance import summarize_log


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measurements(log, scene):
    series = {name: [] for name in ("present_ms", "model_project_ms", "depth_submit_ms",
                                   "vertices", "draws", "depth_upload_mib")}
    presents = None
    for line in log.splitlines():
        if line.startswith("[present-perf]"):
            n = re.search(r"frames=(\d+)", line)
            value = re.search(r"present-ms avg=([\d.]+)", line)
            if n and value:
                series["present_ms"].append((int(n[1]), float(value[1])))
        elif line.startswith("[sim3d-perf]"):
            n = re.search(r"presents=(\d+)", line)
            presents = int(n[1]) if n else None
            for field, key in (("depth-project", "model_project_ms"), ("depth-submit", "depth_submit_ms")):
                value = re.search(rf" {field}=([\d.]+)ms", line)
                if presents and value:
                    series[key].append((presents, float(value[1])))
        elif line.startswith("[sim3d-work]") and presents:
            for field, key in (("vertices/present", "vertices"), ("draws/present", "draws"),
                               ("vertex-upload-MiB/present", "depth_upload_mib")):
                value = re.search(rf" {field}=([\d.]+)", line)
                if value:
                    series[key].append((presents, float(value[1])))
    result = {}
    for key, rows in series.items():
        steady = rows[3:]  # Boot, entry and initial cache warmup excluded.
        if len(steady) < 3:
            raise RuntimeError(f"insufficient settled measurements: {key}")
        result[key] = sum(n * v for n, v in steady) / sum(n for n, _ in steady)
    # Legacy present-ms has only 0.1 ms precision. Use the existing pipeline
    # reducer for the default decision, while retaining legacy comparisons.
    fine = summarize_log(log, scene, windows=64)
    result["pipeline_present_ms"] = fine["stages"]["presentation"]
    result["render_cpu_ms"] = fine["stages"]["render CPU"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "rom", "config", "settings", "save", "palace_replay", "navigation_replay"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--default-candidate", action="store_true",
                        help="with --feature all, compare forced-off against unset/default cache policy")
    parser.add_argument("--feature", choices=("solids", "ground", "all"), default="solids",
                        help="ground toggles ground with solids fixed on; all toggles both")
    parser.add_argument("--workers", type=int, choices=range(4), default=3)
    parser.add_argument("--preset", choices=("Performance", "Balanced", "Quality"), default="Performance")
    args = parser.parse_args()
    if args.default_candidate and args.feature != "all":
        parser.error("--default-candidate requires --feature all")
    inputs = {name: getattr(args, name).resolve(strict=True)
              for name in ("binary", "rom", "config", "settings", "save", "palace_replay", "navigation_replay")}
    hashes = {name: digest(path) for name, path in inputs.items()}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"inputs": {name: str(p) for name, p in inputs.items()}, "sha256": hashes,
              "scope": "held-view retained projected geometry; not GPU model projection or GPU timestamps",
              "feature": args.feature, "default_candidate": args.default_candidate,
              "verification": args.verify_only, "workers": args.workers,
              "preset": args.preset, "runs": [], "summary": {}}
    for view in ("palace", "navigation"):
        captures = []
        for index, enabled in enumerate((0, 1) if args.verify_only else (0, 1, 1, 0, 0, 1, 1, 0)):
            if {name: digest(p) for name, p in inputs.items()} != hashes:
                raise RuntimeError("input or executable changed during comparison")
            name = f"{view}-{index}-{enabled}"
            save = output / (name + ".srm")
            settings = output / (name + ".ini")
            shutil.copyfile(inputs["save"], save)
            shutil.copyfile(inputs["settings"], settings)
            env = {k: v for k, v in os.environ.items() if not k.startswith(("AR_", "SNESRECOMP_"))}
            env.update(AR_HEADLESS="1", AR_HEADLESS_VIDEO="1", AR_ENABLE_RUN_DIR="1",
                       AR_REFRESH_MODE="Unlimited", AR_RENDER_WORKERS=str(args.workers),
                       AR_SIM3D_WORLD_NAV="1", AR_SIM3D_SKY_PALACE="1", AR_SIM3D_VOXEL_PRESET=args.preset,
                       AR_SIM3D_RETAINED_SOLIDS=str(1 if args.feature == "ground" else enabled),
                       AR_SIM3D_RETAINED_GROUND=str(enabled if args.feature != "solids" else 0),
                       AR_SAVE_NATIVE_PATH=str(save),
                       AR_SETTINGS_PATH=str(settings), AR_INPUT_REPLAY=str(inputs[view + "_replay"]),
                       SDL_AUDIODRIVER="dummy")
            if enabled and args.default_candidate:
                env.pop("AR_SIM3D_RETAINED_SOLIDS")
                env.pop("AR_SIM3D_RETAINED_GROUND")
            if args.verify_only:
                env.update(AR_SIM3D_CLOUD_DRIFT="0", AR_SHOT_EVERY="100", AR_SHOT_FROM="400",
                           AR_SHOT_TO="1700" if view == "palace" else "2200", AR_SHOT_REQUIRE_COMPOSITE="1")
            else:
                env["AR_PERF"] = "1"
                env["AR_PIPELINE_PERF"] = "1"
                env["AR_PERFORMANCE_OVERLAY"] = "Off"
            start = time.monotonic()
            completed = subprocess.run([str(inputs["binary"]), str(inputs["rom"]), "--config", str(inputs["config"])],
                                       env=env, capture_output=True, text=True, timeout=300)
            log = completed.stdout + completed.stderr
            (output / (name + ".log")).write_text(log)
            if completed.returncode:
                raise RuntimeError(f"{name}: run failed; inspect log")
            run_match = re.search(r"\[run-dir\] (runs/\d+-\d+)", log)
            if not run_match:
                raise RuntimeError(f"{name}: missing run bundle")
            run_dir = Path(run_match[1])
            row = {"view": view, "enabled": enabled, "index": index, "log": name + ".log",
                   "run_dir": str(run_dir), "seconds": time.monotonic() - start,
                   "final_wram_sha256": digest(run_dir / "dump_wram.bin")}
            if args.verify_only:
                images = {p.name: digest(p) for p in run_dir.glob("shot_*.ppm")}
                expected = 14 if view == "palace" else 19
                if len(images) != expected:
                    raise RuntimeError(f"{name}: expected {expected} composite captures, found {len(images)}")
                captures.append(images)
                row["images"] = images
            else:
                row["means"] = measurements(log, "Sky Palace" if view == "palace" else "World 3D")
            report["runs"].append(row)
            (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
            print(json.dumps({k: v for k, v in row.items() if k != "images"}), flush=True)
        rows = [r for r in report["runs"] if r["view"] == view]
        if len({r["final_wram_sha256"] for r in rows}) != 1:
            raise RuntimeError(f"{view}: simulation state differs")
        if args.verify_only:
            if captures[0] != captures[1]:
                raise RuntimeError(f"{view}: strict composite parity failed")
            report["summary"][view] = {"byte_identical_captures": len(captures[0])}
        else:
            report["summary"][view] = {}
            for field in rows[0]["means"]:
                report["summary"][view][field] = {}
                for enabled in (0, 1):
                    values = [r["means"][field] for r in rows if r["enabled"] == enabled]
                    report["summary"][view][field][str(enabled)] = {
                        "median": statistics.median(values), "min": min(values), "max": max(values)}
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    if {name: digest(p) for name, p in inputs.items()} != hashes:
        raise RuntimeError("input or executable changed during comparison")
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
