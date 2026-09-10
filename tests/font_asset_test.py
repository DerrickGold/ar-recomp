#!/usr/bin/env python3
"""Verify the bundled primary font without depending on fontTools."""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"
LICENSE = ROOT / "game-assets/fonts/noto/OFL.txt"
EXPECTED_SHA256 = "c3c65645ed2c76892b0bf72c714783885117f2aa4695d89e87ea0b2f1c8798fe"
FALLBACK_FONTS = (
    (
        ROOT / "tests/fixtures/unicode-fonts/fonts/NotoSansJP-Bold.otf",
        "1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f",
        "日本語カタカナひらがな中文",
    ),
    (
        ROOT / "tests/fixtures/unicode-fonts/fonts/NotoSansArabic[wdth,wght].ttf",
        "63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e",
        "العربية",
    ),
)


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def table(data: bytes, tag: bytes) -> bytes:
    count = u16(data, 4)
    for index in range(count):
        record = 12 + index * 16
        if data[record : record + 4] == tag:
            offset = u32(data, record + 8)
            length = u32(data, record + 12)
            assert offset + length <= len(data), f"truncated {tag!r} table"
            return data[offset : offset + length]
    raise AssertionError(f"missing {tag!r} table")


def format12_contains(cmap: bytes, offset: int, codepoint: int) -> bool:
    length = u32(cmap, offset + 4)
    groups = u32(cmap, offset + 12)
    assert offset + length <= len(cmap), "truncated cmap format 12"
    for index in range(groups):
        group = offset + 16 + index * 12
        start = u32(cmap, group)
        end = u32(cmap, group + 4)
        if codepoint < start:
            return False
        if codepoint <= end:
            return True
    return False


def format4_contains(cmap: bytes, offset: int, codepoint: int) -> bool:
    if codepoint > 0xFFFF:
        return False
    length = u16(cmap, offset + 2)
    end = offset + length
    assert end <= len(cmap), "truncated cmap format 4"
    segments = u16(cmap, offset + 6) // 2
    end_codes = offset + 14
    start_codes = end_codes + segments * 2 + 2
    deltas = start_codes + segments * 2
    range_offsets = deltas + segments * 2
    for index in range(segments):
        segment_end = u16(cmap, end_codes + index * 2)
        if codepoint > segment_end:
            continue
        start = u16(cmap, start_codes + index * 2)
        if codepoint < start:
            return False
        delta = u16(cmap, deltas + index * 2)
        range_word = range_offsets + index * 2
        glyph_offset = u16(cmap, range_word)
        if glyph_offset == 0:
            return ((codepoint + delta) & 0xFFFF) != 0
        glyph_word = range_word + glyph_offset + (codepoint - start) * 2
        assert glyph_word + 2 <= end, "bad cmap format 4 glyph offset"
        glyph = u16(cmap, glyph_word)
        return glyph != 0 and ((glyph + delta) & 0xFFFF) != 0
    return False


def cmap_contains(cmap: bytes, codepoint: int) -> bool:
    subtable_count = u16(cmap, 2)
    candidates: list[tuple[int, int]] = []
    for index in range(subtable_count):
        record = 4 + index * 8
        offset = u32(cmap, record + 4)
        assert offset + 2 <= len(cmap), "bad cmap subtable offset"
        format_number = u16(cmap, offset)
        if format_number in (4, 12):
            candidates.append((format_number, offset))
    for format_number, offset in sorted(candidates, reverse=True):
        if format_number == 12 and format12_contains(cmap, offset, codepoint):
            return True
        if format_number == 4 and format4_contains(cmap, offset, codepoint):
            return True
    return False


def main() -> None:
    data = FONT.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    assert digest == EXPECTED_SHA256, f"font SHA-256 changed: {digest}"

    license_text = LICENSE.read_text(encoding="utf-8")
    assert "SIL OPEN FONT LICENSE Version 1.1" in license_text

    cmap = table(data, b"cmap")
    required = (
        "ÀÂÇÉÈÊËÎÏÔÙÛÜŸŒÆàâçéèêëîïôùûüÿœæ"
        "ßẞ ĀȘŽ Ελληνικά Кириллица –—‘’…"
        "\u0300\u0301\u0302\u0308"
    )
    missing = sorted({character for character in required if not cmap_contains(cmap, ord(character))})
    assert not missing, "font lacks required characters: " + " ".join(
        f"U+{ord(character):04X}" for character in missing
    )

    for fallback, expected_hash, sample in FALLBACK_FONTS:
        fallback_data = fallback.read_bytes()
        assert hashlib.sha256(fallback_data).hexdigest() == expected_hash
        fallback_cmap = table(fallback_data, b"cmap")
        fallback_missing = sorted(
            {character for character in sample if not cmap_contains(fallback_cmap, ord(character))}
        )
        assert not fallback_missing, f"{fallback.name} lacks fixture glyphs: {fallback_missing!r}"

    print(
        f"font assets OK: base={len(data)} bytes, "
        f"fallbacks={len(FALLBACK_FONTS)}, SHA-256 {digest}"
    )


if __name__ == "__main__":
    main()
