#!/usr/bin/env python3
"""Measure unaltered cloud comparison frames; save PNGs and amplified errors."""
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
for a in sorted({p.resolve() for p in (args.root / (args.cohort + "-0-full")).glob("*/runs/*/shot_*.ppm")}):
    matches = {p.resolve() for p in (args.root / (args.cohort + "-0-bounded")).glob("*/runs/*/" + a.name)}
    assert len(matches) == 1
    b = matches.pop()
    av, bv = np.asarray(Image.open(a)).astype(np.int16), np.asarray(Image.open(b)).astype(np.int16)
    error = np.abs(av-bv)
    record = {"frame": a.stem, "size": list(Image.open(a).size),
              "changed_pixels": int(np.count_nonzero(np.max(error, axis=2))),
              "changed_over_8": int(np.count_nonzero(np.max(error, axis=2)>8)),
              "mean_channel_error": float(error.mean()), "max_error": int(error.max())}
    records.append(record)
    Image.open(a).save(out / (a.stem + "-full.png"))
    Image.open(b).save(out / (a.stem + "-bounded.png"))
    Image.fromarray(np.clip(error*8, 0, 255).astype(np.uint8)).save(out / (a.stem + "-difference.png"))
    print(json.dumps(record))
assert len(records) == 3
(out / "results.json").write_text(json.dumps(records, indent=2)+"\n")
