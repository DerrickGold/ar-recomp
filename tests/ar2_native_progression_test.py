"""Opt-in local-ROM regression checks against the production AR2 objects.

Position, health and pending-scene writes are explicit fixtures. These exercise
native transitions/AI, and are not a claim of an unassisted level playthrough.
The narrow-view pixel hash includes the native bottom frame border, which
now stays in the scene instead of the independently scaled HUD. All other
223 rows still match the baseline from before the widescreen hooks. The RAM hash also
includes the corrected common palette/OBJ graphics bootstrap; that correction
only changed palette bytes and the graphics decompression workspace.
"""
import argparse
import csv
import hashlib
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
    # Native $80:BBDD loads OBJ palette 1 from $97:FFD7. An independent
    # Snes9x world-to-level entry confirms START uses this shared palette.
    start_palette = rom.read_bytes()[0x17FFD7:0x17FFF7]

    with tempfile.TemporaryDirectory(prefix="ar2-native-") as directory:
        root = Path(directory)

        def run(name, scene, frames, margin, commands):
            folder = root / name
            folder.mkdir()
            script = folder / "input.txt"
            script.write_text(commands.replace("@", str(folder)))
            result = subprocess.run(
                [str(probe), str(rom), f"{scene:x}", str(frames), str(margin), str(script)],
                capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f"{name}: {result.stderr[-3000:]}")
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            assert rows, name
            print(f"{name}: reached native frame {rows[-1]['frame']}", flush=True)
            return folder, rows

        def word(folder, filename, address):
            data = (folder / filename).read_bytes()
            assert len(data) == 0x20000
            return int.from_bytes(data[address:address + 2], "little")

        def check_entry_control(rows):
            active = [row for row in rows if row["level"] == "1"]
            control_frame = next(int(row["frame"]) for row in active if int(row["control"]))
            entry = [row for row in active if int(row["frame"]) < control_frame]
            assert entry and all(int(row["health"]) == 20 for row in entry)
            # The orb's phase flag clears well before the player can act.
            assert any(row["entry_phase"] == "0" and row["control"] == "0" for row in entry)
            return control_frame

        def check_sleeping_wide_enemy(folder, filename):
            # Benefic's $82:F590 actor at (480,864), visible only in the
            # extended capture, must stay in COP $1E through orb AND stone.
            assert word(folder, filename, 0x951) == 0
            assert word(folder, filename, 0x1390) == 480
            assert word(folder, filename, 0x1392) == 864
            assert not word(folder, filename, 0x1394) & 0x8000, "enemy should remain rendered"
            assert word(folder, filename, 0x11394) == 0xF5C5, "enemy woke before player control"

        def check_start_palette(folder):
            palette = (folder / "start.cgram").read_bytes()
            assert len(palette) == 512
            assert palette[0x120:0x140] == start_palette, (
                f"{folder.name}: START retained a boot-screen palette")

        def terrain(folder):
            header, dimensions, maximum, pixels = (folder / "entrance.ppm").read_bytes().split(b"\n", 3)
            assert header == b"P6" and maximum == b"255"
            width, height = map(int, dimensions.split())
            assert len(pixels) == width * height * 3 and height == 224
            return width, [pixels[y * width * 3:(y + 1) * width * 3] for y in range(height)]

        def check_filled_entrance(narrow, wide):
            nw, native = terrain(narrow)
            ww, expanded = terrain(wide)
            assert nw == 256 and ww == 512
            # At the left room boundary, all 512 columns are real world
            # terrain. Original pixels stay aligned with world x=0, while
            # the last 128 columns are already painted before camera travel.
            # SNES scanout starts at row 1. At Industen's bottom camera,
            # its final scanline reads past the finite map into resident
            # VRAM's wrapped tile row; the world provider correctly leaves
            # that out-of-world guard row transparent.
            assert native[:223] == [row[:256 * 3] for row in expanded[:223]]
            assert sum(value != 0 for row in expanded[160:] for value in row[384 * 3:]) > 2000

        start_capture = "450 palette @/start.cgram\n450 dump @/stone.ram\n"
        entrance_capture = "300 terrain @/entrance.ppm\n300 dump @/orb.ram\n"
        movement = "551 input 80\n650 input 0\n"
        narrow, rows = run("narrow", 0x14, 901, 0, entrance_capture + start_capture + movement +
                           "900 dump @/state.ram\n900 image @/frame.ppm\n")
        check_start_palette(narrow)
        industen_control = check_entry_control(rows)
        for filename, expected in {
            "state.ram": "dc263784d510ac06e5a0f1ed40e886f0ec881e48839ea51f87c8ce04ce63b20f",
            "frame.ppm": "24d583368bb7b0c941b11f248c2698024ba450417d7ecb9e7d2342665f7155c9",
        }.items():
            actual = hashlib.sha256((narrow / filename).read_bytes()).hexdigest()
            assert actual == expected, f"256-pixel baseline changed: {filename}: {actual}"

        wide, rows = run("wide_and_retry", 0x14, 1301, 128,
                         entrance_capture + "400 view 1\n" + start_capture + "500 view 0\n" + movement +
                         "700 view 1\n850 view 0\n900 dump @/before.ram\n"
                         "951 write 91d 0\n1300 dump @/after.ram\n")
        check_start_palette(wide)
        check_filled_entrance(narrow, wide)
        assert check_entry_control(rows) == industen_control
        assert any(int(row["right_objects"]) for row in rows)
        assert word(wide, "before.ram", 0x91b) == 3
        assert word(wide, "after.ram", 0x91b) == 2
        assert word(wide, "after.ram", 0x91d) == 20
        assert word(wide, "after.ram", 0x51a) == 0x14

        narrow, rows = run("benefic_narrow", 0x16, 551, 0,
                        entrance_capture + start_capture + "550 dump @/state.ram\n")
        check_start_palette(narrow)
        benefic_control = check_entry_control(rows)
        wide, rows = run("benefic_wide_and_retry", 0x16, 1001, 128,
                         entrance_capture + "400 view 1\n" + start_capture + "500 view 0\n"
                         "550 dump @/state.ram\n551 write 91d 0\n600 view 1\n"
                         "1000 dump @/after.ram\n")
        check_start_palette(wide)
        check_filled_entrance(narrow, wide)
        assert check_entry_control(rows) == benefic_control
        check_sleeping_wide_enemy(wide, "orb.ram")
        check_sleeping_wide_enemy(wide, "stone.ram")
        assert word(wide, "stone.ram", 0x95b) == 0
        assert word(wide, "state.ram", 0x951) != 0
        assert word(wide, "state.ram", 0x1390) != 480, "enemy did not wake after control"
        # Native death retries skip the orb and enable input immediately;
        # do not introduce a fixed intro delay or leave enemies frozen there.
        assert any(int(row["frame"]) > 550 and row["control"] == "0" for row in rows)
        assert word(wide, "after.ram", 0x951) != 0
        assert word(wide, "after.ram", 0x1390) != 480
        # The native actor at x=304 starts outside the original viewport.
        assert word(narrow, "state.ram", 0x1124) & 0x8000
        assert not word(wide, "state.ram", 0x1124) & 0x8000
        assert any(int(row["right_objects"]) for row in rows)
        assert word(wide, "after.ram", 0x91b) == 2
        assert word(wide, "after.ram", 0x91d) == 20

        paused, rows = run("paused_resize", 0x16, 851, 128,
                          "600 input 8\n610 input 0\n650 dump @/paused.ram\n"
                          "650 snapshot @/snapshot\n650 text @/wide.ppm\n650 hud @/hud.ppm\n"
                          "660 extra 0\n675 text @/narrow.ppm\n680 extra 64\n"
                          "690 text @/middle.ppm\n700 view 1\n710 text @/diorama.ppm\n"
                          "720 dump @/still-paused.ram\n750 input 8\n760 input 0\n"
                          "850 dump @/resumed.ram\n")
        # Hold Start across the native alternating input poll. A one-tick tap
        # can be missed and previously let this test pass without pausing.
        assert word(paused, "paused.ram", 0x5ff) & 0x1000
        assert word(paused, "still-paused.ram", 0x5ff) & 0x1000
        assert word(paused, "paused.ram", 0x30) == word(paused, "still-paused.ram", 0x30)
        assert not word(paused, "resumed.ram", 0x5ff) & 0x1000
        assert word(paused, "resumed.ram", 0x30) > word(paused, "paused.ram", 0x30)
        for name, size in {"wram": 131072, "vram": 65536, "cgram": 512,
                           "oam": 512, "highoam": 32}.items():
            assert (paused / f"snapshot.{name}.bin").stat().st_size == size
        assert (paused / "snapshot.wram.bin").read_bytes() == (paused / "paused.ram").read_bytes()
        import json
        assert json.loads((paused / "snapshot.cpu.json").read_text())["scene"] == 0x16
        assert json.loads((paused / "snapshot.ppu.json").read_text())["bgmode"] & 7 == 1
        for name, width in (("wide", 512), ("narrow", 256), ("middle", 384), ("diorama", 512)):
            _, dimensions, _, pixels = (paused / f"{name}.ppm").read_bytes().split(b"\n", 3)
            assert dimensions == f"{width} 224".encode()
            columns = [i % width for i in range(width * 224)
                       if pixels[i * 3:i * 3 + 3] != b"\xff\x00\xff"]
            assert len(columns) > 200, f"{name}: PAUSE missing"
            assert min(columns) + max(columns) == width - 1, f"{name}: PAUSE is off center"
        _, dimensions, _, pixels = (paused / "hud.ppm").read_bytes().split(b"\n", 3)
        width, _ = map(int, dimensions.split())
        assert pixels[64 * width * 3:] == b"\xff\x00\xff" * (width * 160), "frame border leaked into HUD"

        resized, _ = run("paused_from_native", 0x16, 751, 0,
                         "600 input 8\n610 input 0\n650 image @/narrow.ppm\n"
                         "660 extra 128\n700 view 1\n750 image @/wide.ppm\n")
        narrow_pixels = (resized / "narrow.ppm").read_bytes().split(b"\n", 3)[3]
        wide_pixels = (resized / "wide.ppm").read_bytes().split(b"\n", 3)[3]
        # A paused OAM built at native width must move with the captured
        # scenery when the finite-edge camera offset changes. Compare the
        # original world columns, excluding the independently owned border.
        assert narrow_pixels[:223 * 256 * 3] == b"".join(
            wide_pixels[y * 512 * 3:(y * 512 + 256) * 3] for y in range(223))

        room, rows = run("industen_following_room", 0x14, 1301, 64,
                         "550 write 518 15\n700 view 1\n1300 dump @/state.ram\n")
        assert word(room, "state.ram", 0x51a) == 0x15
        assert word(room, "state.ram", 0x659) != 0
        assert word(room, "state.ram", 0x953) > 0
        assert all(row["level"] == "1" for row in rows if int(row["frame"]) >= 800)
        assert any(int(row["audio_energy"]) for row in rows if int(row["frame"]) >= 800)

        arena, rows = run("benefic_arena", 0x16, 1401, 128,
                          "550 write 1030 340\n550 write 1032 c0\n"
                          "550 write 1205e 7fff\n550 input 82\n700 view 1\n"
                          "1400 dump @/state.ram\n")
        assert word(arena, "state.ram", 0x659) != 0
        assert word(arena, "state.ram", 0x953) > 0
        assert word(arena, "state.ram", 0x588) == 768
        assert all(row["level"] == "1" for row in rows if int(row["frame"]) >= 800)
        assert any(int(row["audio_energy"]) for row in rows if int(row["frame"]) >= 800)
        for scene in (0x14, 0x16):
            _, rows = run(f"last_life_{scene:x}", scene, 1001, 128,
                          "550 write 91b 1\n550 write 91d 0\n")
            assert rows[-1]["event"] == "1"
            assert rows[-1]["pending"] == "0a"
            assert rows[-1]["health"] == "0"
            assert rows[-1]["lives"] == "1"
        print("START palettes, narrow parity, entrance activation, widened actors, resize, retries, "
              "boss activity and world return passed.")


if __name__ == "__main__":
    main()
