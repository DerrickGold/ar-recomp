#!/usr/bin/env python3
"""Compatibility launcher for the Go opcode differential harness.

New automation should call `v2regen opcode-diff` directly. This filename is
kept so existing debugging notes and local scripts continue to work; it no
longer imports the legacy Python recompiler.
"""

import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
GO_ROOT = ROOT / "snesrecomp-go"

os.chdir(GO_ROOT)
os.execvp("go", [
    "go", "run", "./cmd/v2regen", "opcode-diff",
    "--cache-dir", "../tools/oracle/harte_cache",
    "--runtime-dir", "runtime/src",
    "--work-dir", "../build/opcode_diff",
    *sys.argv[1:],
])
