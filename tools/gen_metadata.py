#!/usr/bin/env python3
"""Compatibility launcher for the Go generated-code metadata scraper."""

import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
os.chdir(ROOT / "snesrecomp-go")
os.execvp("go", [
    "go", "run", "./cmd/v2regen", "metadata",
    "--gen-dir", "../src/gen",
    "--cfg-dir", "../recomp",
    "--out", "../saves/gen_meta.json",
    *sys.argv[1:],
])
