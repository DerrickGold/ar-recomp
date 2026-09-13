#!/usr/bin/env python3
"""Short serial Deck follow-up: low-angle repeats and Palace cloud ABBA."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
base = [sys.executable, str(root / "probe_followup.py"), str(root), "--deck", "--visible", "--telemetry"]
subprocess.run(base + ["--cohort", "low-angle", "--cases", "sim-low", "--repeats", "3"], check=True)
for index, enabled in enumerate((True, False, False, True)):
    subprocess.run(base + ["--cohort", f"cloud-pair-{index}-{'on' if enabled else 'off'}",
                          "--cases", "palace", "--repeats", "1", "--set",
                          f"AR_SIM3D_CLOUD_OPACITY={35 if enabled else 0}"], check=True)
