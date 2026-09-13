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
import signal
import time
import platform

ROOT = Path(__file__).resolve().parents[1]
RENDER_STAGES = (
    "PPU + capture", "world map build", "SIM metadata", "town canvas",
    "frame snapshot", "upload", "presentation",
)


def summarize_log(log: str, scene: str, windows: int = 5, map_id: str | None = None) -> dict:
    """Weight samples by presents; reject short/missing or malformed evidence."""
    samples = []
    for block in log.split("[pipeline-perf]")[1:]:
        if not block.startswith(f" scene={scene} "):
            continue
        if map_id is not None and not re.search(
                rf" map={re.escape(map_id)}(?:\s|$)", block.splitlines()[0]):
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


def validate_run_completion(log: str, expected_ticks: int) -> dict:
    """A zero process exit code is not proof that the graphics run completed."""
    failure = re.search(r"\bVK_ERROR_[A-Z_]+\b|Wayland display connection closed", log)
    if failure:
        raise ValueError(f"Graphics failure ({failure[0]}); discard comparison")
    cadence = re.findall(
        r"\[present-cadence\] tick-presents=(\d+) re-presents=(\d+)(?:\s|$)", log)
    # This harness forces headless-video: exactly one tick/present per loop,
    # with no idle frame-generation re-presents. Early replay/window exits
    # can otherwise leave enough settled samples and a successful exit code.
    if not cadence or tuple(map(int, cadence[-1])) != (expected_ticks, 0):
        raise ValueError("Incomplete headless presentation schedule; discard comparison")
    return {"tick_presents": expected_ticks, "re_presents": 0}


def run_evidence(log: str) -> dict:
    match = re.search(r"\[run-dir\] (runs/\d+-\d+(?:-\d+)?)(?:\s|$)", log)
    if not match:
        raise ValueError("Missing run bundle")
    run_dir = ROOT / match[1]
    return {"run_dir": str(run_dir),
            "final_wram_sha256": digest(run_dir / "dump_wram.bin")}


def capture_evidence(run_dir: Path, start: int, end: int, every: int) -> dict:
    expected = {f"shot_{frame}.ppm" for frame in range(start, end + 1)
                if frame % every == 0}
    images = {path.name: digest(path) for path in run_dir.glob("shot_*.ppm")}
    if not expected or images.keys() != expected:
        raise ValueError("Composite capture schedule incomplete or unexpected")
    return images


def verify_runs(results: list[dict], captures: bool = False) -> None:
    if not results or len({r["final_wram_sha256"] for r in results}) != 1:
        raise ValueError("Simulation state differs; discard comparison")
    if captures and any(r["images"] != results[0]["images"] for r in results[1:]):
        raise ValueError("Final composite pixels differ; inspect captures")


def deck_resources() -> dict:
    fields = {line.split(':')[0]: int(line.split()[1]) * 1024
              for line in Path('/proc/meminfo').read_text().splitlines()}
    device = Path('/sys/class/drm/card0/device')
    gpu = sum(int((device / name).read_text())
              for name in ('mem_info_vram_used', 'mem_info_gtt_used'))
    temperatures = list(device.glob('hwmon/hwmon*/temp1_input'))
    return {'available_bytes': fields['MemAvailable'], 'gpu_bytes': gpu,
            'temperature_c': int(temperatures[0].read_text()) / 1000,
            'cpu0_khz': int(Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq').read_text())}


def deck_platform() -> dict:
    files = [Path('/etc/os-release'),
             Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'),
             Path('/sys/class/drm/card0/device/power_dpm_force_performance_level')]
    for pattern in ('/sys/class/drm/card0/device/hwmon/hwmon*/power1_cap',
                    '/sys/class/power_supply/*/online',
                    '/sys/class/power_supply/*/capacity',
                    '/sys/class/power_supply/*/status'):
        files.extend(Path('/').glob(pattern.lstrip('/')))
    return {'machine': platform.machine(), 'kernel': platform.release(),
            'files': {str(path): path.read_text().strip() for path in files},
            'display_environment': {name: os.environ.get(name) for name in
                ('XDG_RUNTIME_DIR', 'WAYLAND_DISPLAY', 'LD_LIBRARY_PATH')},
            'library_and_installed_binary_sha256': {str(path): digest(path) for path in (
                Path('/home/deck/argame/libSDL3.so.0.4.14'),
                Path('/home/deck/argame/libSDL3_ttf.so.0.2.2'),
                Path('/home/deck/argame/ActRaiserRecomp'))}}


def check_competing_processes() -> None:
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            argv = (proc / 'cmdline').read_bytes().split(b'\0')
            name = Path(os.fsdecode(argv[0])).name
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if name in {'ActRaiserRecomp', 'control', 'candidate', 'baseline',
                    'ctest', 'cmake', 'ninja', 'deckbench-cpu', 'deckbench-gpu'}:
            raise RuntimeError(f'Competing game/build/test: PID {proc.name} {name}')


def run_guarded(command, *, cwd, env, log, timeout, guard_path) -> dict:
    check_competing_processes()
    initial = deck_resources()
    if initial['available_bytes'] < 8 * 1024**3:
        raise RuntimeError('Insufficient initial memory headroom')
    minimum_available = initial['available_bytes']
    peak_gpu = initial['gpu_bytes']
    peak_temperature = initial['temperature_c']
    reason = None
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=cwd, env=env, stdout=log,
                               stderr=subprocess.STDOUT, start_new_session=True)
    try:
        while process.poll() is None:
            current = deck_resources()
            minimum_available = min(minimum_available, current['available_bytes'])
            peak_gpu = max(peak_gpu, current['gpu_bytes'])
            peak_temperature = max(peak_temperature, current['temperature_c'])
            if minimum_available < 6 * 1024**3 or peak_gpu - initial['gpu_bytes'] > 2 * 1024**3:
                reason = 'Memory guard'
            if time.monotonic() - started > timeout:
                reason = 'Timeout'
            if reason:
                break
            time.sleep(0.1)
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        result = {'initial': initial, 'final': deck_resources(),
                  'minimum_available_bytes': minimum_available,
                  'peak_gpu_bytes': peak_gpu, 'peak_temperature_c': peak_temperature,
                  'seconds': time.monotonic() - started,
                  'returncode': process.returncode, 'abort_reason': reason}
        guard_path.write_text(json.dumps(result, indent=2) + '\n')
    if process.returncode != 0 or reason:
        raise RuntimeError(f'Guarded run failed: {result}')
    check_competing_processes()
    return result


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
    parser.add_argument("--control-scene", help="Previous label when comparing an instrumentation rename")
    parser.add_argument("--map", dest="map_id", help="Exact hexadecimal group/room, e.g. 04/04")
    parser.add_argument("--quit-frames", type=int, default=1800)
    parser.add_argument("--verify-only", action="store_true",
                        help="Two untimed runs comparing exact composite pixels and WRAM")
    parser.add_argument("--capture-from", type=int, default=400)
    parser.add_argument("--capture-to", type=int, default=1700)
    parser.add_argument("--capture-every", type=int, default=100)
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
    if (args.capture_from < 0 or args.capture_to < args.capture_from or
            args.capture_to > 65535 or args.capture_every < 1 or
            args.capture_to // args.capture_every <
            (args.capture_from + args.capture_every - 1) // args.capture_every):
        parser.error("Capture schedule must include at least one 16-bit game frame")
    checkpoint = json.loads(args.manifest.read_text())["checkpoints"][args.checkpoint]
    map_id = args.map_id or checkpoint.get("map")
    if map_id is not None and not re.fullmatch(r"[0-9a-fA-F]{2}/[0-9a-fA-F]{2}", map_id):
        parser.error("Map must be two hexadecimal bytes, e.g. 04/04")
    if map_id is not None:
        map_id = map_id.lower()
    if args.control_scene and map_id is None:
        parser.error("--control-scene requires a room filter to exclude unrelated scenes")
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
              args.manifest.resolve(), replay, settings, seed_path, Path(__file__).resolve()]
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
    for name in tuple(env):
        if name.startswith("AR_SHOT_"):
            env.pop(name)
    for name in ("AR_INPUT_RECORD", "AR_PERF", "AR_ACTION_PERF", "AR_SIM3D_PERF", "AR_DUMP_EVERY"):
        env.pop(name, None)
    if args.verify_only:
        env.pop("AR_PIPELINE_PERF")
        env.update(AR_SIM3D_CLOUD_DRIFT="0", AR_SHOT_REQUIRE_COMPOSITE="1",
                   AR_SHOT_FROM=str(args.capture_from), AR_SHOT_TO=str(args.capture_to),
                   AR_SHOT_EVERY=str(args.capture_every))
    results = []
    report = {"schema": "actraiser-pipeline-comparison-v1", "scene": args.scene, "map": map_id,
              "platform": deck_platform(),
              "control_scene": args.control_scene or args.scene,
              "input_sha256": hashes, "seed_sha256": seed_hash,
              "environment": {k: v for k, v in env.items() if k.startswith("AR_")},
              "workers": {"control": args.control_workers,
                          "candidate": args.candidate_workers},
              "verification": args.verify_only, "runs": results}
    print(f"Evidence: {output}", flush=True)
    order = (("control", "candidate") if args.verify_only else
             ("control", "candidate", "candidate", "control") * 2)
    for index, variant in enumerate(order):
        if any(digest(Path(path)) != expected for path, expected in hashes.items()):
            raise RuntimeError("Benchmark inputs changed; discard this comparison")
        env["AR_RENDER_WORKERS"] = str(report["workers"][variant])
        isolated_seed.write_bytes(seed)
        isolated_settings.write_bytes(settings.read_bytes())
        log_path = output / f"{index}-{variant}.log"
        with log_path.open("w") as log:
            guard = run_guarded([str(binaries[variant]), str(args.rom.resolve()),
                                 "--config", str(args.config.resolve())],
                                cwd=ROOT, env=env, log=log, timeout=args.timeout,
                                guard_path=output / f"{index}-{variant}-guard.json")
        if any(digest(Path(path)) != expected for path, expected in hashes.items()):
            raise RuntimeError("Benchmark inputs changed during run")
        scene = args.control_scene if variant == "control" and args.control_scene else args.scene
        log = log_path.read_text()
        completion = validate_run_completion(log, args.quit_frames)
        result = run_evidence(log)
        result.update(completion)
        if args.verify_only:
            if "capture=failed" in log or "capture=native-framebuffer" in log:
                raise ValueError("Strict composite capture failed")
            result["images"] = capture_evidence(
                Path(result["run_dir"]), args.capture_from, args.capture_to, args.capture_every)
        else:
            result.update(summarize_log(log, scene, map_id=map_id))
        result.update(variant=variant, log=str(log_path), guard=guard)
        results.append(result)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        message = (f"{len(result['images'])} composite captures" if args.verify_only else
                   f"{result['stages']['render CPU']:.4f} ms render CPU")
        print(f"{index + 1}/{len(order)} {variant}: {message}", flush=True)
    verify_runs(results, captures=args.verify_only)
    if args.verify_only:
        report["summary"] = {"byte_identical_captures": len(results[0]["images"]),
                             "identical_final_wram": True}
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report["summary"]), flush=True)
        return
    report["summary"] = aggregate(results)
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    before = report["summary"]["control"]["render CPU"]["median_ms"]
    after = report["summary"]["candidate"]["render CPU"]["median_ms"]
    print(f"Render CPU median: {before:.4f} -> {after:.4f} ms")
    print("CPU wall scopes only; review ranges and stages, not just the medians.")


if __name__ == "__main__":
    main()
