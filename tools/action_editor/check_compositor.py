#!/usr/bin/env python3
"""Compare production Diorama commands/projections natively and in WASM (not pixels)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from build_preview import ROOT, compiler_environment


def check(sanitize=False):
    compiler = os.environ.get("EMCC", "emcc")
    env = compiler_environment(compiler)
    sources = (ROOT / "tools/action_editor/compositor_sources.txt").read_text().splitlines()
    common = ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
              "-I", str(ROOT / "src"), "-I", str(ROOT / "tests"),
              "-I", str(ROOT / "snesrecomp-go/runtime/include"),
              "tests/diorama_compositor_test.c", *sources]
    with tempfile.TemporaryDirectory(prefix="diorama-compositor-") as directory:
        work = Path(directory)
        native, wasm, trace = work / "native", work / "compositor.wasm", work / "native.trace"
        native_flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []
        subprocess.run([os.environ.get("CC", "cc"), *common, *native_flags,
                        "-lm", "-o", str(native)], cwd=ROOT, check=True)
        subprocess.run([str(native), str(trace)], check=True)
        exports = ["Count", "Run", "Trace"]
        subprocess.run([compiler, *common, "-DAR_COMPOSITOR_WASM", "--no-entry",
                        "-sSTANDALONE_WASM=1", "-sFILESYSTEM=0", "-sSTACK_SIZE=1048576",
                        "-sINITIAL_MEMORY=33554432", "-sALLOW_MEMORY_GROWTH=0",
                        *["-Wl,--export=DioramaFixture_" + name for name in exports],
                        "-o", str(wasm)], cwd=ROOT, env=env, check=True)
        subprocess.run(["node", str(ROOT / "tests/diorama_compositor_wasm.test.mjs"),
                        str(wasm), str(trace)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="Run native ASan/UBSan too")
    check(parser.parse_args().sanitize)
