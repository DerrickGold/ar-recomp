#!/usr/bin/env python3
"""Compatibility launcher for `v2regen rts-webs`.

The census implementation is Go-only. This historical entry point remains so
older debugging commands do not break; new scripts should invoke the Go binary
directly.
"""

import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
arguments = list(sys.argv[1:])
forwarded = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == "--bank" and index + 1 < len(arguments):
        forwarded.extend((value, arguments[index + 1]))
        index += 2
    elif value == "--suggest":
        forwarded.append(value)
        index += 1
    elif value.startswith("-"):
        forwarded.append(value)
        index += 1
    else:
        forwarded.extend(("--rom", str(pathlib.Path(value).resolve())))
        index += 1

os.chdir(ROOT / "snesrecomp-go")
os.execvp("go", [
    "go", "run", "./cmd/v2regen", "rts-webs",
    "--rom", str(ROOT / "ar.sfc"),
    "--cfg-dir", str(ROOT / "recomp"),
    *forwarded,
])
