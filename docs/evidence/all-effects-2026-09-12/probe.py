#!/usr/bin/env python3
"""Isolated all-effects replay profiling. No installed game/settings mutations.

Default headless runs measure tick work, not gameplay FPS. Use --visible for
normal ticking plus interpolated re-presents and presentation throughput.
Captures and metadata traces are separate untimed runs; clouds keep moving.
"""
import argparse
import base64
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
RENDER = ("PPU + capture", "world map build", "SIM metadata", "town canvas",
          "frame snapshot", "upload", "presentation")
CASES = {
    "sim-held": ("D7-voxel-town", "Town 3D", 2400, {}),
    "sim-low": ("D7-voxel-town", "Town 3D", 2400,
                {"AR_SIM3D_PITCH": "-1350", "AR_SIM3D_DISTANCE": "450"}),
    "sim-effects": ("D0-fillmore-actions", "Town 3D", 3200,
                    {"AR_SIM3D_CAMERA_MODE": "1"}),
    "navigation": ("navigation", "World 3D", 2400, {}),
    "palace": ("palace", "Sky Palace", 2000, {}),
}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")

def prepare(args):
    dest = args.root.resolve()
    dest.mkdir(exist_ok=True)
    manifest = json.loads((ROOT / "tests/fixtures/sim3d/checkpoints.json").read_text())["checkpoints"]
    manifest.update(json.loads(args.world_manifest.read_text())["checkpoints"])
    inputs = dest / "inputs"
    inputs.mkdir(exist_ok=False)
    shutil.copy2(args.binary, inputs / "game")
    shutil.copy2(ROOT / "ar.sfc", inputs / "ar.sfc")
    shutil.copy2(HERE / "profile.json", inputs / "profile.json")
    shutil.copy2(__file__, inputs / "probe-source.py")
    (inputs / "config.ini").write_text("# Isolated compiled defaults; effects pinned in profile.json.\n")
    for name in {c[0] for c in CASES.values()}:
        fixture = manifest[name]
        seed = (ROOT / fixture.get("sram_base64", fixture.get("sram", ""))).read_bytes()
        if "sram_base64" in fixture:
            seed = base64.b64decode(b"".join(seed.split()), validate=True)
        assert len(seed) == 8192 and hashlib.sha256(seed).hexdigest() == fixture["sram_sha256"]
        (inputs / f"{name}.srm").write_bytes(seed)
        shutil.copy2(ROOT / fixture["replay"], inputs / f"{name}.rec")
        shutil.copy2(ROOT / fixture["settings"], inputs / f"{name}.ini")
    write_json(inputs / "hashes.json", {p.name: digest(p) for p in inputs.iterdir() if p.is_file()})
    print(f"Pinned inputs: {inputs}", flush=True)

def summarize(log, scene):
    samples = []
    for block in log.split("[pipeline-perf]")[1:]:
        header = block.splitlines()[0]
        if not header.startswith(f" scene={scene} "):
            continue
        frames = int(re.search(r" frames=(\d+)", header)[1])
        stages = {k: float(v) for k, v in re.findall(
            r"\[pipeline-stage\] (.*?) mean-ms=([\d.]+)", block)}
        assert all(k in stages for k in ("PPU + capture", "presentation"))
        stages["render CPU"] = sum(stages.get(k, 0) for k in RENDER)
        counts = {}
        for category, body in re.findall(r"\[(pipeline-(?:work|traffic|path|atlas))\] ([^\n]+)", block):
            counts.update({f"{category}/{k}": float(v) for k, v in re.findall(r"([\w-]+)=([\d.]+)", body)})
        timing = re.search(r"output=(\d+x\d+).*?cadence-ms=([\d.]+) p95=([\d.]+)", header)
        assert timing
        samples.append(dict(header=header, frames=frames, stages=stages, counts=counts,
                            output=timing[1], cadence_ms=float(timing[2]), p95_ms=float(timing[3])))
    # Preserve the full scene timeline, but omit the first scene-entry window
    # from aggregates. Do not silently select only a cheap idle tail.
    settled = samples[1:]
    assert len(settled) >= 3, f"Too few settled windows for {scene}: {len(settled)}"
    total = sum(s["frames"] for s in settled)
    result = {"frames": total, "windows": len(settled), "samples": samples}
    intervals = sum(s["frames"] - 1 for s in settled)
    result["cadence_ms"] = sum((s["frames"] - 1) * s["cadence_ms"] for s in settled) / intervals
    result["present_fps"] = 1000 / result["cadence_ms"]
    for field in ("stages", "counts"):
        names = set().union(*(s[field] for s in settled))
        result[field] = {k: sum(s["frames"] * s[field].get(k, 0) for s in settled) / total for k in sorted(names)}
    result["peak_window_stages"] = {k: max(s["stages"].get(k, 0) for s in settled) for k in result["stages"]}
    return result

def processes():
    return subprocess.check_output(["ps", "-axo", "pid,ppid,%cpu,comm"], text=True)

def memory():
    available = next(int(line.split()[1]) * 1024 for line in Path("/proc/meminfo").read_text().splitlines()
                     if line.startswith("MemAvailable:"))
    gpu = sum(int(Path(p).read_text()) for pattern in (
        "/sys/class/drm/card[0-9]*/device/mem_info_vram_used",
        "/sys/class/drm/card[0-9]*/device/mem_info_gtt_used") for p in glob.glob(pattern))
    return available, gpu

def platform():
    patterns = ("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor",
                "/sys/class/drm/card[0-9]*/device/power_dpm_force_performance_level",
                "/sys/class/hwmon/hwmon*/temp1_input", "/sys/class/power_supply/BAT*/status")
    return {"uname": list(os.uname()), "sensors": {
        p: Path(p).read_text().strip() for pattern in patterns for p in glob.glob(pattern)}}

def run(args):
    root = args.root.resolve()
    inputs = root / "inputs"
    hashes = json.loads((inputs / "hashes.json").read_text())
    cohort = root / args.cohort
    cohort.mkdir(exist_ok=False)
    profile = json.loads((inputs / "profile.json").read_text())
    overrides = dict(item.split("=", 1) for item in args.set)
    results = []
    order = list(args.cases)
    for repeat in range(args.repeats):
        for name in (order if repeat % 2 == 0 else list(reversed(order))):
            assert all(digest(inputs / p) == h for p, h in hashes.items()), "Changed pinned inputs"
            fixture, scene, ticks, camera = CASES[name]
            ticks = args.frames or ticks
            output = cohort / f"{repeat}-{name}"
            output.mkdir()
            shutil.copy2(inputs / f"{fixture}.srm", output / "seed.srm")
            shutil.copy2(inputs / f"{fixture}.ini", output / "settings.ini")
            env = {k: v for k, v in os.environ.items() if not k.startswith(("AR_", "SNESRECOMP_"))}
            env.update(profile)
            env.update(camera)
            env.update(overrides)
            env.update(AR_HEADLESS="1", AR_HEADLESS_VIDEO="1", AR_ENABLE_RUN_DIR="1",
                       AR_REPLAY_NOSTOP="1", AR_INPUT_REPLAY=str(inputs / f"{fixture}.rec"),
                       AR_SAVE_NATIVE_PATH=str(output / "seed.srm"), AR_SETTINGS_PATH=str(output / "settings.ini"),
                       AR_PERFORMANCE_OVERLAY="Off", AR_REFRESH_MODE="Unlimited", AR_QUIT_FRAMES=str(ticks))
            if args.deck:
                env.update(LD_LIBRARY_PATH="/home/deck/argame", XDG_RUNTIME_DIR="/run/user/1000",
                           WAYLAND_DISPLAY="wayland-0", SDL_VIDEODRIVER="wayland")
                for p in Path("/proc").glob("[0-9]*/comm"):
                    try:
                        assert p.read_text().strip() not in ("game", "ActRaiserRecomp"), "Another game is running"
                    except FileNotFoundError:
                        pass
            if args.visible and not args.visual:
                env.update(AR_HEADLESS="0", AR_HEADLESS_VIDEO="0",
                           AR_WINDOW_MODE=overrides.get("AR_WINDOW_MODE", "Borderless"),
                           AR_WINDOW_SCALE=overrides.get("AR_WINDOW_SCALE", "3"))
            if args.visual:
                env.update(AR_SHOT_REQUIRE_COMPOSITE="1", AR_SHOT_FROM="1200", AR_SHOT_TO="1800", AR_SHOT_EVERY="300")
                if name.startswith("sim"):
                    env["AR_SIM3D_D1_TRACE"] = str(output / "metadata.jsonl")
            else:
                env["AR_PIPELINE_PERF"] = "1"
            write_json(output / "environment.json", {k: v for k, v in env.items() if k.startswith("AR_")})
            (output / "processes-before.txt").write_text(processes())
            before = platform()
            minimum, initial_gpu = memory() if args.deck else (0, 0)
            assert not args.deck or minimum > 6 * 1024**3, "Insufficient free memory"
            growth, abort = 0, None
            telemetry = []
            busy_paths = glob.glob("/sys/class/drm/card[0-9]*/device/gpu_busy_percent") if args.telemetry else []
            started = time.monotonic()
            with (output / "game.log").open("w") as log_file:
                process = subprocess.Popen([str(inputs / "game"), str(inputs / "ar.sfc"),
                    "--config", str(inputs / "config.ini")], cwd=output, env=env,
                    stdout=log_file, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    while process.poll() is None:
                        if args.deck:
                            available, gpu = memory()
                            minimum, growth = min(minimum, available), max(growth, gpu - initial_gpu)
                            if available < 4 * 1024**3 or growth > 2 * 1024**3:
                                abort = "memory guard"
                        if time.monotonic() - started > 150:
                            abort = "timeout"
                        if busy_paths:
                            telemetry.append({"seconds": time.monotonic()-started,
                                              "gpu_busy_pct": {p: int(Path(p).read_text()) for p in busy_paths}})
                        if abort:
                            break
                        time.sleep(.25)
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
            code = process.returncode
            guard = dict(before=before, after=platform(), minimum_available=minimum,
                         maximum_gpu_growth=growth, abort=abort, returncode=code)
            write_json(output / "guard.json", guard)
            if args.telemetry:
                write_json(output / "telemetry.json", telemetry)
            (output / "processes-after.txt").write_text(processes())
            log = (output / "game.log").read_text()
            assert code == 0 and abort is None and not re.search(r"VK_ERROR_[A-Z_]+|Wayland display connection closed|fatal", log, re.I)
            cadence = tuple(map(int, re.findall(r"\[present-cadence\] tick-presents=(\d+) re-presents=(\d+)", log)[-1]))
            if args.visible and not args.visual:
                assert ticks * .9 < cadence[0] <= ticks and cadence[1] > 0, cadence
            else:
                assert cadence == (ticks, 0), cadence
            assert "ordered GPU interop enabled" in log and "[crt] shader ready" in log
            if name.startswith("sim"):
                assert "features=$7eff" in log, "Not all shipped SIM features enabled"
            run_dir = output / re.search(r"\[run-dir\] (runs/\d+-\d+(?:-\d+)?)", log)[1]
            result = dict(case=name, repeat=repeat, seconds=time.monotonic()-started,
                          final_wram_sha256=digest(run_dir / "dump_wram.bin"), run_dir=str(run_dir),
                          tick_presents=cadence[0], represents=cadence[1], guard=guard)
            if args.visual:
                result["images"] = {p.name: digest(p) for p in run_dir.glob("shot_*.ppm")}
                assert len(result["images"]) == 3
            else:
                result.update(summarize(log, scene))
                if args.deck and args.visible:
                    assert {s["output"] for s in result["samples"]} == {"1280x800"}
            results.append(result)
            write_json(cohort / "results.json", results)
            print(json.dumps({k: v for k, v in result.items() if k not in ("samples", "stages", "counts", "peak_window_stages", "guard")}), flush=True)
            if not args.visual:
                print("Top scopes: " + repr(sorted(result["stages"].items(), key=lambda kv: -kv[1])[:12]), flush=True)
    for name in order:
        assert len({r["final_wram_sha256"] for r in results if r["case"] == name}) == 1
    if not args.visual:
        summary = {}
        for name in order:
            runs = [r for r in results if r["case"] == name]
            summary[name] = {field: {k: {"median": statistics.median(r[field].get(k, 0) for r in runs),
                                         "min": min(r[field].get(k, 0) for r in runs),
                                         "max": max(r[field].get(k, 0) for r in runs)}
                                      for k in set().union(*(r[field] for r in runs))}
                             for field in ("stages", "counts", "peak_window_stages")}
        write_json(cohort / "summary.json", summary)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--world-manifest", type=Path)
    parser.add_argument("--cohort", default="baseline")
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--visual", action="store_true")
    parser.add_argument("--deck", action="store_true")
    parser.add_argument("--visible", action="store_true")
    parser.add_argument("--telemetry", action="store_true", help="Optional 4 Hz Deck GPU busy counter; not GPU timestamps")
    parser.add_argument("--frames", type=int)
    parser.add_argument("--set", action="append", default=[])
    args = parser.parse_args()
    prepare(args) if args.prepare else run(args)
