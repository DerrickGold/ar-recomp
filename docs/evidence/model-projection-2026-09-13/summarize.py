#!/usr/bin/env python3
"""Validate model-projection A/B logs and archive reproducible, ROM-free evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import sys
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "all-effects-2026-09-12"))
from probe import summarize

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
args = parser.parse_args()
sources = [("mac-fullscreen-noisy", args.root),
           ("mac-windowed-noisy", args.root / "windowed"),
           ("deck", args.root / "deck-results")]
records, files = [], {}
checks = ("pipeline-work/failed", "pipeline-work/fallback", "pipeline-path/rejected",
          "pipeline-path/opt-out", "pipeline-path/limit")
for host, source in sources:
    for result_path in sorted(source.glob("*-*/results.json")):
        if result_path.parent.name.startswith("images"):
            continue
        for result in json.loads(result_path.read_text()):
            name = result_path.parent.name
            run = result_path.parent / f'{result["repeat"]}-{result["case"]}'
            log = (run / "game.log").read_text()
            parsed = summarize(log, "Town 3D")
            for field in ("frames", "windows", "stages", "counts", "present_fps"):
                assert parsed[field] == result[field], (run, field)
            assert result["guard"]["returncode"] == 0 and not result["guard"]["abort"]
            assert all(s["counts"].get(c, 0) == 0 for s in parsed["samples"] for c in checks), run
            folder = f"{host}/{name}"
            files[folder + "/results.json"] = result_path
            for filename in ("game.log", "environment.json", "guard.json", "telemetry.json"):
                if (run / filename).exists(): files[folder + "/" + filename] = run / filename
            worst = max(parsed["samples"][1:], key=lambda s: s["cadence_ms"])
            record = {k: result[k] for k in ("stages", "counts", "present_fps", "cadence_ms",
                       "final_wram_sha256", "frames", "windows", "run_dir")}
            record.update(host=host, cohort=name, variant=name.rsplit("-",1)[1],
                          mode=name.split("-",1)[0], worst_window_fps=1000/worst["cadence_ms"],
                          output=parsed["samples"][0]["output"],
                          log_sha256=hashlib.sha256((run / "game.log").read_bytes()).hexdigest())
            records.append(record)
    if (source / "inputs/hashes.json").exists():
        files[host + "/input-hashes.json"] = source / "inputs/hashes.json"
assert len({r["final_wram_sha256"] for r in records}) == 1
groups = {}
for key in sorted({(r["host"], r["mode"], r["variant"]) for r in records}):
    runs = [r for r in records if (r["host"],r["mode"],r["variant"]) == key]
    group = {"runs": len(runs), "output": sorted({r["output"] for r in runs})}
    for name, values in {
        "fps": [r["present_fps"] for r in runs],
        "render_cpu_ms": [r["stages"]["render CPU"] for r in runs],
        "model_prepare_ms": [r["stages"]["SIM model project"] for r in runs],
        "depth_upload_MiB": [r["counts"]["pipeline-work/depth-MiB"] for r in runs],
        "worst_window_fps": [r["worst_window_fps"] for r in runs],
    }.items():
        group[name] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
    groups["/".join(key)] = group
    print("/".join(key), json.dumps(group))
images = args.root / "images-comparison/results.json"
files["images/results.json"] = images
smoke = args.root / "default-check/smoke-0-default/0-sim-held"
assert "AR_SIM3D_TOWN_GPU_MODELS" not in json.loads((smoke / "environment.json").read_text())
guard = json.loads((smoke / "guard.json").read_text())
assert guard["returncode"] == 0 and not guard["abort"]
comparisons = []
for shot in sorted({p.resolve() for p in smoke.glob("runs/*/shot_*.ppm")}):
    peers = {p.resolve() for p in (args.root / "images-1-gpu/0-sim-held").glob("runs/*/" + shot.name)}
    assert len(peers) == 1 and shot.read_bytes() == peers.pop().read_bytes()
    comparisons.append(shot.stem)
assert len(comparisons) == 3
default_check = {"no_model_path_override": True, "identical_to_opt_in_frames": comparisons,
                 "binary_sha256": hashlib.sha256((args.root / "default-check/inputs/game").read_bytes()).hexdigest()}
for name in ("environment.json", "guard.json", "game.log"):
    files["default-check/" + name] = smoke / name
files["default-check/input-hashes.json"] = args.root / "default-check/inputs/hashes.json"
for name in ("gpu-tests-r3.log", "deck-depth-test.log", "default-gpu-tests.log"):
    files[name] = args.root / name
with zipfile.ZipFile(HERE / "raw-evidence.zip", "w", zipfile.ZIP_DEFLATED) as archive:
    for name, path in sorted(files.items()): archive.write(path, name)
(HERE / "results.json").write_text(json.dumps({
    "baseline_commit": "e1699c9e", "groups": groups, "runs": records,
    "images": json.loads(images.read_text()),
    "default_check": default_check,
    "zero_failure_counters": checks,
    "note": "Mac throughput has substantial drift; do not report median ratios there as a reliable gain. Nested CPU scopes are not additive.",
}, indent=2)+"\n")
