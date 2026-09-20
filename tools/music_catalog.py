"""Shared song identity data used by the ROM census and manifest generator."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "installer/internal/builder/assets/music.json"


def load_music_catalog():
    return json.loads(CATALOG.read_text())
