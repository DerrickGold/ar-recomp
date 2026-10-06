#!/usr/bin/env python3
"""Optional local-ROM angel regression using the production play build.

Requires a Unix Makefiles play build; relinks its objects with an audit main.
No GPU, controller or captured state
is needed; animation, arrows and generated return dispatch execute normally.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release")
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--output", type=Path, default=ROOT / "build/angel-generated")
    args = parser.parse_args()
    build, out = args.build_dir.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    flags = (build / "CMakeFiles/ActRaiserRecomp.dir/flags.make").read_text()
    opts = []
    for key in ("C_DEFINES", "C_INCLUDES", "C_FLAGS"):
        opts += shlex.split(re.search(r"^" + key + r" = (.*)$", flags, re.M)[1])
    compiler = re.search(r"^CMAKE_C_COMPILER:FILEPATH=(.*)$",
                         (build / "CMakeCache.txt").read_text(), re.M)[1]
    obj = out / "angel_movement_callers.o"
    subprocess.run([compiler, *opts, "-I" + str(ROOT / "tests"),
                    "-c", str(ROOT / "tests/fixtures/angel_movement_callers.c"),
                    "-o", str(obj)], check=True)
    link = shlex.split((build / "CMakeFiles/ActRaiserRecomp.dir/link.txt").read_text())
    link[link.index("CMakeFiles/ActRaiserRecomp.dir/src/main.c.o")] = str(obj)
    executable = out / "angel-movement-callers"
    link[link.index("-o") + 1] = str(executable)
    subprocess.run(link, cwd=build, check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("AR_", "SNESRECOMP_"))}
    subprocess.run([str(executable), str(args.rom.resolve())], env=env, check=True, timeout=30)


if __name__ == "__main__":
    main()
