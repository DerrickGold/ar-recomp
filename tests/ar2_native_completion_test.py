"""Local-ROM fixtures for AR2 clear/tally/departure and last-life failure.

The clear fixture schedules the boss's recorded death continuation. It proves
the native continuation, not damage/collision or an input-only boss victory.
No completion flag, score, timer or departure request is injected afterward.
"""
import argparse
from contextlib import nullcontext
import csv
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile


BOUNDARIES = ("82ef69", "829b55", "829c7f", "829d5c", "828875",
              "829e23", "829e47", "808944", "829e49", "829eb0")
FAILURE_BOUNDARIES = ("828c7a", "828c96")


def word(data, address):
    assert len(data) == 0x20000
    return int.from_bytes(data[address:address + 2], "little")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--output", type=Path, help="Retain evidence in a new directory")
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.output.resolve()) if args.output else tempfile.TemporaryDirectory(prefix="ar2-clear-")
    with context as directory:
        root = Path(directory)

        def run(name, margin, rows, view, frames, fixture):
            folder = root / name
            folder.mkdir()
            commands = [f"0 artifacts {folder}/exit", f"0 rows {rows}", f"0 view {view}"]
            commands += [f"0 observe-block {pc}" for pc in BOUNDARIES + FAILURE_BOUNDARIES]
            script = folder / "input.txt"
            script.write_text("\n".join(commands) + "\n" + fixture.replace("@", str(folder)))
            result = subprocess.run([str(args.probe.resolve()), str(args.rom.resolve()), "14",
                                     str(frames), str(margin), str(script)],
                                    capture_output=True, text=True, timeout=120)
            (folder / "trace.csv").write_text(result.stdout)
            (folder / "stderr.log").write_text(result.stderr)
            assert result.returncode == 0, f"{name}: {result.stderr[-4000:]}"
            observations = [json.loads(line.removeprefix("native observation "))
                            for line in result.stderr.splitlines() if line.startswith("native observation {")]
            counts = {pc: int(count) for pc, count in re.findall(
                r"native observation count ([0-9a-f]+): (\d+)", result.stderr)}
            trace = list(csv.DictReader(io.StringIO(result.stdout)))
            assert trace[-1]["event"] == "1", f"{name}: no terminal world return"
            # The harness also verifies repeated terminal delivery and reopen.
            assert "Reopened entrance 14 after terminal return" in result.stderr
            state = (folder / "exit.wram.bin").read_bytes()
            assert word(state, 0x518) == 0x0a
            print(f"{name}: native world return at frame {trace[-1]['frame']}", flush=True)
            return folder, observations, counts, trace, state

        clear = """550 write 518 15
1300 write 971 2
1300 write 975 10
1300 write 90c 1
1300 snapshot @/before
1300 write 11064 ef69
1300 write 11066 82
1300 write 106e 0
1300 write 12084 0
1300 write 1205e 7fff
"""
        baseline = None
        for name, margin, rows, view in (("native", 0, 0, 0), ("wide", 128, 64, 0),
                                         ("diorama", 128, 64, 1)):
            folder, observations, counts, trace, state = run(name, margin, rows, view, 3800, clear)
            before = (folder / "before.wram.bin").read_bytes()
            # These allocation values belong only to this fresh-boot fixture.
            assert word(before, 0x51a) == 0x15 and word(before, 0x951) == 1
            assert word(before, 0x659) == 0x1060
            assert word(before, 0x12084) == 0xef69 and word(before, 0x12086) == 0x82
            assert word(before, 0x13074) == 0x14
            assert before[0x90c:0x911] == bytes((1, 0, 0, 0, 0))
            assert tuple(o["pc"] for o in observations) == BOUNDARIES, observations
            assert all(counts[pc] == 1 for pc in BOUNDARIES)
            assert all(counts[pc] == 0 for pc in FAILURE_BOUNDARIES)
            assert all((o["record"], o["record_offset"]) == (2, 0x10) for o in observations)
            assert observations[0]["control"] == 1
            assert all(o["control"] == 0 and o["boss"] == 0 for o in observations[1:])
            by_pc = {o["pc"]: o for o in observations}
            writer, written = by_pc["829e23"], by_pc["829e47"]
            assert (writer["db"], writer["d"], writer["m"], writer["x_flag"]) == (0x81, 0, 0, 0)
            assert writer["progress"] == [1, 0, 0, 0, 0]
            assert written["progress"] == [3, 0, 0, 0, 0]
            assert writer["time"] == 0 and writer["score"] != [0, 0]
            assert by_pc["808944"]["transition"] == 0
            for pc in ("829e49", "829eb0"):
                assert by_pc[pc]["transition"] == 0x0a
                assert by_pc[pc]["source_request"] & 0x8000
            assert state[0x90c:0x911] == bytes((3, 0, 0, 0, 0))
            assert word(state, 0x95b) == 0 and word(state, 0x97b) == 0x8014
            # Compare actual native observations, including frame and CPU state,
            # across presentation requests. Extra pixels must not advance clears.
            signature = (observations, counts, trace[-1]["frame"])
            if baseline is None:
                baseline = signature
            else:
                assert signature == baseline, f"{name}: presentation changed native lifecycle"

        _, observations, counts, _, state = run("last_life", 128, 64, 1, 1200, """550 write 971 2
550 write 975 10
550 write 90c 1
550 write 91b 1
550 write 91d 0
""")
        assert tuple(o["pc"] for o in observations) == FAILURE_BOUNDARIES
        assert all(counts[pc] == 0 for pc in BOUNDARIES)
        assert all(counts[pc] == 1 for pc in FAILURE_BOUNDARIES)
        assert state[0x90c:0x911] == bytes((1, 0, 0, 0, 0))
        # Bit $8000 is a return marker in BOTH routes. Last-life failure sets
        # $C000 at $82:8C96; observing $8000 alone cannot establish success.
        assert word(state, 0x97b) == 0xc014
        print("AR2 native clear/failure boundaries agree across all three views")


if __name__ == "__main__":
    main()
