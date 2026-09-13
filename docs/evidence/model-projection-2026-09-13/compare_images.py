#!/usr/bin/env python3
"""Quantify and retain unaltered CPU/GPU screenshot pairs plus diagnostic differences."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
parser.add_argument("--cohort", default="images")
args = parser.parse_args()
out = args.root / (args.cohort + "-comparison")
out.mkdir(exist_ok=True)
records = []
for cpu in sorted({p.resolve() for p in (args.root / (args.cohort + "-0-cpu")).glob("*/runs/*/shot_*.ppm")}):
    case = cpu.parents[2].name
    matches = sorted({p.resolve() for p in (args.root / (args.cohort + "-1-gpu") / case).glob("runs/*/" + cpu.name)})
    if not matches:
        continue
    a, b = Image.open(cpu).convert("RGB"), Image.open(matches[0]).convert("RGB")
    av, bv = np.asarray(a).astype(np.int16), np.asarray(b).astype(np.int16)
    error = np.abs(av-bv)
    record = {"case": case, "frame": cpu.stem, "size": a.size,
              "changed_pixels": int(np.count_nonzero(np.max(error,axis=2))),
              "changed_over_8": int(np.count_nonzero(np.max(error,axis=2)>8)),
              "mean_channel_error": float(error.mean()), "max_error": int(error.max())}
    name = case + "-" + cpu.stem
    a.save(out / (name + "-cpu.png")); b.save(out / (name + "-gpu.png"))
    Image.fromarray(np.clip(error*4,0,255).astype(np.uint8)).save(out / (name + "-difference.png"))
    records.append(record)
    print(json.dumps(record))
(out / "results.json").write_text(json.dumps(records,indent=2)+"\n")
