"""Local-ROM checks that presentation size does not advance native encounters.

Position, matching camera scroll and invulnerability are explicit fixtures.
The encounter scripts, camera locks and subsequent movement execute natively.
The cleanup case selects an existing task's COP boundary to reproduce a play
snapshot. Offscreen cleanup supplies an actor's removal flag and position;
its task PC, removal and draw-list continuation run natively. These fixtures
do not claim an unassisted playthrough.
"""
import argparse
import csv
import io
from pathlib import Path
import subprocess
import tempfile


def word(data, address):
    return int.from_bytes(data[address:address + 2], "little")


def live_actors(data):
    actors = set()
    actor = word(data, 0x87F)
    while actor:
        assert actor <= 0xFFD0 and actor not in actors, "invalid native actor list"
        actors.add(actor)
        actor = word(data, actor + 0x2C)
    return actors


def drawn_actors(data):
    actors = []
    for at in range(0xD00, 0x1000, 2):
        actor = word(data, at)
        if not actor:
            assert len(actors) == len(set(actors)), "duplicate native draw-list actor"
            return set(actors)
        actors.append(actor)
    raise AssertionError("unterminated native draw list")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    probe, rom = args.probe.resolve(), args.rom.resolve()

    with tempfile.TemporaryDirectory(prefix="ar2-activation-") as directory:
        root = Path(directory)

        def run(name, scene, frames, commands):
            folder = root / name
            folder.mkdir()
            script = folder / "input.txt"
            script.write_text(commands.replace("@", str(folder)))
            result = subprocess.run(
                [str(probe), str(rom), f"{scene:x}", str(frames), "0", str(script)],
                capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, f"{name}: {result.stderr[-3000:]}"
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            assert rows, name
            print(f"{name}: reached native frame {rows[-1]['frame']}", flush=True)
            return folder, rows

        # A native sound task already exists in slot $1090. COP $A2 removes
        # it, then RTI must reach the RTL at $829692 rather than dispatch-miss.
        cleanup, rows = run("task_cleanup", 0x14, 601,
                            "0 extra 128\n0 watch 829692\n549 dump @/before.ram\n"
                            "550 write 11094 9690\n550 write 109e 0\n"
                            "600 require 829692\n600 dump @/after.ram\n")
        assert 0x1090 in live_actors((cleanup / "before.ram").read_bytes())
        assert 0x1090 not in live_actors((cleanup / "after.ram").read_bytes())
        assert rows[-1]["health"] == "20" and rows[-1]["control"] == "1"

        # The longer arena replay exposed a stored charged-sword continuation.
        # Exercise it independently of the boss gate using ordinary input.
        _, rows = run("charged_sword_release", 0x14, 1001,
                      "0 extra 128\n0 rows 64\n0 view 1\n"
                      "0 watch 828a59\n0 watch 828f45\n"
                      "550 write 1205e 7fff\n550 input 2\n700 input 0\n"
                      "1000 require 828a59\n1000 require 828f45\n")
        assert rows[-1]["health"] == "20" and rows[-1]["control"] == "1"

        modes = {
            "native": "",
            "wide": "0 extra 128\n0 rows 32\n",
            "diorama": "0 extra 51\n0 rows 64\n0 view 1\n",
            "resized": "500 extra 128\n500 rows 64\n500 view 1\n",
        }
        for mode, settings in modes.items():
            # The draw producer removes offscreen actors bearing flag $0002
            # through COP $A2 at $809843. Its $809845 return must restore Y
            # before following the actor list. Supply only the removal flag
            # and a position outside every view; do not patch task PCs.
            folder, rows = run(f"offscreen_cleanup_{mode}", 0x14, 601,
                               "0 watch 809845\n" + settings +
                               "549 dump @/before.ram\n"
                               "550 write 10f0 600\n550 write 10f4 8402\n"
                               "551 dump @/after.ram\n600 require 809845\n")
            before = (folder / "before.ram").read_bytes()
            after = (folder / "after.ram").read_bytes()
            assert 0x10F0 in live_actors(before) and 0x10F0 not in live_actors(after)
            assert word(before, 0x114C) == 0x10F0 and word(after, 0x114C) == 0x10C0
            # Other actors must survive in the draw list, including the
            # player visited after the removed actor. A lost PLY corrupts
            # the cursor even when task removal itself succeeds.
            assert drawn_actors(after) == drawn_actors(before) - {0x10F0}
            assert 0x1030 in drawn_actors(after)
            assert rows[-1]["health"] == "20" and rows[-1]["control"] == "1"
        approaches = {
            # $82:E577 waits for native camera X >= $490. Start 16 pixels
            # before it, dwell, then walk across it with ordinary input.
            # Continue through both boss attacks: the third projectile's
            # COP $B0 must resume at $EE8D, and COP $AF must return to the
            # attack loop through $EE20. No boss tasks or RNG are patched.
            "industen": (0x14, 2001, 0x480, (0x500, 0x600),
                         "550 write 1030 500\n550 write 1032 270\n"
                         "550 write 538 480\n550 write 540 480\n"
                         "550 write 53c 1d0\n550 write 544 1d0\n"
                         "700 dump @/before.ram\n701 input 80\n750 input 0\n"
                         "2000 require 82ee8d\n2000 require 82ee20\n"
                         "2000 dump @/after.ram\n"),
            # $82:F33B waits for player X >= 808 and Y < 224. Dwell on the
            # platform before it, allowing the upper-room patrols to run,
            # then jump right across the gate with ordinary input.
            "benefic": (0x16, 901, 0x2A7, (0x300, 0x400),
                        "550 write 1030 327\n550 write 1032 e0\n"
                        "550 write 538 2a7\n550 write 540 2a7\n"
                        "550 write 53c 40\n550 write 544 40\n"
                        "700 dump @/before.ram\n701 input 81\n750 input 0\n"
                        "900 require 82d9c5\n900 require 82f985\n"
                        "900 dump @/after.ram\n"),
        }
        # Capture offsets can differ at finite edges. Compare native WRAM
        # scroll, player position and encounter state, not the render camera.
        fields = ("frame", "scene", "pending", "x", "y", "health", "lives",
                  "control", "boss_health", "camera_min_x", "camera_max_x",
                  "native_camera_x", "native_camera_y")
        for stage, (scene, frames, camera_before, bounds_after, approach) in approaches.items():
            baseline = None
            for mode, settings in modes.items():
                watches = ("0 watch 82d9c5\n0 watch 82f985\n" if stage == "benefic"
                           else "0 watch 82ee8d\n0 watch 82ee20\n")
                folder, rows = run(f"{stage}_{mode}", scene, frames,
                                   "0 timing @/timing.csv\n" + watches + settings +
                                   "550 write 1205e 7fff\n" + approach)
                before = (folder / "before.ram").read_bytes()
                after = (folder / "after.ram").read_bytes()
                assert word(before, 0x659) == 0, f"{stage}/{mode}: boss started before gate"
                assert word(before, 0x540) == camera_before
                assert word(before, 0x91D) == word(after, 0x91D) == 20
                assert word(before, 0x951) == word(after, 0x951) == 1
                assert word(after, 0x659) and word(after, 0x953) > 0
                assert (word(after, 0x588), word(after, 0x58C)) == bounds_after
                trace = [tuple(row[key] for key in fields) + (bool(int(row["boss_actor"])),)
                         for row in rows if int(row["frame"]) >= 550]
                assert any(row[-1] for row in trace), "boss event missing from trace"
                assert any(int(row["audio_energy"]) for row in rows
                           if int(row["boss_actor"])), f"{stage}/{mode}: silent boss encounter"
                if stage == "industen":
                    with (folder / "timing.csv").open() as stream:
                        timing = [row for row in csv.DictReader(stream)
                                  if int(row["frame"]) >= 550]
                    upload = max(timing, key=lambda row: int(row["apu_cycles"]))
                    # The native ~32 KB boss sample transfer costs about 1.1M
                    # SPC cycles. An erroneous 4 ms command dwell per byte
                    # made it cost 136M and stall for seconds. Bound emulated
                    # work, not machine-dependent wall time, while requiring
                    # that this replay actually exercised the upload.
                    assert 500_000 < int(upload["apu_cycles"]) < 4_000_000, (
                        f"{stage}/{mode}: unexpected upload work {upload}")
                    assert int(upload["frame"]) < next(
                        int(row["frame"]) for row in rows if int(row["boss_actor"]))
                if baseline is None:
                    baseline = trace
                else:
                    assert trace == baseline, f"{stage}/{mode}: native encounter timing changed"
            print(f"{stage}: identical encounter and camera-lock timing in all views", flush=True)
        print("Native cleanup return and encounter activation passed.")


if __name__ == "__main__":
    main()
