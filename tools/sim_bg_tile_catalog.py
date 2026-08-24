#!/usr/bin/env python3
"""Render an exhaustive SIM-town background-tile classification catalog.

The catalog joins two authoritative sources:

* a GF-stable SIM snapshot supplies the current town's BG1 CHR, palette,
  64x64-tile map, terrain/structure metatile definitions, and structure records;
* the stock ROM supplies all 94 structure draw lists at $03:D928-$03:DC73.

The result is intended for the object-extrusion research pass: it keeps every
tile/metatile/list address visible so houses, cathedrals, factories, and trees
can be classified without losing the multi-cell relationships authored by the
original game.

Example:
  python3 tools/sim_bg_tile_catalog.py --rom ar.sfc \
      --captures-dir /tmp/ar-sim-town-captures \
      --out runs/sim-background-tile-dump

Pillow is used only for labeled PNG output. The Codex workspace runtime ships
it; ordinary environments may install Pillow or use the generated JSON/CSVs
after adding an alternate image backend.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from ar_lib import lorom_offset, read_le16


TOWNS = (
    ("fillmore", "Fillmore", 1),
    ("bloodpool", "Bloodpool", 2),
    ("kasandora", "Kasandora", 3),
    ("aitos", "Aitos", 4),
    ("marahna", "Marahna", 5),
    ("northwall", "Northwall", 6),
)
TOWN_BY_SLUG = {slug: (title, map_number)
                for slug, title, map_number in TOWNS}

WRAM_SIZE = 0x20000
VRAM_SIZE = 0x10000
CGRAM_SIZE = 0x0200
TERRAIN_DEFINITIONS = 0x2100
STRUCTURE_DEFINITIONS = 0x3100
METATILE_COUNT = 0x100
METATILE_BYTES = 8
TOWN_TILEMAP = 0x10000                 # flat mirror of $7F:0000
TOWN_CELL_MAPS = 0x12000               # flat mirror of $7F:2000
TOWN_CELL_MAP_BYTES = 0x400
STRUCTURE_RECORDS = 0x16BE7            # flat mirror of $7F:6BE7
STRUCTURE_RECORD_BYTES = 4
STRUCTURE_RECORD_COUNT = 128
VISUAL_WORD_MASK = 0xFDFF              # $03:9B5A/$9C43 clear bit 9

DRAW_LIST_BANK = 0x03
DRAW_LIST_START = 0xD928
DRAW_LIST_END = 0xDC74                 # exclusive
# Visual shortlist only: the exhaustive sheet remains authoritative. These are
# the obvious house/support/special/factory compositions; the review CSV keeps
# them unclassified until a human assigns names and footprint/elevation roles.
LIKELY_BUILDING_DRAW_LISTS = frozenset(
    (*range(0, 32), *range(74, 80), 88, 89))

STRUCTURE_CLASSES = {
    0: "house",
    1: "bridge",
    2: "field",
    3: "factory3",
    4: "factory4",
    5: "special5",
    6: "special6",
}
RECORD_COLORS = (
    "#ffde59", "#4cc9f0", "#80ed99", "#f72585",
    "#b5179e", "#ff9f1c", "#9b5de5", "#adb5bd",
)
QUADRANT_NAMES = ("TL", "TR", "BL", "BR")


def strip_snapshot_suffix(path: Path) -> Path:
    text = str(path)
    for suffix in (".wram.bin", ".vram.bin", ".cgram.bin", ".ppu.json"):
        if text.endswith(suffix):
            return Path(text[:-len(suffix)])
    return path


def find_snapshot_prefix(directory: Path) -> Path:
    candidates = sorted(directory.glob("*.wram.bin"))
    if len(candidates) != 1:
        raise ValueError(
            f"{directory}: expected one *.wram.bin, found {len(candidates)}")
    return strip_snapshot_suffix(candidates[0])


@dataclass(frozen=True)
class DrawCommand:
    dx: int
    dy: int
    metatile: int


@dataclass(frozen=True)
class DrawList:
    index: int
    address: int
    commands: tuple[DrawCommand, ...]

    @property
    def width_cells(self) -> int:
        return max(command.dx for command in self.commands) + 1

    @property
    def height_cells(self) -> int:
        return max(command.dy for command in self.commands) + 1

    @property
    def metatiles(self) -> tuple[int, ...]:
        return tuple(command.metatile for command in self.commands)


@dataclass(frozen=True)
class StructureRecord:
    slot: int
    cell_x: int
    cell_y: int
    flags: int
    action: int

    @property
    def class_id(self) -> int:
        return self.flags & 0x0F

    @property
    def subtype(self) -> int:
        return self.flags >> 4 & 3


class Snapshot:
    def __init__(self, slug: str, prefix: Path):
        if slug not in TOWN_BY_SLUG:
            raise ValueError(f"unknown town slug {slug!r}")
        self.slug = slug
        self.title, self.expected_map = TOWN_BY_SLUG[slug]
        self.prefix = strip_snapshot_suffix(prefix)
        self.wram = Path(str(self.prefix) + ".wram.bin").read_bytes()
        self.vram = Path(str(self.prefix) + ".vram.bin").read_bytes()
        self.cgram = Path(str(self.prefix) + ".cgram.bin").read_bytes()
        if len(self.wram) != WRAM_SIZE:
            raise ValueError(f"{self.prefix}: WRAM is {len(self.wram)} bytes")
        if len(self.vram) != VRAM_SIZE:
            raise ValueError(f"{self.prefix}: VRAM is {len(self.vram)} bytes")
        if len(self.cgram) != CGRAM_SIZE:
            raise ValueError(f"{self.prefix}: CGRAM is {len(self.cgram)} bytes")
        if self.wram[0x18] != 0 or self.wram[0x19] != self.expected_map:
            raise ValueError(
                f"{self.prefix}: expected SIM map 00/{self.expected_map:02X}, "
                f"got {self.wram[0x18]:02X}/{self.wram[0x19]:02X}")
        if self.wram[0x17BF9] != self.expected_map - 1:
            raise ValueError(
                f"{self.prefix}: current-town byte is "
                f"{self.wram[0x17BF9]:02X}, expected {self.expected_map - 1:02X}")
        self.palette = tuple(self._decode_color(index) for index in range(256))

    def _decode_color(self, index: int) -> tuple[int, int, int]:
        color = read_le16(self.cgram, index * 2) & 0x7FFF
        return (
            (color & 0x1F) * 255 // 31,
            (color >> 5 & 0x1F) * 255 // 31,
            (color >> 10 & 0x1F) * 255 // 31,
        )

    def definition_words(self, atlas: str, metatile: int) -> tuple[int, ...]:
        base = (STRUCTURE_DEFINITIONS if atlas == "structure"
                else TERRAIN_DEFINITIONS)
        offset = base + metatile * METATILE_BYTES
        return tuple(read_le16(self.wram, offset + index * 2) & VISUAL_WORD_MASK
                     for index in range(4))

    def town_tilemap_word(self, tile_x: int, tile_y: int) -> int:
        quadrant = (2 if tile_y >= 32 else 0) + (1 if tile_x >= 32 else 0)
        word = (quadrant * 1024 + (tile_y & 31) * 32 + (tile_x & 31))
        return read_le16(self.wram, TOWN_TILEMAP + word * 2)

    def live_cell_words(self, cell_x: int, cell_y: int) -> tuple[int, ...]:
        tile_x, tile_y = cell_x * 2, cell_y * 2
        return (
            self.town_tilemap_word(tile_x, tile_y),
            self.town_tilemap_word(tile_x + 1, tile_y),
            self.town_tilemap_word(tile_x, tile_y + 1),
            self.town_tilemap_word(tile_x + 1, tile_y + 1),
        )

    def live_cell_key(self, cell_x: int, cell_y: int) -> str:
        raw = b"".join(word.to_bytes(2, "little")
                       for word in self.live_cell_words(cell_x, cell_y))
        return hashlib.sha256(raw).hexdigest()[:8]

    def cell_map_value(self, cell_x: int, cell_y: int) -> int:
        town = self.expected_map - 1
        quadrant = (2 if cell_y >= 16 else 0) + (1 if cell_x >= 16 else 0)
        index = quadrant * 256 + (cell_y & 15) * 16 + (cell_x & 15)
        return self.wram[TOWN_CELL_MAPS + town * TOWN_CELL_MAP_BYTES + index]

    def used_cell_metatiles(self) -> set[int]:
        return {self.cell_map_value(x, y)
                for y in range(32) for x in range(32)}

    def used_terrain_metatiles(self) -> set[int]:
        # $E0-$EF are structure/special expansion marks in the cell map, not
        # IDs consumed directly through the $7E:2100 terrain atlas.
        return {value for value in self.used_cell_metatiles()
                if not 0xE0 <= value <= 0xEF}

    def structure_records(self) -> list[StructureRecord]:
        base = STRUCTURE_RECORDS + (self.expected_map - 1) * 0x200
        records = []
        for slot in range(STRUCTURE_RECORD_COUNT):
            at = base + slot * STRUCTURE_RECORD_BYTES
            x, y, flags, action = self.wram[at:at + 4]
            if flags & 0x80:
                records.append(StructureRecord(slot, x, y, flags, action))
        return records

    def render_entry(self, entry: int, transparent_zero: bool = True):
        Image, _, _ = pillow()
        tile = entry & 0x3FF
        palette_base = (entry >> 10 & 7) * 16
        hflip = bool(entry & 0x4000)
        vflip = bool(entry & 0x8000)
        data_offset = tile * 32
        if data_offset + 32 > len(self.vram):
            raise ValueError(f"tile ${tile:03X} reads past VRAM")
        image = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
        pixels = image.load()
        for out_y in range(8):
            source_y = 7 - out_y if vflip else out_y
            p0 = self.vram[data_offset + source_y * 2]
            p1 = self.vram[data_offset + source_y * 2 + 1]
            p2 = self.vram[data_offset + 16 + source_y * 2]
            p3 = self.vram[data_offset + 16 + source_y * 2 + 1]
            for out_x in range(8):
                source_x = 7 - out_x if hflip else out_x
                bit = 7 - source_x
                color_index = ((p0 >> bit & 1) |
                               (p1 >> bit & 1) << 1 |
                               (p2 >> bit & 1) << 2 |
                               (p3 >> bit & 1) << 3)
                if color_index == 0 and transparent_zero:
                    continue
                red, green, blue = self.palette[palette_base + color_index]
                pixels[out_x, out_y] = (red, green, blue, 255)
        return image

    def render_metatile(self, atlas: str, metatile: int,
                        transparent_zero: bool = True):
        Image, _, _ = pillow()
        image = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        for index, entry in enumerate(self.definition_words(atlas, metatile)):
            tile = self.render_entry(entry, transparent_zero)
            image.alpha_composite(tile, ((index & 1) * 8, (index >> 1) * 8))
        return image

    def render_live_cell(self, cell_x: int, cell_y: int,
                         transparent_zero: bool = True):
        Image, _, _ = pillow()
        image = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        for index, entry in enumerate(self.live_cell_words(cell_x, cell_y)):
            tile = self.render_entry(entry, transparent_zero)
            image.alpha_composite(tile, ((index & 1) * 8, (index >> 1) * 8))
        return image

    def render_town_map(self):
        Image, _, _ = pillow()
        backdrop = self.palette[0] + (255,)
        image = Image.new("RGBA", (512, 512), backdrop)
        for tile_y in range(64):
            for tile_x in range(64):
                entry = self.town_tilemap_word(tile_x, tile_y)
                tile = self.render_entry(entry, transparent_zero=True)
                image.alpha_composite(tile, (tile_x * 8, tile_y * 8))
        return image


_PILLOW = None


def pillow():
    global _PILLOW
    if _PILLOW is None:
        try:
            from PIL import Image, ImageDraw, ImageFont
        except ImportError as error:
            raise RuntimeError(
                "labeled PNG output requires Pillow (import PIL failed)") from error
        _PILLOW = (Image, ImageDraw, ImageFont)
    return _PILLOW


def parse_draw_lists(rom: bytes) -> list[DrawList]:
    offset = lorom_offset(DRAW_LIST_BANK, DRAW_LIST_START)
    end = lorom_offset(DRAW_LIST_BANK, DRAW_LIST_END)
    address = DRAW_LIST_START
    lists = []
    while offset < end:
        count = rom[offset]
        size = 1 + count * 3
        if not count or offset + size > end:
            raise ValueError(
                f"invalid draw list at $03:{address:04X}: count={count}")
        commands = tuple(
            DrawCommand(*rom[offset + 1 + index * 3:
                              offset + 4 + index * 3])
            for index in range(count))
        lists.append(DrawList(len(lists), address, commands))
        offset += size
        address += size
    if offset != end or address != DRAW_LIST_END:
        raise ValueError("draw-list crawl did not end exactly at $03:DC74")
    return lists


def checker(size: tuple[int, int], light=(43, 47, 59, 255),
            dark=(30, 33, 43, 255), square: int = 8):
    Image, ImageDraw, _ = pillow()
    image = Image.new("RGBA", size, dark)
    draw = ImageDraw.Draw(image)
    for y in range(0, size[1], square):
        for x in range(0, size[0], square):
            if (x // square + y // square) & 1:
                draw.rectangle((x, y, min(x + square - 1, size[0] - 1),
                                min(y + square - 1, size[1] - 1)), fill=light)
    return image


def scaled(image, factor: int):
    Image, _, _ = pillow()
    return image.resize((image.width * factor, image.height * factor),
                        Image.Resampling.NEAREST)


def make_header(draw, font, title: str, subtitle: str, width: int) -> int:
    draw.rectangle((0, 0, width, 52), fill="#171923")
    draw.text((12, 8), title, fill="#f8f9fa", font=font)
    draw.text((12, 28), subtitle, fill="#a9b1c3", font=font)
    return 52


def render_metatile_sheet(snapshot: Snapshot, atlas: str, ids: Iterable[int],
                          output: Path, subtitle: str) -> None:
    Image, ImageDraw, ImageFont = pillow()
    font = ImageFont.load_default()
    values = sorted(set(ids))
    columns = 16
    cell_w, cell_h = 56, 58
    rows = math.ceil(len(values) / columns)
    width, height = columns * cell_w, 52 + rows * cell_h
    sheet = Image.new("RGBA", (width, height), "#20232e")
    draw = ImageDraw.Draw(sheet)
    y0 = make_header(draw, font,
                     f"{snapshot.title} - {atlas} metatile candidates",
                     subtitle, width)
    for position, metatile in enumerate(values):
        col, row = position % columns, position // columns
        x, y = col * cell_w, y0 + row * cell_h
        draw.rectangle((x, y, x + cell_w - 1, y + cell_h - 1),
                       outline="#444a5a")
        draw.text((x + 4, y + 3), f"MT ${metatile:02X}",
                  fill="#e9ecef", font=font)
        preview = checker((32, 32), square=4)
        art = scaled(snapshot.render_metatile(atlas, metatile), 2)
        preview.alpha_composite(art)
        sheet.alpha_composite(preview, (x + 12, y + 20))
    sheet.convert("RGB").save(output, optimize=True)


def draw_list_preview(snapshot: Snapshot, item: DrawList):
    Image, _, _ = pillow()
    image = Image.new("RGBA", (item.width_cells * 16,
                               item.height_cells * 16), (0, 0, 0, 0))
    for command in item.commands:
        art = snapshot.render_metatile("structure", command.metatile)
        image.alpha_composite(art, (command.dx * 16, command.dy * 16))
    return image


def command_rows(item: DrawList) -> list[str]:
    rows = []
    by_position = {(command.dx, command.dy): command.metatile
                   for command in item.commands}
    for y in range(item.height_cells):
        rows.append(" ".join(
            f"{by_position[(x, y)]:02X}" if (x, y) in by_position else "--"
            for x in range(item.width_cells)))
    return rows


def render_draw_list_sheet(snapshot: Snapshot, draw_lists: list[DrawList],
                           output: Path, focused: bool = False) -> None:
    Image, ImageDraw, ImageFont = pillow()
    font = ImageFont.load_default()
    columns = 10
    cell_w, cell_h = 112, 112
    rows = math.ceil(len(draw_lists) / columns)
    width, height = columns * cell_w, 52 + rows * cell_h
    sheet = Image.new("RGBA", (width, height), "#20232e")
    draw = ImageDraw.Draw(sheet)
    title = (f"{snapshot.title} - building-focused draw-list shortlist"
             if focused else
             f"{snapshot.title} - all ROM structure draw lists")
    subtitle = (
        "Visual shortlist for review; stable ROM addresses and authentic cell layout"
        if focused else
        "94 lists; labels are stable bank-$03 addresses; previews preserve cell layout")
    y0 = make_header(draw, font, title, subtitle, width)
    for position, item in enumerate(draw_lists):
        col, row = position % columns, position // columns
        x, y = col * cell_w, y0 + row * cell_h
        border = "#ffd166" if len(item.commands) > 1 else "#444a5a"
        draw.rectangle((x, y, x + cell_w - 1, y + cell_h - 1), outline=border)
        draw.text((x + 4, y + 3),
                  f"#{item.index:02d}  $03:{item.address:04X}",
                  fill="#f8f9fa", font=font)
        preview_size = (item.width_cells * 32, item.height_cells * 32)
        preview = checker(preview_size, square=4)
        preview.alpha_composite(scaled(draw_list_preview(snapshot, item), 2))
        sheet.alpha_composite(
            preview, (x + (cell_w - preview.width) // 2, y + 20))
        for line_index, text in enumerate(command_rows(item)):
            draw.text((x + 4, y + 89 + line_index * 10), text,
                      fill="#adb5bd", font=font)
    sheet.convert("RGB").save(output, optimize=True)


def render_bg_tile_sheet(snapshot: Snapshot, sources: dict[int, set[str]],
                         output: Path, title: str, subtitle: str) -> None:
    Image, ImageDraw, ImageFont = pillow()
    font = ImageFont.load_default()
    entries = sorted(sources)
    columns = 16
    cell_w, cell_h = 64, 58
    rows = math.ceil(len(entries) / columns)
    width, height = columns * cell_w, 52 + rows * cell_h
    sheet = Image.new("RGBA", (width, height), "#20232e")
    draw = ImageDraw.Draw(sheet)
    y0 = make_header(draw, font, f"{snapshot.title} - {title}", subtitle, width)
    for position, entry in enumerate(entries):
        col, row = position % columns, position // columns
        x, y = col * cell_w, y0 + row * cell_h
        draw.rectangle((x, y, x + cell_w - 1, y + cell_h - 1),
                       outline="#444a5a")
        tile = entry & 0x3FF
        palette = entry >> 10 & 7
        flips = ("H" if entry & 0x4000 else "-") + \
                ("V" if entry & 0x8000 else "-")
        draw.text((x + 3, y + 3), f"T${tile:03X} P{palette} {flips}",
                  fill="#e9ecef", font=font)
        preview = checker((32, 32), square=4)
        preview.alpha_composite(scaled(snapshot.render_entry(entry), 4))
        sheet.alpha_composite(preview, (x + 16, y + 20))
    sheet.convert("RGB").save(output, optimize=True)


def metatile_entry_sources(snapshot: Snapshot, atlas: str,
                           ids: Iterable[int]) -> dict[int, set[str]]:
    sources: dict[int, set[str]] = {}
    for metatile in sorted(set(ids)):
        for quadrant, entry in enumerate(snapshot.definition_words(atlas, metatile)):
            sources.setdefault(entry, set()).add(
                f"{atlas[0].upper()}{metatile:02X}:{QUADRANT_NAMES[quadrant]}")
    return sources


def unique_live_cells(snapshot: Snapshot
                      ) -> dict[tuple[int, ...], list[tuple[int, int]]]:
    result: dict[tuple[int, ...], list[tuple[int, int]]] = {}
    for cell_y in range(32):
        for cell_x in range(32):
            words = snapshot.live_cell_words(cell_x, cell_y)
            result.setdefault(words, []).append((cell_x, cell_y))
    return result


def render_live_cell_sheet(snapshot: Snapshot, output: Path) -> None:
    Image, ImageDraw, ImageFont = pillow()
    font = ImageFont.load_default()
    cells = sorted(unique_live_cells(snapshot).items())
    columns = 16
    cell_w, cell_h = 64, 68
    rows = math.ceil(len(cells) / columns)
    width, height = columns * cell_w, 52 + rows * cell_h
    sheet = Image.new("RGBA", (width, height), "#20232e")
    draw = ImageDraw.Draw(sheet)
    y0 = make_header(
        draw, font, f"{snapshot.title} - unique live 16x16 cells",
        f"{len(cells)} exact BG-word compositions; first coordinate and occurrence count shown",
        width)
    for position, (words, coordinates) in enumerate(cells):
        col, row = position % columns, position // columns
        x, y = col * cell_w, y0 + row * cell_h
        draw.rectangle((x, y, x + cell_w - 1, y + cell_h - 1),
                       outline="#444a5a")
        raw = b"".join(word.to_bytes(2, "little") for word in words)
        key = hashlib.sha256(raw).hexdigest()[:8]
        draw.text((x + 3, y + 3), key, fill="#e9ecef", font=font)
        preview = checker((32, 32), square=4)
        first_x, first_y = coordinates[0]
        preview.alpha_composite(scaled(
            snapshot.render_live_cell(first_x, first_y), 2))
        sheet.alpha_composite(preview, (x + 16, y + 17))
        draw.text((x + 3, y + 52),
                  f"({first_x:02d},{first_y:02d}) x{len(coordinates)}",
                  fill="#adb5bd", font=font)
    sheet.convert("RGB").save(output, optimize=True)


def render_annotated_town(snapshot: Snapshot, output: Path) -> None:
    Image, ImageDraw, ImageFont = pillow()
    font = ImageFont.load_default()
    scale = 2
    margin_left, margin_top = 34, 54
    source = scaled(snapshot.render_town_map(), scale)
    width = margin_left + source.width + 8
    height = margin_top + source.height + 8
    image = Image.new("RGBA", (width, height), "#171923")
    image.alpha_composite(source, (margin_left, margin_top))
    draw = ImageDraw.Draw(image, "RGBA")
    draw.text((8, 8), f"{snapshot.title} - live 32x32-cell town map",
              fill="#f8f9fa", font=font)
    draw.text((8, 27),
              "16px cell grid; colored boxes are active structure-record origins",
              fill="#a9b1c3", font=font)
    for cell in range(33):
        coordinate = margin_left + cell * 16 * scale
        alpha = 125 if cell % 4 == 0 else 55
        draw.line((coordinate, margin_top, coordinate, margin_top + source.height),
                  fill=(255, 255, 255, alpha), width=1)
        coordinate_y = margin_top + cell * 16 * scale
        draw.line((margin_left, coordinate_y, margin_left + source.width, coordinate_y),
                  fill=(255, 255, 255, alpha), width=1)
        if cell < 32 and cell % 4 == 0:
            draw.text((margin_left + cell * 16 * scale + 2, margin_top - 12),
                      str(cell), fill="#adb5bd", font=font)
            draw.text((4, margin_top + cell * 16 * scale + 2),
                      str(cell), fill="#adb5bd", font=font)
    for record in snapshot.structure_records():
        x0 = margin_left + record.cell_x * 16 * scale
        y0 = margin_top + record.cell_y * 16 * scale
        color = RECORD_COLORS[record.class_id % len(RECORD_COLORS)]
        draw.rectangle((x0 + 1, y0 + 1, x0 + 16 * scale - 2,
                        y0 + 16 * scale - 2), outline=color, width=2)
        draw.text((x0 + 3, y0 + 3), str(record.slot), fill=color, font=font,
                  stroke_width=1, stroke_fill="#11131a")
    image.convert("RGB").save(output, optimize=True)


def render_record_crops(snapshot: Snapshot, output: Path) -> None:
    Image, ImageDraw, ImageFont = pillow()
    font = ImageFont.load_default()
    records = snapshot.structure_records()
    columns = 5
    cell_w, cell_h = 156, 164
    rows = max(1, math.ceil(len(records) / columns))
    width, height = columns * cell_w, 52 + rows * cell_h
    sheet = Image.new("RGBA", (width, height), "#20232e")
    draw = ImageDraw.Draw(sheet)
    y0 = make_header(
        draw, font, f"{snapshot.title} - active structure-record neighborhoods",
        "Each crop is its 4x4 selector square; the record origin cell is boxed",
        width)
    town = snapshot.render_town_map()
    for position, record in enumerate(records):
        col, row = position % columns, position // columns
        x, y = col * cell_w, y0 + row * cell_h
        draw.rectangle((x, y, x + cell_w - 1, y + cell_h - 1),
                       outline="#444a5a")
        class_name = STRUCTURE_CLASSES.get(record.class_id,
                                           f"class{record.class_id}")
        draw.text((x + 4, y + 3),
                  f"slot {record.slot:03d} {class_name} sub{record.subtype}",
                  fill="#f8f9fa", font=font)
        draw.text((x + 4, y + 15),
                  f"origin ({record.cell_x},{record.cell_y}) flags ${record.flags:02X}",
                  fill="#adb5bd", font=font)
        square_x = max(0, min(28, record.cell_x & ~3))
        square_y = max(0, min(28, record.cell_y & ~3))
        crop = town.crop((square_x * 16, square_y * 16,
                          square_x * 16 + 64, square_y * 16 + 64))
        crop = scaled(crop, 2)
        sheet.alpha_composite(crop, (x + 14, y + 31))
        origin_x = x + 14 + (record.cell_x - square_x) * 32
        origin_y = y + 31 + (record.cell_y - square_y) * 32
        color = RECORD_COLORS[record.class_id % len(RECORD_COLORS)]
        draw.rectangle((origin_x + 1, origin_y + 1, origin_x + 30,
                        origin_y + 30), outline=color, width=2)
    sheet.convert("RGB").save(output, optimize=True)


def write_draw_list_csv(path: Path, snapshots: list[Snapshot],
                        draw_lists: list[DrawList]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        fields = [
            "town", "draw_list_index", "draw_list_address", "cell_count",
            "width_cells", "height_cells", "commands", "candidate_scope", "category",
            "structure_name", "continuation_group", "footprint_cells",
            "elevation_cells", "notes",
        ]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for snapshot in snapshots:
            for item in draw_lists:
                commands = " ".join(
                    f"{command.dx},{command.dy}:${command.metatile:02X}"
                    for command in item.commands)
                writer.writerow({
                    "town": snapshot.slug,
                    "draw_list_index": item.index,
                    "draw_list_address": f"$03:{item.address:04X}",
                    "cell_count": len(item.commands),
                    "width_cells": item.width_cells,
                    "height_cells": item.height_cells,
                    "commands": commands,
                    "candidate_scope": (
                        "building_review" if item.index in LIKELY_BUILDING_DRAW_LISTS
                        else "all_structure_review"),
                })


def write_bg_tile_csv(path: Path, snapshots: list[Snapshot],
                      structure_ids: set[int]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        fields = [
            "town", "source", "entry", "tile_id", "palette", "hflip",
            "vflip", "metatile_quadrants", "category", "continuation_group",
            "notes",
        ]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for snapshot in snapshots:
            source_sets = (
                ("structure", metatile_entry_sources(
                    snapshot, "structure", structure_ids)),
                ("terrain_used", metatile_entry_sources(
                    snapshot, "terrain", snapshot.used_terrain_metatiles())),
            )
            for source_name, entries in source_sets:
                for entry, origins in sorted(entries.items()):
                    writer.writerow({
                        "town": snapshot.slug,
                        "source": source_name,
                        "entry": f"${entry:04X}",
                        "tile_id": f"${entry & 0x3FF:03X}",
                        "palette": entry >> 10 & 7,
                        "hflip": int(bool(entry & 0x4000)),
                        "vflip": int(bool(entry & 0x8000)),
                        "metatile_quadrants": " ".join(sorted(origins)),
                    })


def write_record_csv(path: Path, snapshots: list[Snapshot]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        fields = ["town", "slot", "cell_x", "cell_y", "class_id",
                  "class_name", "subtype", "under_construction", "action",
                  "flags"]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for snapshot in snapshots:
            for record in snapshot.structure_records():
                writer.writerow({
                    "town": snapshot.slug,
                    "slot": record.slot,
                    "cell_x": record.cell_x,
                    "cell_y": record.cell_y,
                    "class_id": record.class_id,
                    "class_name": STRUCTURE_CLASSES.get(
                        record.class_id, f"class{record.class_id}"),
                    "subtype": record.subtype,
                    "under_construction": int(bool(record.flags & 0x40)),
                    "action": record.action,
                    "flags": f"${record.flags:02X}",
                })


def write_town_cell_csv(path: Path, snapshots: list[Snapshot]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        fields = [
            "town", "cell_x", "cell_y", "visual_key", "cell_map_value",
            "word_tl", "word_tr", "word_bl", "word_br", "tile_tl",
            "tile_tr", "tile_bl", "tile_br", "structure_record_slots",
            "category", "structure_name", "continuation_group", "role",
            "group_origin", "notes",
        ]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for snapshot in snapshots:
            slots_at: dict[tuple[int, int], list[int]] = {}
            for record in snapshot.structure_records():
                slots_at.setdefault((record.cell_x, record.cell_y), []).append(
                    record.slot)
            for cell_y in range(32):
                for cell_x in range(32):
                    words = snapshot.live_cell_words(cell_x, cell_y)
                    writer.writerow({
                        "town": snapshot.slug,
                        "cell_x": cell_x,
                        "cell_y": cell_y,
                        "visual_key": snapshot.live_cell_key(cell_x, cell_y),
                        "cell_map_value": f"${snapshot.cell_map_value(cell_x, cell_y):02X}",
                        "word_tl": f"${words[0]:04X}",
                        "word_tr": f"${words[1]:04X}",
                        "word_bl": f"${words[2]:04X}",
                        "word_br": f"${words[3]:04X}",
                        "tile_tl": f"${words[0] & 0x3FF:03X}",
                        "tile_tr": f"${words[1] & 0x3FF:03X}",
                        "tile_bl": f"${words[2] & 0x3FF:03X}",
                        "tile_br": f"${words[3] & 0x3FF:03X}",
                        "structure_record_slots": " ".join(
                            str(slot) for slot in slots_at.get(
                                (cell_x, cell_y), [])),
                    })


def catalog_dict(rom: bytes, snapshots: list[Snapshot],
                 draw_lists: list[DrawList], structure_ids: set[int]) -> dict:
    return {
        "schema": "actraiser-sim-bg-tile-catalog-v1",
        "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "draw_list_range": "$03:D928-$03:DC73",
        "draw_list_count": len(draw_lists),
        "structure_metatile_count": len(structure_ids),
        "structure_metatiles": [f"${value:02X}" for value in sorted(structure_ids)],
        "draw_lists": [
            {
                "index": item.index,
                "address": f"$03:{item.address:04X}",
                "width_cells": item.width_cells,
                "height_cells": item.height_cells,
                "commands": [
                    {"dx": command.dx, "dy": command.dy,
                     "metatile": f"${command.metatile:02X}"}
                    for command in item.commands
                ],
            }
            for item in draw_lists
        ],
        "towns": [
            {
                "slug": snapshot.slug,
                "name": snapshot.title,
                "map": f"$00/{snapshot.expected_map:02X}",
                "snapshot": str(snapshot.prefix),
                "active_structure_records": len(snapshot.structure_records()),
                "unique_live_cells": len(unique_live_cells(snapshot)),
                "used_cell_metatiles": [
                    f"${value:02X}" for value in sorted(snapshot.used_cell_metatiles())
                ],
                "used_terrain_metatiles": [
                    f"${value:02X}" for value in sorted(snapshot.used_terrain_metatiles())
                ],
                "bg1_chr_sha256": hashlib.sha256(snapshot.vram[:0x8000]).hexdigest(),
                "bg_palette_sha256": hashlib.sha256(snapshot.cgram[:0x100]).hexdigest(),
            }
            for snapshot in snapshots
        ],
    }


def write_readme(path: Path, snapshots: list[Snapshot], draw_lists: list[DrawList],
                 structure_ids: set[int]) -> None:
    lines = [
        "# SIM background tile classification dump",
        "",
        "This is a local, ROM-derived research artifact for the SIM 3D object-extrusion pass.",
        "It deliberately keeps authentic tile IDs, metatile IDs, draw-list addresses, and town",
        "coordinates visible.",
        "",
        f"The building side is exhaustive: {len(draw_lists)} ROM draw lists at",
        f"`$03:D928-$03:DC73` reference {len(structure_ids)} structure metatiles. The terrain",
        "candidate sheets are conservative: they contain every actual terrain metatile ID",
        "present in that town's current 32x32 cell map (the `$E0-$EF` structure marks are",
        "excluded), so trees are not missed; ordinary terrain can simply be left unclassified.",
        "",
        "Use `draw-list-classification.csv` for whole structures and multi-cell relationships.",
        "The building-focused PNG is a visual shortlist for convenience, not a semantic claim;",
        "the exhaustive 94-list sheet and `candidate_scope` column remain available for review.",
        "Use `town-cell-classification.csv` for base landmarks such as the cathedral and for",
        "tree cells found in the full live map; it records all four 8x8 BG words per 16x16 cell.",
        "Use `bg-tile-classification.csv` only when classification needs to reach the underlying",
        "8x8 BG entry. `structure-records.csv` supplies known class/origin evidence from the save.",
        "",
        "The yellow-bordered draw-list previews are multi-cell. Their tiny bottom grids show",
        "metatile IDs in authentic `(dx,dy)` layout, so vertical continuations and wider bases",
        "can be recorded without guessing from numeric adjacency.",
        "",
        "## Town sheets",
        "",
    ]
    for snapshot in snapshots:
        lines.extend([
            f"### {snapshot.title}",
            "",
            f"- [building-focused shortlist]({snapshot.slug}/building-candidates.png)",
            f"- [all structure draw lists]({snapshot.slug}/draw-lists.png)",
            f"- [structure metatile candidates]({snapshot.slug}/structure-metatiles.png)",
            f"- [structure BG entries]({snapshot.slug}/structure-bg-tiles.png)",
            f"- [terrain/tree candidates]({snapshot.slug}/terrain-candidates.png)",
            f"- [terrain candidate BG entries]({snapshot.slug}/terrain-bg-tiles.png)",
            f"- [unique live 16x16 cells]({snapshot.slug}/live-cells.png)",
            f"- [annotated full town]({snapshot.slug}/town-map-annotated.png)",
            f"- [active structure neighborhoods]({snapshot.slug}/structure-records.png)",
            "",
        ])
    lines.extend([
        "## Classification fields",
        "",
        "Suggested `category` values: `building`, `tree`, `flat`, `ignore`, `unknown`.",
        "For a cathedral/factory, give related rows one `continuation_group`, then place",
        "relative cells in `footprint_cells` and `elevation_cells` (for example `0,1` or",
        "`0,0 1,0`). Keep uncertainty in `notes`; the ROM address remains the stable key.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_snapshot_arguments(values: list[str], captures_dir: Path | None
                             ) -> list[tuple[str, Path]]:
    explicit: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--snapshot expects town=prefix, got {value!r}")
        slug, raw_path = value.split("=", 1)
        if slug in explicit:
            raise ValueError(f"duplicate snapshot for {slug}")
        explicit[slug] = strip_snapshot_suffix(Path(raw_path))
    if captures_dir is not None:
        for slug, _, _ in TOWNS:
            if slug not in explicit:
                explicit[slug] = find_snapshot_prefix(captures_dir / slug)
    missing = [slug for slug, _, _ in TOWNS if slug not in explicit]
    if missing:
        raise ValueError("missing snapshots: " + ", ".join(missing))
    extra = sorted(set(explicit) - set(TOWN_BY_SLUG))
    if extra:
        raise ValueError("unknown snapshot towns: " + ", ".join(extra))
    return [(slug, explicit[slug]) for slug, _, _ in TOWNS]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--snapshot", action="append", default=[],
                        help="town=prefix; repeat once per town")
    parser.add_argument("--captures-dir", type=Path,
                        help="directory containing one subdirectory per town")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    rom = args.rom.read_bytes()
    if len(rom) < lorom_offset(DRAW_LIST_BANK, DRAW_LIST_END):
        raise ValueError(f"{args.rom}: ROM is too short ({len(rom)} bytes)")
    draw_lists = parse_draw_lists(rom)
    if len(draw_lists) != 94:
        raise ValueError(f"expected 94 draw lists, found {len(draw_lists)}")
    structure_ids = {command.metatile for item in draw_lists
                     for command in item.commands}
    if len(structure_ids) != 206:
        raise ValueError(
            f"expected 206 referenced structure metatiles, found {len(structure_ids)}")

    snapshots = [Snapshot(slug, prefix) for slug, prefix in
                 parse_snapshot_arguments(args.snapshot, args.captures_dir)]
    args.out.mkdir(parents=True, exist_ok=True)
    for snapshot in snapshots:
        town_dir = args.out / snapshot.slug
        town_dir.mkdir(parents=True, exist_ok=True)
        terrain_ids = snapshot.used_terrain_metatiles()
        render_draw_list_sheet(snapshot, draw_lists, town_dir / "draw-lists.png")
        render_draw_list_sheet(
            snapshot,
            [item for item in draw_lists
             if item.index in LIKELY_BUILDING_DRAW_LISTS],
            town_dir / "building-candidates.png", focused=True)
        render_metatile_sheet(
            snapshot, "structure", structure_ids,
            town_dir / "structure-metatiles.png",
            f"{len(structure_ids)} IDs referenced by the complete ROM draw-list range")
        render_bg_tile_sheet(
            snapshot, metatile_entry_sources(snapshot, "structure", structure_ids),
            town_dir / "structure-bg-tiles.png", "structure BG entries",
            "Unique 8x8 tilemap words used by referenced structure metatiles")
        render_metatile_sheet(
            snapshot, "terrain", terrain_ids,
            town_dir / "terrain-candidates.png",
            f"{len(terrain_ids)} IDs present in this town's live cell map; classify trees only")
        render_bg_tile_sheet(
            snapshot, metatile_entry_sources(snapshot, "terrain", terrain_ids),
            town_dir / "terrain-bg-tiles.png", "terrain/tree candidate BG entries",
            "Unique 8x8 tilemap words in the used terrain candidate set")
        render_live_cell_sheet(snapshot, town_dir / "live-cells.png")
        render_annotated_town(snapshot, town_dir / "town-map-annotated.png")
        render_record_crops(snapshot, town_dir / "structure-records.png")
        print(f"{snapshot.title}: {town_dir}")

    write_draw_list_csv(args.out / "draw-list-classification.csv",
                        snapshots, draw_lists)
    write_bg_tile_csv(args.out / "bg-tile-classification.csv",
                      snapshots, structure_ids)
    write_record_csv(args.out / "structure-records.csv", snapshots)
    write_town_cell_csv(args.out / "town-cell-classification.csv", snapshots)
    (args.out / "catalog.json").write_text(
        json.dumps(catalog_dict(rom, snapshots, draw_lists, structure_ids),
                   indent=2) + "\n", encoding="utf-8")
    write_readme(args.out / "README.md", snapshots, draw_lists, structure_ids)
    print(f"catalog: {args.out / 'README.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
