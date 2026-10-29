"""Probe the USA AR2 action entrances using the game's native objects.

This is an investigation sweep, not a full-level completion test. It uses only
controller inputs (walk, jump, swing, pause/resume), without position, health or
task writes. The probe and game use the same entrance and capture policy.
"""
import argparse
import concurrent.futures
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
from ar2_metadata import ROM_SHA256, scene_profiles


NATIVE_FIELDS = ("frame", "scene", "pending", "x", "y", "health", "lives", "event",
                 "entry_phase", "control", "boss_actor", "boss_health",
                 "native_camera_x", "native_camera_y", "native_bg2_x", "native_bg2_y")
MODES = {"native": [], "wide": ["extra 128", "rows 64"],
         "diorama": ["extra 51", "rows 64", "view 1"]}
# This controller route crosses Almetha's first doorway at native $84:B10F.
# Accept the documented destination; unrelated scene changes still fail.
PROFILES = scene_profiles()
CONNECTED_ROOMS = {entrance: tuple(scene for scene, owner in PROFILES.items()
                                 if owner == entrance and scene != entrance)
                   for entrance in PROFILES.values()}


def entrances(rom):
    records = (1, 2, 3, 4, 5, 6, 8, 10, 11, 21, 14, 17, 18, 13)
    for caption, record in enumerate(records):
        scene = struct.unpack_from("<H", rom, 0x18000 + record * 16 + 8)[0] & 255
        if PROFILES.get(scene) != scene:
            raise ValueError(f"native world entrance {scene:02x} has no scene profile")
        label = rom[0x181B5 + caption * 21:0x181B5 + caption * 21 + 14]
        yield scene, label.decode("ascii").strip().replace("x", "'")


def run_case(args, scene, label, mode, required_blocks=()):
    folder = args.output / f"{scene:02x}-{mode}"
    folder.mkdir()  # Preserve earlier evidence instead of overwriting it.
    script = folder / "input.txt"
    events = [(0, command) for command in MODES[mode]]
    events += [(0, f"watch-block {pc:06x}") for pc in required_blocks]
    events += [(0, f"artifacts {folder}/exit"), (0, f"timing {folder}/timing.csv"),
               (550, "refresh"), (600, "input 80"), (660, "input 81"), (680, "input 80"),
               (700, "input 82"), (710, "input 80"), (780, "input 0"),
               (810, "input 8"), (820, "input 0"),
               (820, "refresh"), (820, f"dump {folder}/paused.wram.bin"),
               (830, f"dump {folder}/still-paused.wram.bin"),
               (840, "input 8"), (850, "input 0"),
               (850, f"dump {folder}/resumed.wram.bin")]
    events += [(args.frames - 1, f"require {pc:06x}") for pc in required_blocks]
    events.sort(key=lambda event: event[0])
    script.write_text("".join(f"{tick} {command}\n" for tick, command in events
                              if tick < args.frames))
    command = [str(args.probe), str(args.rom), f"{scene:x}", str(args.frames), "0", str(script)]
    with (folder / "trace.csv").open("w") as stdout, (folder / "stderr.log").open("w") as stderr:
        try:
            result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=args.timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = None
    log = (folder / "stderr.log").read_text(errors="replace")
    rows = list(csv.DictReader(io.StringIO((folder / "trace.csv").read_text())))
    active = [row for row in rows if int(row["scene"], 16) == scene and
              row["control"] == "1" and int(row["health"]) > 0]
    captured = [row for row in active if row["level"] == "1"]
    motion = [int(row["x"]) for row in active if 600 <= int(row["frame"]) <= 780]
    moved = bool(motion and max(motion) - min(motion) > 8)
    miss = re.search(r"Uncovered AR2 code ([0-9a-f]+) -> ([0-9a-f]+) \(M(\d) X(\d)\)", log)
    failure_frame = re.search(r" at frame (\d+)", log)
    allowed_scenes = (scene, *CONNECTED_ROOMS.get(scene, ()))
    survived = bool(rows and int(rows[-1]["scene"], 16) in allowed_scenes and
                    rows[-1]["control"] == "1" and int(rows[-1]["health"]) > 0 and
                    rows[-1]["lives"] == "3" and rows[-1]["event"] == "0")
    paused = resumed = None
    if args.frames > 850 and code == 0:
        paths = [folder / f"{name}.wram.bin" for name in ("paused", "still-paused", "resumed")]
        paused = resumed = False
        if all(path.exists() and path.stat().st_size == 0x20000 for path in paths):
            snapshots = [path.read_bytes() for path in paths]
            pause_flags = [struct.unpack_from("<H", ram, 0x5ff)[0] for ram in snapshots]
            ticks = [struct.unpack_from("<H", ram, 0x30)[0] for ram in snapshots]
            paused = bool(pause_flags[0] & pause_flags[1] & 0x1000) and ticks[0] == ticks[1]
            resumed = not bool(pause_flags[2] & 0x1000) and ticks[2] != ticks[1]
    status = ("timeout" if code is None else "native_failure" if code else
              "no_control" if not active else "no_separated_capture" if not captured else
              "no_movement" if not moved else "interrupted" if not survived else
              "pause_failure" if paused is False or resumed is False else "entrance_smoke_ok")
    trace = [[row[key] for key in NATIVE_FIELDS] for row in rows]
    scene_path = []
    for row in rows:
        if int(row["scene"], 16) >= 0x14 and (not scene_path or row["scene"] != scene_path[-1]):
            scene_path.append(row["scene"])
    report = {"scene": f"{scene:02x}", "label": label, "mode": mode, "status": status,
              "scene_path": scene_path,
              "returncode": code, "last_frame": int(rows[-1]["frame"]) if rows else None,
              "failure_frame": int(failure_frame[1]) if failure_frame else None,
              "last_state": rows[-1] if rows else None,
              "control_frame": int(active[0]["frame"]) if active else None,
              "separated_capture": bool(captured), "moved": moved,
              "paused": paused, "resumed": resumed,
              "audible": any(int(row["audio_energy"]) for row in active),
              "missing_dispatch": miss.groups() if miss else None,
              "snapshot": all((folder / f"exit.{suffix}").exists() for suffix in
                              ("cpu.json", "ppu.json", "wram.bin", "vram.bin", "cgram.bin",
                               "oam.bin", "highoam.bin")),
              "native_trace_sha256": hashlib.sha256(json.dumps(trace).encode()).hexdigest(),
              "artifacts": str(folder)}
    (folder / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    detail = f"{miss[1]} -> {miss[2]}" if miss else f"control={report['control_frame']}"
    end_frame = report["failure_frame"] if failure_frame else report["last_frame"]
    print(f"{scene:02x} {label:14} {mode:7} {status:22} frame={end_frame} {detail}",
          flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--scene", action="append", type=lambda value: int(value, 16))
    parser.add_argument("--mode", action="append", choices=MODES)
    parser.add_argument("--frames", type=int, default=901)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.frames <= 0 or args.jobs <= 0 or args.timeout <= 0:
        parser.error("frames, jobs and timeout must be positive")
    args.probe, args.rom, args.output = (p.resolve() for p in (args.probe, args.rom, args.output))
    if any(c.isspace() for c in str(args.output)):
        parser.error("probe script artifact paths cannot contain whitespace")
    rom = args.rom.read_bytes()
    if hashlib.sha256(rom).hexdigest() != ROM_SHA256:
        parser.error("requires the unmodified, headerless USA AR2 ROM")
    catalog = dict(entrances(rom))
    if args.scene and set(args.scene) - catalog.keys():
        parser.error("scene must be an action entrance from the native world catalog")
    args.output.mkdir(parents=True, exist_ok=True)
    modes = list(dict.fromkeys(args.mode or MODES))
    cases = [(scene, label, mode) for scene, label in catalog.items()
             if not args.scene or scene in args.scene for mode in modes]
    reports = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_case, args, *case) for case in cases]
        for future in concurrent.futures.as_completed(futures):
            reports.append(future.result())
    reports.sort(key=lambda row: (row["scene"], row["mode"]))
    native = {row["scene"]: row for row in reports if row["mode"] == "native"}
    for row in reports:
        reference = native.get(row["scene"])
        row["matches_native_trace"] = (row["native_trace_sha256"] == reference["native_trace_sha256"]
                                        if reference and row["returncode"] == reference["returncode"] == 0
                                        else None)
        (Path(row["artifacts"]) / "result.json").write_text(json.dumps(row, indent=2) + "\n")
    summary = {"rom_sha256": ROM_SHA256, "frames": args.frames,
               "scenario": "entrance, walk, jump, swing, pause/resume; no RAM fixtures",
               "cases": reports}
    (args.output / "report.json").write_text(json.dumps(summary, indent=2) + "\n")
    counts = {status: sum(row["status"] == status for row in reports)
              for status in sorted({row["status"] for row in reports})}
    print(json.dumps(counts), flush=True)
    print(f"Report: {args.output / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
