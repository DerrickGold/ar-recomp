#!/usr/bin/env python3
"""Private same-binary CPU/GPU image and throughput comparisons."""
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
parser.add_argument("--visual", action="store_true")
parser.add_argument("--windowed", action="store_true")
parser.add_argument("--default", action="store_true", help="Run once without the model-path override")
parser.add_argument("--cohort", default="images")
parser.add_argument("--cases", nargs="+", default=["sim-held", "sim-low", "sim-effects"])
args = parser.parse_args()
inputs = args.root / "inputs"
if args.binary:
    shutil.copytree(args.inputs, inputs)
    shutil.copy2(args.binary, inputs / "game")
    (inputs / "hashes.json").unlink()
    (inputs / "hashes.json").write_text(json.dumps({p.name:
        hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs.iterdir()}, indent=2))
base = [sys.executable, str(HERE.parent / "all-effects-2026-09-12/probe.py"),
        str(args.root), "--cases", *args.cases, "--repeats", "1"]
if args.visual:
    base += ["--visual", "--set", "AR_SIM3D_CLOUD_DRIFT=0", "--set", "AR_SIM3D_REACTIVE=0"]
else:
    base += ["--visible", "--set", "AR_SIM3D_CAMERA_MODE=1"]
    if args.windowed:
        base += ["--set", "AR_WINDOW_MODE=Windowed", "--set", "AR_WINDOW_SCALE=2"]
variants = (None,) if args.default else ((0, 1) if args.visual else (0, 1, 1, 0, 0, 1))
for index, enabled in enumerate(variants):
    label = "default" if enabled is None else "gpu" if enabled else "cpu"
    override = [] if enabled is None else ["--set", f"AR_SIM3D_TOWN_GPU_MODELS={enabled}"]
    subprocess.run(base + ["--cohort", f"{args.cohort}-{index}-{label}", *override], check=True)
