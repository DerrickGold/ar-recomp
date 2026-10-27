"""Local-ROM checks for background parallax at widened foreground camera stops.

Only invulnerability is supplied. Native input, camera motion and layer scroll
must agree across views; animated background artwork remains game-owned.
"""
import argparse
import csv
import io
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    probe, rom = args.probe.resolve(), args.rom.resolve()
    modes = {
        "native": [],
        "wide": [(0, "extra 128")],
        "diorama": [(0, "extra 51"), (0, "rows 64"), (0, "view 1")],
        "resized": [(0, "extra 128"), (640, "extra 43"), (720, "extra 51"),
                    (720, "rows 64"), (720, "view 1")],
    }
    native_fields = ("frame", "x", "y", "native_camera_x", "native_camera_y",
                     "native_bg2_x", "native_bg2_y", "health", "lives", "control")
    with tempfile.TemporaryDirectory(prefix="ar2-scroll-") as directory:
        root = Path(directory)
        # These are the ratios authored by the supported entrances, not a
        # ratio estimated from adjacent frames (which fails while stationary).
        for scene, denominator in ((0x14, 2), (0x16, 1)):
            baseline = None
            for mode, settings in modes.items():
                folder = root / f"{scene:02x}_{mode}"
                folder.mkdir()
                events = settings + [(550, "write 1205e 7fff"), (550, "input 80"),
                                     (1000, "input 0")]
                if scene == 0x14 and mode == "wide":
                    events += [(tick, f"background {folder}/{tick}.ppm")
                               for tick in (600, 630, 660, 690, 720, 750, 800)]
                script = folder / "input.txt"
                script.write_text("".join(f"{tick} {command}\n" for tick, command in
                                          sorted(events, key=lambda event: event[0])))
                result = subprocess.run(
                    [str(probe), str(rom), f"{scene:x}", "1001", "0", str(script)],
                    capture_output=True, text=True, timeout=120)
                assert result.returncode == 0, f"{folder.name}: {result.stderr[-3000:]}"
                rows = [row for row in csv.DictReader(io.StringIO(result.stdout))
                        if int(row["frame"]) >= 550]
                assert rows and rows[-1]["control"] == "1" and rows[-1]["health"] == "20"
                trace = [tuple(row[key] for key in native_fields) for row in rows]
                if baseline is None:
                    baseline = trace
                else:
                    assert trace == baseline, f"{folder.name}: presentation changed native play"
                held_while_native_moved = False
                previous = None
                for row in rows:
                    native = int(row["native_camera_x"])
                    offset = int(row["view_offset_x"])
                    foreground = int(row["camera_x"]) + offset
                    background = int(row["bg2_camera_x"]) + offset
                    assert int(row["native_bg2_x"]) == native // denominator
                    assert background == foreground // denominator, folder.name
                    if previous and foreground == previous[0]:
                        assert background == previous[1], f"{folder.name}: background drift"
                        held_while_native_moved |= native != previous[2]
                    previous = (foreground, background, native)
                if mode in ("wide", "diorama"):
                    assert held_while_native_moved, f"{folder.name}: camera stop not exercised"
                if scene == 0x14 and mode == "wide":
                    # Industen BG2 is static artwork: while its foreground is
                    # held, the actual rendered background must also be stable.
                    held = (folder / "600.ppm").read_bytes()
                    for tick in (630, 660, 690, 720, 750):
                        assert (folder / f"{tick}.ppm").read_bytes() == held
                    assert (folder / "800.ppm").read_bytes() != held
                print(f"{folder.name}: foreground stop and native parallax passed", flush=True)


if __name__ == "__main__":
    main()
