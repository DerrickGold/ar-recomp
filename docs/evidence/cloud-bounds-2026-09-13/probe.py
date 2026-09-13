#!/usr/bin/env python3
"""Same-binary full/bounded foreground cloud comparisons with private fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
parser.add_argument("--binary", type=Path)
parser.add_argument("--inputs", type=Path)
parser.add_argument("--base-probe", type=Path, default=HERE.parent / "all-effects-2026-09-12/probe.py")
parser.add_argument("--visual", action="store_true")
parser.add_argument("--flat", action="store_true")
parser.add_argument("--deck", action="store_true")
parser.add_argument("--default", action="store_true")
parser.add_argument("--cohort", default="clouds")
parser.add_argument("--repeats", type=int, default=3)
args = parser.parse_args()
if args.binary:
    hashes = json.loads((args.inputs / "hashes.json").read_text())
    assert all(hashlib.sha256((args.inputs / p).read_bytes()).hexdigest() == h for p, h in hashes.items())
    inputs = args.root / "inputs"
    shutil.copytree(args.inputs, inputs)
    shutil.copy2(args.binary, inputs / "game")
    (inputs / "hashes.json").write_text(json.dumps({p.name:
        hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs.iterdir()
        if p.name != "hashes.json"}, indent=2)+"\n")
base = [sys.executable, str(args.base_probe), str(args.root), "--cases", "palace", "--repeats", "1"]
if args.visual:
    base += ["--visual", "--set", "AR_SIM3D_CLOUD_DRIFT=0"]
else:
    base += ["--visible"]
    if not args.deck:
        base += ["--set", "AR_WINDOW_MODE=Windowed", "--set", "AR_WINDOW_SCALE=2"]
if args.flat:
    base += ["--set", "AR_SIM3D_SKY_PALACE_VOLUMETRIC=0"]
if args.deck:
    base += ["--deck", "--telemetry"]
for repeat in range(1 if args.visual or args.default else args.repeats):
    variants = (None,) if args.default else ((0, 1) if repeat % 2 == 0 else (1, 0))
    for bounded in variants:
        name = "default" if bounded is None else "bounded" if bounded else "full"
        override = [] if bounded is None else ["--set", f"AR_SIM3D_SKY_CLOUD_BOUNDS={bounded}"]
        subprocess.run(base + ["--cohort", f"{args.cohort}-{repeat}-{name}", *override], check=True)
