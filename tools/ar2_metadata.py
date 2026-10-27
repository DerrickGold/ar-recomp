"""Shared USA ROM identity and the host/module's checked scene profiles."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ROM_SHA256 = "71bdd448a30b88725864e55594ebb67a118b1f197a3f9e5dd39dbf23399efa15"
ROM_BYTES = 0x180000


def scene_profiles():
    text = (ROOT / "src/actraiser2/ar2_scenes.inc").read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    profiles = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"AR2_SCENE\((0x[0-9a-f]+), (0x[0-9a-f]+), (Production|Research)\)", line)
        if not match:
            raise ValueError(f"invalid AR2 scene profile: {line}")
        scene, entrance = (int(value, 16) for value in match.group(1, 2))
        if scene in profiles:
            raise ValueError(f"duplicate AR2 scene {scene:02x}")
        profiles[scene] = (entrance, match[3] == "Production")
    if any(entrance not in profiles or profiles[entrance][0] != entrance
           for entrance, _ in profiles.values()):
        raise ValueError("AR2 connected room has no entrance profile")
    return profiles
