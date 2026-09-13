#!/usr/bin/env python3
"""Validate all-effects cloud A/B records and archive ROM-free diagnostics."""
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
records, files, groups, images = [], {}, {}, {}
checks = ("pipeline-work/failed", "pipeline-work/fallback", "pipeline-path/rejected",
          "pipeline-path/opt-out", "pipeline-path/limit")
for host, source in (("mac", args.root), ("deck", args.root / "deck-results")):
    for path in sorted(source.glob("dynamic-*/results.json")):
        results = json.loads(path.read_text())
        assert len(results) == 1
        result = results[0]
        run = path.parent / "0-palace"
        parsed = summarize((run / "game.log").read_text(), "Sky Palace")
        for field in ("frames", "windows", "stages", "counts", "present_fps"):
            assert parsed[field] == result[field], (run, field)
        assert result["guard"]["returncode"] == 0 and not result["guard"]["abort"]
        assert all(s["counts"].get(c, 0) == 0 for s in parsed["samples"] for c in checks), run
        record = {k: result[k] for k in ("stages", "counts", "present_fps", "cadence_ms",
            "final_wram_sha256", "frames", "windows")}
        record.update(host=host, cohort=path.parent.name, variant=path.parent.name.rsplit("-",1)[1],
            output=parsed["samples"][0]["output"],
            log_sha256=hashlib.sha256((run / "game.log").read_bytes()).hexdigest())
        records.append(record)
    for name in ("full", "bounded"):
        runs = [r for r in records if r["host"] == host and r["variant"] == name]
        assert len(runs) == 3, (host, name)
        group = {"runs": len(runs), "output": sorted({r["output"] for r in runs})}
        for field, values in {"fps": [r["present_fps"] for r in runs],
            "render_cpu_ms": [r["stages"]["render CPU"] for r in runs],
            "cloud_cpu_ms": [r["stages"]["SIM clouds"] for r in runs],
            "cadence_ms": [r["cadence_ms"] for r in runs]}.items():
            group[field] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        groups[host + "/" + name] = group
        print(host + "/" + name, json.dumps(group))
    # Include untimed visual/default checks without trying to interpret them as FPS.
    for path in source.glob("*/results.json"):
        if path.parent.name.endswith("comparison"):
            images[host + "/" + path.parent.name] = json.loads(path.read_text())
            files[host + "/" + path.parent.name + "/results.json"] = path
            continue
        files[host + "/" + path.parent.name + "/results.json"] = path
        for filename in ("game.log", "environment.json", "guard.json", "telemetry.json", "processes-before.txt"):
            p = path.parent / "0-palace" / filename
            if p.exists(): files[host + "/" + path.parent.name + "/" + filename] = p
    files[host + "/input-hashes.json"] = source / "inputs/hashes.json"
assert len({r["final_wram_sha256"] for r in records}) == 1
smoke = args.root / "default-check/images-0-default/0-palace"
assert "AR_SIM3D_SKY_CLOUD_BOUNDS" not in json.loads((smoke / "environment.json").read_text())
guard = json.loads((smoke / "guard.json").read_text())
assert guard["returncode"] == 0 and not guard["abort"]
identical = []
for shot in sorted({p.resolve() for p in smoke.glob("runs/*/shot_*.ppm")}):
    peers = {p.resolve() for p in (args.root / "images-0-bounded/0-palace").glob("runs/*/" + shot.name)}
    assert len(peers) == 1 and shot.read_bytes() == peers.pop().read_bytes()
    identical.append(shot.stem)
assert len(identical) == 3
for name in ("game.log", "environment.json", "guard.json"):
    files["default-check/" + name] = smoke / name
files["default-check/input-hashes.json"] = args.root / "default-check/inputs/hashes.json"
for name in ("unit-tests.log", "boundary-tests.log"):
    files[name] = args.root / name
with zipfile.ZipFile(HERE / "raw-evidence.zip", "w", zipfile.ZIP_DEFLATED) as archive:
    for name, path in sorted(files.items()): archive.write(path, name)
(HERE / "results.json").write_text(json.dumps({
    "baseline_commit": "fd73797e", "groups": groups, "runs": records,
    "images": images, "zero_failure_counters": checks,
    "default_check": {"no_bounds_override": True, "identical_to_opt_in_frames": identical},
    "note": "Paired runs were stable on both hosts in this batch. Mac background indexing/media processes are recorded, not claimed absent. Nested CPU scopes are not additive.",
}, indent=2)+"\n")
