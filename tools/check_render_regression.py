#!/usr/bin/env python3
"""Compare game pixels and WRAM across the named rendering regression scenes.

Requires the user's ROM and a real GPU. Controller-input fixtures are included
in the checkout. Each case delegates capture and validation to the same engine used by
the pipeline benchmark. Failed runs retain their logs and images for review.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / "tests/fixtures/benchmark/render-regression.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--case", action="append", help="Named case; default: all")
    parser.add_argument("--output", type=Path, help="New directory for retained evidence")
    args = parser.parse_args()
    cases = json.loads(SUITE.read_text())["cases"]
    unknown = set(args.case or ()) - cases.keys()
    if unknown:
        parser.error(f"Unknown cases: {', '.join(sorted(unknown))}")
    output = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="actraiser-render-regression-"))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    config = output / "config.ini"
    config.write_text("# Fixture settings only; no developer config or environment.\n")
    print(f"Render regression evidence: {output}", flush=True)
    for name in args.case or cases:
        case = cases[name]
        command = [sys.executable, str(ROOT / "tools/compare_pipeline_performance.py"),
                   "--verify-only", "--control", str(args.control.resolve()),
                   "--candidate", str(args.candidate.resolve()), "--rom", str(args.rom.resolve()),
                   "--config", str(config), "--output", str(output / name),
                   "--manifest", str(ROOT / case["manifest"]),
                   "--checkpoint", case["checkpoint"], "--timeout", "600"]
        if "replay" in case:
            command += ["--replay", str(ROOT / case["replay"])]
        for key in ("quit_frames", "capture_from", "capture_to", "capture_every", "require_scene"):
            if key in case:
                command += ["--" + key.replace("_", "-"), str(case[key])]
        print(f"Checking {name}", flush=True)
        subprocess.run(command, check=True)
    print("All selected scenes have identical composite captures and final WRAM.")


if __name__ == "__main__":
    main()
