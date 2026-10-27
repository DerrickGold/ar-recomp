"""Local-ROM entrance regressions, including the first whole-catalog coverage gaps.

Require the formerly missing native blocks to execute, then verify the entrance
sequence and compare gameplay across native, widescreen and diorama captures.
This exercises controller input without changing health, position or task RAM.
"""
import argparse
import concurrent.futures
import csv
import hashlib
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ar2_probe_entrances as sweep


# Actual failed dispatch destinations from the initial 14-entrance sweep.
# Shared targets are required in both scenes which independently hit them.
REQUIRED_BLOCKS = {
    0x18: (0x83822E,), 0x1A: (0x8393A3,), 0x1E: (0x83A0C1,),
    0x21: (0x83AAF7,), 0x28: (0x83BF7F,), 0x2B: (0x83D5BE,),
    0x2F: (0x848515,), 0x3A: (0x82D887,), 0x3D: (0x84D7C6,),
    0x41: (0x848515,), 0x51: (0x859E95,),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    args.probe, args.rom = args.probe.resolve(), args.rom.resolve()
    args.frames, args.timeout = 901, 120
    rom = args.rom.read_bytes()
    assert hashlib.sha256(rom).hexdigest() == sweep.ROM_SHA256
    catalog = dict(sweep.entrances(rom))
    assert len(catalog) == 14 and REQUIRED_BLOCKS.keys() <= catalog.keys()
    with tempfile.TemporaryDirectory(prefix="ar2-entrances-") as directory:
        args.output = Path(directory)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(sweep.run_case, args, scene, label, mode,
                                       REQUIRED_BLOCKS.get(scene, ()))
                       for scene, label in catalog.items() for mode in sweep.MODES]
            reports = [future.result() for future in futures]
        native = {row["scene"]: row for row in reports if row["mode"] == "native"}
        for row in reports:
            name = f"{row['scene']} {row['label']} {row['mode']}"
            log = (Path(row["artifacts"]) / "stderr.log").read_text()
            assert row["status"] == "entrance_smoke_ok", f"{name}: {row['status']}\n{log[-3000:]}"
            assert row["snapshot"] and row["audible"] and row["paused"] and row["resumed"], name
            expected_path = ["3a", "34"] if row["scene"] == "3a" else [row["scene"]]
            assert row["scene_path"] == expected_path, f"{name}: unexpected room progression"
            assert row["last_state"]["level"] == "1", f"{name}: final room capture unavailable"
            with (Path(row["artifacts"]) / "trace.csv").open() as trace:
                entry = [sample for sample in csv.DictReader(trace)
                         if sample["scene"] == row["scene"] and
                         int(sample["frame"]) < row["control_frame"]]
            assert entry and all(sample["health"] == "20" and sample["lives"] == "3"
                                 for sample in entry), f"{name}: player hurt before gaining control"
            assert row["native_trace_sha256"] == native[row["scene"]]["native_trace_sha256"], (
                f"{name}: capture mode changed native gameplay")
        print("All 14 entrances: native coverage, control, capture, audio and pause/resume passed")


if __name__ == "__main__":
    main()
