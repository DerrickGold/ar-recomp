#!/usr/bin/env python3
"""Serial ABC-CBA Palace diagnostic; all other effects stay enabled."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
base = [sys.executable, str(root / "probe_followup.py"), str(root),
        "--deck", "--visible", "--telemetry", "--cases", "palace", "--repeats", "1"]
variants = {
    "full": [],
    "no-shadows": ["--set", "AR_SIM3D_WORLD_NAV_CLOUD_SHADOWS=0"],
    "flat-foreground": ["--set", "AR_SIM3D_SKY_PALACE_VOLUMETRIC=0"],
}
for index, name in enumerate(("full", "no-shadows", "flat-foreground",
                              "flat-foreground", "no-shadows", "full")):
    subprocess.run(base + ["--cohort", f"cloud-component-{index}-{name}"] + variants[name], check=True)
