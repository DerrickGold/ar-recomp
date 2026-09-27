#!/usr/bin/env python3
"""Exercise nine regional hooks through the production generated game callers.

Optional local-ROM test; requires a built Unix Makefiles game tree (the play
preset). Relinks copies of five HLE owners with observation counters, retaining
all generated/native callees. Regional snapshots are seeded through their real
session resolvers. Prefix probes exit at the native continuation; this is not a
full stage playthrough. All fixture writes stay in the output directory.
"""

import argparse
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def build(root, build_dir, out):
    flags = (build_dir / "CMakeFiles/ActRaiserRecomp.dir/flags.make").read_text()
    opts = []
    for key in ("C_DEFINES", "C_INCLUDES", "C_FLAGS"):
        opts += shlex.split(re.search(r"^" + key + r" = (.*)$", flags, re.M)[1])
    compiler = re.search(
        r"^CMAKE_C_COMPILER:FILEPATH=(.*)$", (build_dir / "CMakeCache.txt").read_text(), re.M
    )[1]
    link = shlex.split((build_dir / "CMakeFiles/ActRaiserRecomp.dir/link.txt").read_text())
    modules = {
        "actraiser/actraiser_action_motion.c": [
            "ActRaiser_EmitterCadence",
            "ActRaiser_ActionCollisionBirth",
        ],
        "actraiser/actraiser_fire_enemy.c": ["ActRaiser_FireBounce"],
        "actraiser/actraiser_boss_rules.c": [
            "ActRaiser_AntlionVolley",
            "ActRaiser_AntlionDecision",
            "ActRaiser_WizardPause",
        ],
        "actraiser/actraiser_score_lives.c": ["ActRaiser_ScoreLives"],
        "actraiser/regional/actraiser_regional_runtime.c": [
            "ActRaiser_RegionalRecoveryDrain",
            "ActRaiser_RegionalRetry",
        ],
    }
    for module, hooks in modules.items():
        src = out / (Path(module).stem + "_probe.c")
        obj = src.with_suffix(".o")
        text = (
            "".join(f"#define {h} AuditOriginal_{h}\n" for h in hooks)
            + f'#include "{module}"\nvoid AuditHook(const char*,CpuState*);\n'
        )
        text += "".join(
            f'#undef {h}\nRecompReturn {h}(CpuState*c){{AuditHook("{h}",c);return AuditOriginal_{h}(c);}}\n'
            for h in hooks
        )
        if "regional_runtime" in module:
            text += """
    void AuditPolicy(unsigned source, bool active) {
      const uint8_t identity[16]={1}; ArRegionalCostPolicy costs;
      ArRegionalCosts_Init(&costs,kArRegionalSource_US);
      if(!ArRegionalSession_NewGame(&s_campaign.active,0,identity,&costs)){fprintf(stderr,"new game failed\\n");abort();}
      ArRegionalRules rules=s_campaign.active.requested;
      ArRegionalBoss_Init(&rules.bosses,source);
      ArRegionalEmitter_Init(&rules.emitters,source);
      ArRegionalCollision_Init(&rules.collision,source);
      ArRegionalFire_Init(&rules.fire_enemy,source);
      ArRegionalRecovery_Init(&rules.recovery,source);
      rules.retry_score=rules.score_lives=source;
      s_campaign.active.requested=s_campaign.active.effective=rules;
      s_campaign.active_valid=active;
      uint16_t time;
      if(!ArRegionalSession_BeginActionRoom(&s_campaign.active,3,0x300,&time,&s_action)){fprintf(stderr,"room snapshot failed\\n");abort();}
    }
    """
        src.write_text(text)
        subprocess.run([compiler, *opts, "-c", str(src), "-o", str(obj)], check=True)
        link[link.index("CMakeFiles/ActRaiserRecomp.dir/src/" + module + ".o")] = str(obj)
    src = root / "tests/fixtures/regional_hle_callers.c"
    obj = out / "regional_hle_callers.o"
    subprocess.run([compiler, *opts, "-c", str(src), "-o", str(obj)], check=True)
    link[link.index("CMakeFiles/ActRaiserRecomp.dir/src/main.c.o")] = str(obj)
    link[link.index("-o") + 1] = str(out / "regional-hle-callers")
    subprocess.run(link, cwd=build_dir, check=True)


def check(root, out, rom):
    rows = []
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("AR_", "SNESRECOMP_"))
    }
    env["AR_AUDIT_ROM"] = str(rom)
    for kind in range(9):
        values = {
            1: [0, 209, 210, 211, 220, 241, 242, 243, 255],
            2: [0, 63, 64, 65, 2303, 2304, 2432],
            5: [0, 1],
            6: [0, 1, 4, 16],
        }.get(kind, [0])
        for source, value, active in itertools.product(
            range(3), values, [0, 1] if kind in (5, 6) else [1]
        ):
            p = subprocess.run(
                [
                    str(out / "regional-hle-callers"),
                    str(kind),
                    str(source),
                    str(value),
                    str(active),
                ],
                cwd=root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10,
            )
            assert p.returncode == 0, (kind, source, value, active, p.stdout)
            line = next(l for l in p.stdout.splitlines() if l.startswith("RESULT"))
            hexkeys = {"pc", "A", "X", "Y", "S", "P", "score", "resume"}
            r = {
                k: int(v, 16 if k in hexkeys else 10)
                for k, v in (field.split("=") for field in line.split()[1:])
            }
            expected_hooks = (
                source != 0
                if kind == 0
                else source == 1
                if kind in (1, 2, 3, 4, 8)
                else source == 2
                if kind == 7
                else active
            )
            assert r["hooks"] == int(expected_hooks), (kind, source, value, active, r)
            assert r["sounds"] == int(kind == 7 and source == 2), r
            expected_pc = [
                0xB3E7,
                0xC40D,
                0xC726 if value < 64 else 0xC682 if source == 1 else 0xC6E3,
                0xC71E if source == 1 else 0x8657,
                0xBE7E if source == 1 else 0x86FA,
                (0x9826 if active else 0x8538) if value else 0x982F,
                0x01B281,
                0x889C,
                0x96A1,
            ][kind]
            assert r["pc"] == expected_pc, r
            assert r["PB"] == r["DB"] == (1 if kind == 6 else 0) and r["D"] == 0, r
            assert r["X"] == (0 if kind == 6 else 0x8E0), (kind, r)
            assert r["Y"] == (0x61 if kind == 6 else 0xC961), (kind, r)
            assert r["S"] == (
                0x1FFB
                if kind in (6, 7)
                or (kind == 5 and not active and value)
                or (kind in (3, 4) and source != 1)
                else 0x1FFD
            ), (kind, r)
            if kind == 0:
                assert r["A"] == (0x2402 if source == 0 else 0x2401) and r["P"] == 0, r
            if kind == 1:
                threshold = 210 if source == 1 else 242
                difference = (value - threshold) & 65535
                assert r["A"] == value and r["P"] == (
                    0x40
                    | int(value >= threshold)
                    | (2 if difference == 0 else 0)
                    | (128 if difference & 32768 else 0)
                ), r
            if kind == 2:
                if value >= 64 and source == 1:
                    assert (
                        r["A"] == 0xC6E3
                        and r["delay"] == 60
                        and r["resume"] == 0xC6E3
                        and r["P"] == 0x81
                    ), r
                else:
                    d = (value - 64) & 65535
                    assert r["A"] == value and r["P"] == (
                        int(value >= 64) | (2 if d == 0 else 0) | (128 if d & 32768 else 0)
                    ), r
            if kind in (3, 4):
                assert r["P"] == 0x41 and r["A"] == (
                    0 if source == 1 else 12 if kind == 3 else 30
                ), r
            if kind == 5:
                assert (
                    r["score"] == (0 if source == 1 and active and value else 0x4567)
                    and r["A"] == value
                    and r["P"] == (0x41 if value else 0x43)
                ), r
            if kind == 6:
                jp = source == 1 and active
                # The real preceding 9BEB motion call also advances the JP eligible-call counter.
                assert r["q4"] == (4 if jp else 3 - (value % 16 == 0)) and r["q5"] == (
                    3 if jp else 3 - (value % 4 == 0)
                ), r
            if kind == 7:
                assert (
                    r["score"] == 0x2000
                    and r["lives"] == (3 if source == 2 else 2)
                    and r["A"] == (0x1999 if source == 2 else 0x2000)
                    and r["P"] == 0x41
                ), r
            if kind == 8:
                assert (
                    r["top"] == (31 if source == 1 else 32)
                    and r["A"] == 24
                    and r["P"] == 0x42
                    and r["state"] == 15
                ), r
            rows.append(dict(kind=kind, source=source, input=value, active=active, result=r))
    (out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(
        f"{len(rows)} generated caller cases passed: nine hooks, US/JP/PAL policies, "
        "native fallback, CPU/stack contracts and bounded WRAM writes."
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-release")
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="ar-hle-callers-"))
    out.mkdir(parents=True, exist_ok=True)
    build(ROOT, args.build.resolve(), out)
    check(ROOT, out, args.rom.resolve())
    print(f"Evidence: {out}")


if __name__ == "__main__":
    main()
