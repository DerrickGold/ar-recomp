#!/usr/bin/env python3
"""Measure unaltered shadow A/B frames and emit PNG/error diagnostics."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
parser.add_argument("--first", default="images-0-single")
parser.add_argument("--second", default="images-0-batch")
parser.add_argument("--output", default="images-comparison")
args = parser.parse_args()
out = args.root / args.output
out.mkdir(exist_ok=True)
records = []
for a in sorted({p.resolve() for p in (args.root / args.first).glob("*/runs/*/shot_*.ppm")}):
    case = a.parents[2].name
    matches = {p.resolve() for p in (args.root / args.second / case).glob("runs/*/" + a.name)}
    assert len(matches) == 1, (case, a.name)
    b = matches.pop()
    av, bv = np.asarray(Image.open(a)).astype(np.int16), np.asarray(Image.open(b)).astype(np.int16)
    error = np.abs(av-bv)
    record = {"case":case, "frame":a.stem, "size":list(Image.open(a).size),
        "changed_pixels":int(np.count_nonzero(np.max(error,axis=2))),
        "changed_over_8":int(np.count_nonzero(np.max(error,axis=2)>8)),
        "mean_channel_error":float(error.mean()), "max_error":int(error.max())}
    records.append(record)
    name = case + "-" + a.stem
    Image.open(a).save(out/(name+"-single.png")); Image.open(b).save(out/(name+"-batch.png"))
    Image.fromarray(np.clip(error*8,0,255).astype(np.uint8)).save(out/(name+"-difference.png"))
    print(json.dumps(record))
assert records and len(records)%3 == 0
(out/"results.json").write_text(json.dumps(records,indent=2)+"\n")
