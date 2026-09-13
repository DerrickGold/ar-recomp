#!/usr/bin/env python3
"""Validate fetched Deck evidence, summarize repeats, and archive logs (no saves/ROM)."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import zipfile

from probe import summarize

SCENES = {"sim-held": "Town 3D", "sim-low": "Town 3D",
          "navigation": "World 3D", "palace": "Sky Palace"}
FAILURE_COUNTERS = ("pipeline-work/fallback", "pipeline-work/failed",
                    "pipeline-path/rejected", "pipeline-path/opt-out",
                    "pipeline-path/limit")


def distribution(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    sources = [args.scratch / name for name in
               ("deck-baseline", "deck-low-angle", "deck-dynamic-camera")]
    sources += sorted((args.scratch / "deck-cloud-pair").glob("cloud-pair-*"))
    sources += sorted((args.scratch / "deck-cloud-components").glob("cloud-component-*"))
    records, archive_files = [], {}
    for source in sources:
        results_path = source / "results.json"
        results = json.loads(results_path.read_text())
        archive_files[f"{source.name}/results.json"] = results_path
        for run in results:
            folder = source / f'{run["repeat"]}-{run["case"]}'
            log_path = folder / "game.log"
            log = log_path.read_text()
            recomputed = summarize(log, SCENES[run["case"]])
            for field in ("frames", "windows", "cadence_ms", "present_fps", "stages", "counts"):
                assert recomputed[field] == run[field], (folder, field)
            assert run["guard"]["returncode"] == 0 and run["guard"]["abort"] is None
            assert {s["output"] for s in run["samples"]} == {"1280x800"}
            assert all(s["counts"].get(k, 0) == 0
                       for s in run["samples"] for k in FAILURE_COUNTERS), folder
            for name in ("game.log", "environment.json", "guard.json", "telemetry.json"):
                path = folder / name
                if path.exists():
                    archive_files[f"{source.name}/{folder.name}/{name}"] = path
            worst = max(run["samples"][1:], key=lambda s: s["cadence_ms"])
            if source.name == "deck-baseline":
                group = f'baseline/{run["case"]}'
            elif source.name.startswith("cloud-pair-"):
                group = "cloud-pair/" + source.name.split("-", 3)[3]
            elif source.name.startswith("cloud-component-"):
                group = "cloud-component/" + source.name.split("-", 3)[3]
            else:
                group = source.name.removeprefix("deck-")
            record = {k: run[k] for k in ("case", "repeat", "present_fps", "cadence_ms",
                      "frames", "windows", "seconds", "final_wram_sha256", "run_dir", "stages", "counts")}
            record.update(group=group, source=source.name,
                          worst_window={"fps": 1000 / worst["cadence_ms"], **worst},
                          view_transitions=[line for line in log.splitlines() if "[sim3d-view]" in line],
                          log_sha256=hashlib.sha256(log_path.read_bytes()).hexdigest(),
                          minimum_available_MiB=run["guard"]["minimum_available"] / 1024**2,
                          maximum_gpu_growth_MiB=run["guard"]["maximum_gpu_growth"] / 1024**2)
            telemetry_path = folder / "telemetry.json"
            if telemetry_path.exists():
                # Approximate scene-only wall-clock intervals; not per-pass GPU timers.
                start, end = (17, 39) if run["case"].startswith("sim") else (8, 32)
                values = [v for sample in json.loads(telemetry_path.read_text())
                          if start <= sample["seconds"] <= end
                          for v in sample["gpu_busy_pct"].values()]
                if values:
                    record["gpu_busy_pct"] = {"mean": statistics.mean(values),
                        "min": min(values), "max": max(values), "samples": len(values),
                        "wall_seconds": [start, end]}
            records.append(record)

    for fixture in ("sim", "navigation", "palace"):
        matching = [r for r in records if r["case"].startswith(fixture)]
        assert len({r["final_wram_sha256"] for r in matching}) == 1, fixture

    groups = {}
    for name in sorted({r["group"] for r in records}):
        runs = [r for r in records if r["group"] == name]
        group = {"runs": len(runs)}
        for field in ("present_fps", "cadence_ms"):
            group[field] = distribution([r[field] for r in runs])
        for field in ("stages", "counts"):
            keys = set().union(*(r[field] for r in runs))
            group[field] = {k: distribution([r[field].get(k, 0) for r in runs]) for k in sorted(keys)}
        group["worst_window_fps"] = [r["worst_window"]["fps"] for r in runs]
        if all("gpu_busy_pct" in r for r in runs):
            group["gpu_busy_mean_pct"] = distribution([r["gpu_busy_pct"]["mean"] for r in runs])
        groups[name] = group
        fps, cpu = group["present_fps"], group["stages"]["render CPU"]
        print(f'{name:36} n={len(runs)} {fps["median"]:7.2f} FPS '
              f'[{fps["min"]:.2f}..{fps["max"]:.2f}] CPU {cpu["median"]:.4f} ms')

    input_hashes = args.scratch / "deck-input-hashes.json"
    archive_files["input-hashes.json"] = input_hashes
    args.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output / "raw-evidence.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for name, path in sorted(archive_files.items()):
            archive.write(path, name)
    report = {"commit": "e1699c9e", "binary_sha256":
              "012e457c98a4b1063f128dc065414ad27062b7009119e7122a86c01ddd00762a",
              "input_hashes": json.loads(input_hashes.read_text()),
              "target_failure_counters_all_zero": list(FAILURE_COUNTERS),
              "note": "Nested stage means are not additive. GPU busy is coarse utilization, not GPU time.",
              "groups": groups, "runs": records}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
