#!/usr/bin/env python3
"""Reparse shadow benchmarks, validate state/counters and archive safe evidence."""
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
scenes = {"palace":"Sky Palace", "navigation":"World 3D", "sim-held":"Town 3D"}
checks = ("pipeline-work/failed", "pipeline-work/fallback", "pipeline-path/rejected",
          "pipeline-path/opt-out", "pipeline-path/limit")
for host, source in (("mac",args.root),("deck",args.root/"deck-results")):
    for path in sorted(set(source.glob("timed-*/results.json")) | set(source.glob("modes-*/results.json"))):
        for result in json.loads(path.read_text()):
            case = result["case"]
            run = path.parent / f'0-{case}'
            parsed = summarize((run/"game.log").read_text(),scenes[case])
            for field in ("frames","windows","stages","counts","present_fps"):
                assert parsed[field] == result[field],(run,field)
            assert result["guard"]["returncode"] == 0 and not result["guard"]["abort"]
            assert all(s["counts"].get(c,0) == 0 for s in parsed["samples"] for c in checks),run
            record = {k:result[k] for k in ("stages","counts","present_fps","cadence_ms",
                "final_wram_sha256","frames","windows")}
            record.update(host=host,case=case,cohort=path.parent.name,variant=path.parent.name.rsplit("-",1)[1],
                output=parsed["samples"][0]["output"],
                log_sha256=hashlib.sha256((run/"game.log").read_bytes()).hexdigest())
            records.append(record)
    for path in source.glob("*/results.json"):
        folder = host+"/"+path.parent.name
        files[folder+"/results.json"] = path
        if path.parent.name.endswith("comparison"):
            images[folder] = json.loads(path.read_text())
            continue
        for run in path.parent.glob("0-*"):
            for name in ("game.log","environment.json","guard.json","telemetry.json","processes-before.txt"):
                p=run/name
                if p.exists(): files[folder+"/"+run.name+"/"+name]=p
    files[host+"/input-hashes.json"]=source/"inputs/hashes.json"
for key in sorted({(r["host"],r["case"],r["variant"]) for r in records}):
    runs=[r for r in records if (r["host"],r["case"],r["variant"]) == key]
    assert len(runs) == (3 if key[1] == "palace" else 2),key
    group={"runs":len(runs),"output":sorted({r["output"] for r in runs})}
    group["interpretation"] = (
        "No performance conclusion: noisy control; SIM does not submit eligible surface shadows."
        if key[1] == "sim-held" else
        "No speedup claim: substantial host drift during cross-mode runs."
        if key[:2] == ("mac", "navigation") else
        "Stable paired throughput comparison.")
    for name,values in {"fps":[r["present_fps"] for r in runs],
        "render_cpu_ms":[r["stages"]["render CPU"] for r in runs],
        "cadence_ms":[r["cadence_ms"] for r in runs],
        "draws":[r["counts"]["pipeline-work/draws"] for r in runs],
        "vertices":[r["counts"]["pipeline-work/vertices"] for r in runs]}.items():
        group[name]={"median":statistics.median(values),"min":min(values),"max":max(values)}
    groups["/".join(key)]=group
    print("/".join(key),json.dumps(group))
for case in scenes:
    assert len({r["final_wram_sha256"] for r in records if r["case"] == case}) == 1,case
default_root = args.root / "default-check"
defaults = json.loads((default_root / "images-0-default/results.json").read_text())
opt_in = json.loads((args.root / "images-0-batch/results.json").read_text())
assert {r["case"] for r in defaults} == set(scenes)
for result in defaults:
    reference = next(r for r in opt_in if r["case"] == result["case"])
    assert result["images"] == reference["images"], result["case"]
    assert result["final_wram_sha256"] == reference["final_wram_sha256"], result["case"]
    assert result["guard"]["returncode"] == 0 and not result["guard"]["abort"]
    run = default_root / "images-0-default" / ("0-"+result["case"])
    assert "AR_SIM3D_SHADOW_BATCH" not in json.loads((run/"environment.json").read_text())
    for name in ("game.log","environment.json","guard.json","processes-before.txt"):
        files["mac/default-check/"+run.name+"/"+name] = run/name
files["mac/default-check/results.json"] = default_root / "images-0-default/results.json"
files["mac/default-check/input-hashes.json"] = default_root / "inputs/hashes.json"
for name in ("gpu-tests.log","gpu-reference-tests.log","deck-depth-test.log","boundary-tests.log","shader-check.log",
             "gpu-default-tests.log","gpu-default-reference-tests.log",
             "deck-default-depth-test.log","deck-default-reference-test.log"):
    files[name]=args.root/name
hashes = {host:json.loads((source/"inputs/hashes.json").read_text())["game"] for host,source in
          (("mac_ab",args.root),("deck_ab",args.root/"deck-results"),("mac_default",default_root))}
source_files = ["src/platform/sdl/sim3d_depth_pass_sdl.c", "src/sim/sim3d_depth_pass.h",
                "tests/sim3d_depth_pass_gpu_test.c", "tests/shader_blob_test.c"]
for stem in ("sim3d_surface", "sim3d_shadow_batch"):
    for stage in ("vert", "frag"):
        source_files.extend((f"src/shaders/{stem}.{stage}.glsl", f"src/shaders/{stem}_{stage}.h"))
source_files.append("src/shaders/sim3d_surface_mapping.glsl")
source_hashes = {p:hashlib.sha256((HERE.parents[2]/p).read_bytes()).hexdigest() for p in source_files}
with zipfile.ZipFile(HERE/"raw-evidence.zip","w",zipfile.ZIP_DEFLATED) as archive:
    for name,path in sorted(files.items()): archive.write(path,name)
(HERE/"results.json").write_text(json.dumps({"baseline_commit":"815454b5",
    "groups":groups,"runs":records,"images":images,"zero_failure_counters":checks,
    "binary_sha256":hashes,"source_sha256":source_hashes,
    "default_check":{"cases":sorted(scenes),"images_per_case":3,"byte_identical_to_opt_in":True,
                     "no_shadow_batch_override":True,"matching_final_wram":True},
    "note":"Presentation cadence, not emulation FPS; nested scopes are not additive. Three Palace pairs; two navigation/SIM pairs. Mac navigation and both SIM controls are noisy; do not interpret their ratios as causal gains/regressions.",
},indent=2)+"\n")
