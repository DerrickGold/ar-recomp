#!/usr/bin/env python3
"""Reuse existing Deck inputs privately, then alternate CPU/GPU SIM runs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--prepare", action="store_true")
parser.add_argument("--cohort", default="dynamic")
parser.add_argument("--camera", default="1")
parser.add_argument("--repeats", type=int, default=3)
args = parser.parse_args()
if args.prepare:
    previous = Path("/home/deck/argame/all-effects.TtkGxe/inputs")
    hashes = json.loads((previous / "hashes.json").read_text())
    assert all(hashlib.sha256((previous/p).read_bytes()).hexdigest() == h for p,h in hashes.items())
    shutil.copytree(previous, root / "inputs")
    shutil.copy2(root / "game", root / "inputs/game")
    inputs = root / "inputs"
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in inputs.iterdir() if p.name != "hashes.json"}
    (inputs / "hashes.json").write_text(json.dumps(hashes,indent=2)+"\n")
    print(json.dumps(hashes,indent=2),flush=True)
    sys.exit(0)
base = [sys.executable, str(root / "probe.py"), str(root), "--deck", "--visible", "--telemetry",
        "--cases", "sim-held", "--repeats", "1", "--set", f"AR_SIM3D_CAMERA_MODE={args.camera}"]
for repeat in range(args.repeats):
    for enabled in ((0,1) if repeat % 2 == 0 else (1,0)):
        subprocess.run(base + ["--cohort", f"{args.cohort}-{repeat}-{'gpu' if enabled else 'cpu'}",
            "--set", f"AR_SIM3D_TOWN_GPU_MODELS={enabled}"], check=True)
