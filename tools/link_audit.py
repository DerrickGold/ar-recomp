#!/usr/bin/env python3
"""Compatibility launcher for the Go generated-code link auditor."""

import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
os.chdir(ROOT / "snesrecomp-go")
os.execvp("go", [
    "go", "run", "./cmd/v2regen", "link-audit",
    "--gen-dir", "../src/gen",
    "--src-dir", "../src",
    "--runtime-dir", "runtime/src",
    *sys.argv[1:],
])
