#!/usr/bin/env python3
"""Copy hash-verified, already-present Deck fixtures into a private run folder."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path(__file__).resolve().parent
inputs = root / "inputs"
sources = {
    "ar.sfc": ("/home/deck/argame/ar.sfc", "b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0"),
    "D7-voxel-town.srm": ("/home/deck/argame/sim-connected-release.4jFBq6/seed.srm", "26ec2474882a69dff576f518f614a094f58f1428c807faf83230d72fe4c13568"),
    "D7-voxel-town.rec": ("/home/deck/argame/sim-connected-release.4jFBq6/replay.rec", "bfe084061b485c8710d5c2c727d9d0f9983707d3fcf949ca27b3ca422c004eb9"),
    "D7-voxel-town.ini": ("/home/deck/argame/sim-connected-release.4jFBq6/settings.ini", "9f4a56d68a7ac0aa0e96479c024240bc85e9ea370e94c8113b41a3ccfd8a3806"),
    "palace.srm": ("/home/deck/argame/comparison-20260912.Ra0eem/globe-seed.srm", "480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45"),
    "palace.rec": ("/home/deck/argame/comparison-20260912.Ra0eem/palace.rec", "ed649fcc57a586b34d3bd14af75f69e41a7fa335d87c8b9c773a44f714b417dc"),
    "palace.ini": ("/home/deck/argame/comparison-20260912.Ra0eem/world-navigation-settings.ini", "c0acc64b0c021a7fea38081f7646f8ed742dbed9740358a1fb9bd51472d8317b"),
    "navigation.rec": ("/home/deck/argame/cloud-body-probe-20260912.PfiPXt/navigation.rec", "bdf54e78a5061a7c640aef598f4530c7b2413fdd829fc20b1e3157e9443a5c4f"),
}
sources["navigation.srm"] = sources["palace.srm"]
sources["navigation.ini"] = sources["palace.ini"]
for name, (path, expected) in sources.items():
    assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, name
assert hashlib.sha256((root / "game").read_bytes()).hexdigest() == "012e457c98a4b1063f128dc065414ad27062b7009119e7122a86c01ddd00762a"
inputs.mkdir(exist_ok=False)
for name, (path, _) in sources.items():
    shutil.copy2(path, inputs / name)
for name in ("game", "profile.json", "probe.py"):
    shutil.copy2(root / name, inputs / name)
(inputs / "config.ini").write_text("# Isolated compiled defaults; explicit all-effects profile.\n")
hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs.iterdir() if p.is_file()}
(inputs / "hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")
print(json.dumps(hashes, indent=2))
