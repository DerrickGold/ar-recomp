#!/usr/bin/env python3
"""Compatibility launcher for the Go hard-stub census."""

import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
os.chdir(ROOT / "snesrecomp-go")
os.execvp("go", [
    "go", "run", "./cmd/v2regen", "stub-census",
    "--gen-dir", "../src/gen",
    *sys.argv[1:],
])
