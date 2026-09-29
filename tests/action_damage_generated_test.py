#!/usr/bin/env python3
"""Optional local-ROM enemy damage regression using production generated code.

Requires a current Unix Makefiles game build (play preset). Relinks its objects
with a test main; all files stay in the output directory. --oracle-helper can
point to the private regional_native_probe.py to compare against Snes9x running
the original US routines. That optional oracle is not a checkout dependency.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLAYER, BEAM, VICTIM = 0x8A0, 0x8E0, 0x12E0


def word(memory, at):
    return int.from_bytes(memory[at:at + 2], "little")


def put(memory, at, value):
    memory[at:at + 2] = (value & 65535).to_bytes(2, "little")


def build(build_dir, out):
    flags = (build_dir / "CMakeFiles/ActRaiserRecomp.dir/flags.make").read_text()
    opts = []
    for key in ("C_DEFINES", "C_INCLUDES", "C_FLAGS"):
        opts += shlex.split(re.search(r"^" + key + r" = (.*)$", flags, re.M)[1])
    compiler = re.search(r"^CMAKE_C_COMPILER:FILEPATH=(.*)$",
                         (build_dir / "CMakeCache.txt").read_text(), re.M)[1]
    obj = out / "action_damage_callers.o"
    subprocess.run([compiler, *opts, "-c", str(ROOT / "tests/fixtures/action_damage_callers.c"),
                    "-o", str(obj)], check=True)
    link = shlex.split((build_dir / "CMakeFiles/ActRaiserRecomp.dir/link.txt").read_text())
    link[link.index("CMakeFiles/ActRaiserRecomp.dir/src/main.c.o")] = str(obj)
    link[link.index("-o") + 1] = str(out / "action-damage-callers")
    subprocess.run(link, cwd=build_dir, check=True)


class Audit:
    def __init__(self, out, rom, oracle):
        self.out, self.rom, self.oracle = out, rom, oracle
        self.rows = []
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("AR_", "SNESRECOMP_"))}

    def run(self, memory, entry=0x8A3C, x=0, y=0, a=0, p=0, repeat=1, label=""):
        (self.out / "input.wram").write_bytes(memory)
        args = [str(self.out / "action-damage-callers"), str(self.rom),
                str(self.out / "input.wram"), str(self.out / "output.wram")]
        result = subprocess.run(args + list(map(str, (entry, x, y, a, p, repeat))),
                                env=self.env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, (label, result.stdout, result.stderr)
        actual = bytearray((self.out / "output.wram").read_bytes())
        if self.oracle:
            native_input = bytearray(memory)
            native_input[0x1F00:0x1F20] = bytes(0x20)
            put(native_input, 0x1F10, repeat)
            native, _ = self.oracle.run("usa", entry, {0: native_input}, x=x, y=y, a=a, db=0,
                                        m=bool(p & 0x20), carry=p & 1, rts=True, repeat=repeat)
            # Compare every gameplay byte. Only hardware stack, oracle result
            # mailbox and its repeat counter are excluded (different callers).
            mismatches = [at for at in range(len(actual))
                          if not 0x1E00 <= at < 0x1F20 and actual[at] != native[at]]
            assert not mismatches, (label, hex(entry), [hex(i) for i in mismatches[:20]])
        self.rows.append(dict(label=label, entry=entry, repeat=repeat, hp=word(actual, VICTIM+44),
                              flags=word(actual, VICTIM+48), timer=word(actual, VICTIM+38),
                              wram_sha256=hashlib.sha256(actual).hexdigest()))
        return actual


def base():
    memory = bytearray(0x20000)
    for at in range(0x6A0, 0x1AA0, 64):
        put(memory, at, 0x4000)
    put(memory, 0x1AA0, 0x8000)
    put(memory, 0x8A, PLAYER)
    return memory


def actor(memory, at, x, y, flags):
    fields = {0: 0, 2: x, 4: y, 18: 0x9CF1, 22: 0x8000, 24: 6,
              42: 2, 48: flags, 50: 0x9810}
    for offset, value in fields.items():
        put(memory, at + offset, value)


def enemy(memory, x=152, width=16, hp=32, flags=0x4000, at=VICTIM):
    actor(memory, at, x, 100, flags)
    for offset, value in {10: width, 12: 40, 14: width, 16: 8, 36: 100, 44: hp}.items():
        put(memory, at + offset, value)


def beam(audit, state=19, flip=0):
    memory = base()
    actor(memory, BEAM, 100, 100, 1)
    put(memory, BEAM + 40, flip)
    put(memory, BEAM + 58, PLAYER)
    memory = audit.run(memory, 0x9CF2, x=PLAYER, y=BEAM, a=state, label="beam birth")
    assert word(memory, BEAM+42) == 2
    assert word(memory, BEAM+36) == 31
    assert word(memory, BEAM+32) == (0x99E8 if state == 19 else 0x9A17)
    assert word(memory, BEAM+6) == (65528 if flip else 8)
    return memory


def suite(audit):
    # Authored six-part beam: signed bounds, both poses, both horizontal facings.
    beams = {(state, flip): beam(audit, state, flip)
             for state in (19, 20) for flip in (0, 0x4000)}
    for (state, flip), template in beams.items():
        for x in range(24, 201, 4):
            memory = template[:]
            enemy(memory, x=x)
            result = audit.run(memory, p=0x20, label=f"geometry {state}/{flip}/{x}")
            hp = word(result, VICTIM+44)
            assert hp in (30, 32)
            center = (60 if state == 19 else 52) if flip else (140 if state == 19 else 148)
            if x == center:
                assert hp == 30
            if x in (24, 200):
                assert hp == 32
            assert bool(word(result, VICTIM+48) & 8) == (hp == 30)
            assert word(result, VICTIM+38) == (8 if hp == 30 else 0)
    template = beams[19, 0]
    for p in (0, 0x20):
        memory = template[:]
        enemy(memory)
        result = audit.run(memory, p=p, repeat=2, label="repeat collision without update")
        assert word(result, VICTIM+44) == 30
        assert word(result, VICTIM+38) == 8

    # Ordinary sword 1; powered sword 2. Adding the beam cannot stack damage
    # in the same pass, regardless of which attacker comes first in the pool.
    for attack in (1, 2):
        memory = base()
        actor(memory, PLAYER, 100, 100, 1)
        put(memory, PLAYER+42, attack)
        put(memory, PLAYER+26, 8)
        put(memory, PLAYER+28, 1)
        memory = audit.run(memory, 0x8E2F, x=PLAYER, label="sword active pose")
        enemy(memory)
        result = audit.run(memory, label=f"sword attack {attack}")
        assert word(result, VICTIM+44) == 32-attack
        if attack == 2:
            for beam_at in (0x6A0, BEAM):
                both = memory[:]
                both[beam_at:beam_at+64] = template[BEAM:BEAM+64]
                result = audit.run(both, label=f"sword plus beam at {beam_at:x}")
                assert word(result, VICTIM+44) == 30
                assert word(result, VICTIM+38) == 8

    # Every early victim/status rejection bit, plus post-contact deflection.
    for flags in (1, 8, 0x20, 0x400, 0x2000, 0x800):
        memory = template[:]
        enemy(memory, flags=0x4000 | flags)
        result = audit.run(memory, label=f"victim flag {flags:x}")
        assert word(result, VICTIM+44) == 32
        assert word(result, VICTIM+48) == 0x4000 | flags
    for status in (0x400, 0x800, 0x2000, 0x4000):
        memory = template[:]
        enemy(memory)
        put(memory, VICTIM, status)
        result = audit.run(memory, label=f"victim status {status:x}")
        assert word(result, VICTIM+44) == 32
    for flags in (0, 1 | 8, 1 | 0x40):
        memory = template[:]
        enemy(memory)
        put(memory, BEAM+48, flags)
        result = audit.run(memory, label=f"attacker flags {flags:x}")
        assert word(result, VICTIM+44) == 32

    # Lethal HP clamps and death handling; ordinary-enemy score is awarded once.
    for boss in (False, True):
        for hp in (0, 1, 2, 3):
            memory = template[:]
            enemy(memory, hp=hp, flags=0x4000 if boss else 0)
            put(memory, VICTIM+46, 0x20)
            result = audit.run(memory, repeat=2, label=f"lethal boss={boss} hp={hp}")
            assert word(result, VICTIM+44) == max(0, hp-2)
            if hp <= 2:
                assert word(result, VICTIM+18) == (0xA54A if boss else 0xA382)
                assert word(result, 0x1F) == (0 if boss else 0x20)

    # Piercing is per victim, not one damage budget shared across the projectile.
    memory = template[:]
    enemy(memory)
    enemy(memory, at=VICTIM+64)
    result = audit.run(memory, label="two victims")
    assert word(result, VICTIM+44) == word(result, VICTIM+64+44) == 30

    # Native object loop owns cooldown and beam movement. A wide stationary
    # target still overlaps at update 9: the same beam then deals another 2.
    # These are controlled geometry fixtures, not complete boss playthroughs.
    for width, center, expected in ((16, 152, [30]*20),
                                    (64, 192, [30]*9 + [28]*11)):
        memory = template[:]
        enemy(memory, x=center, width=width)
        for frame in range(20):
            if frame:
                memory = audit.run(memory, 0x8915, p=0x20, label=f"update {width}/{frame}")
            memory = audit.run(memory, p=0x20, label=f"frame collision {width}/{frame}")
            assert word(memory, VICTIM+44) == expected[frame], (width, frame)
            if frame <= 8:
                assert word(memory, VICTIM+38) == 8-frame

    for timer, flags in ((0, 1), (31, 0x401)):
        memory = template[:]
        put(memory, BEAM+36, timer)
        put(memory, BEAM+48, flags)
        memory = audit.run(memory, 0x9D1C, x=BEAM, label="beam retirement")
        assert word(memory, BEAM) == 0x4000
        enemy(memory)
        memory = audit.run(memory, label="retired beam cannot hit")
        assert word(memory, VICTIM+44) == 32


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-release")
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--oracle-helper", type=Path)
    args = parser.parse_args()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="ar-damage-"))
    out.mkdir(parents=True, exist_ok=True)
    oracle = None
    if args.oracle_helper:
        spec = importlib.util.spec_from_file_location("damage_native_oracle", args.oracle_helper)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        assert args.rom.resolve() == module.ROOT / module.HASHES["usa"][0]
        oracle = module.Oracle()
    try:
        build(args.build.resolve(), out)
        audit = Audit(out, args.rom.resolve(), oracle)
        suite(audit)
        report = dict(calls=len(audit.rows), original_rom_comparison=bool(oracle),
                      rom_sha256=hashlib.sha256(args.rom.read_bytes()).hexdigest(),
                      test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      fixture_sha256=hashlib.sha256(
                          (ROOT / "tests/fixtures/action_damage_callers.c").read_bytes()).hexdigest(),
                      rows=audit.rows)
        if oracle:
            report["oracle_helper_sha256"] = hashlib.sha256(
                args.oracle_helper.read_bytes()).hexdigest()
            report["oracle_core_sha256"] = hashlib.sha256(
                (module.ROOT / "tools/oracle/snes9x_libretro.dylib").read_bytes()).hexdigest()
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"PASS {len(audit.rows)} generated calls; original-ROM comparison={bool(oracle)}")
        print(f"Evidence: {out / 'results.json'}")
    finally:
        if oracle:
            oracle.close()


if __name__ == "__main__":
    main()
