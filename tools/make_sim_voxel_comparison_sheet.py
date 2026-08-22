#!/usr/bin/env python3
"""Build a numbered original-2D versus production-voxel audit sheet.

The voxel side is read from the production-model renderer manifest emitted by
``sim_voxel_model_sheet``.  The original side is never redrawn: houses and
animated structures are decoded from the ROM/snapshot metatile data, while
town-specific landmarks and palette variants are cropped from authentic town
map renders.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sim_bg_tile_catalog import Snapshot, draw_list_preview, parse_draw_lists


FONT = Path("/System/Library/Fonts/HelveticaNeue.ttc")
TOWNS = {
    1: "Fillmore",
    2: "Bloodpool",
    3: "Kasandora",
    4: "Aitos",
    5: "Marahna",
    6: "Northwall",
}

# House families in the ROM table at $03:DCC6.  Each family owns an eight-
# metatile block; +2 is its final front view and +3 its final alternate view.
HOUSE_FAMILIES = {
    1: (0, 2, 4),
    2: (0, 2, 6),
    3: (0, 8, 10),
    4: (0, 2, 12),
    5: (0, 14, 2),
    6: (0, 2, 10),
}


@dataclass(frozen=True)
class OriginalArt:
    image: Image.Image
    source: str
    detail: str


@dataclass(frozen=True)
class ManifestRow:
    section: str
    label: str
    file: str


def font(size: int, bold: bool = False):
    # Face 0 is regular and face 1 is bold in the system Helvetica Neue TTC.
    return ImageFont.truetype(str(FONT), size=size, index=1 if bold else 0)


def read_manifest(path: Path) -> list[ManifestRow]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        return [ManifestRow(row["section"], row["label"], row["file"])
                for row in reader]


class OriginalLibrary:
    def __init__(self, maps: Path, snapshot_prefix: Path,
                 marahna_snapshot_prefix: Path, rom: Path):
        self.maps = maps
        self.snapshot = Snapshot("aitos", snapshot_prefix)
        self.mar_snapshot = Snapshot("marahna", marahna_snapshot_prefix)
        self.draw_lists = parse_draw_lists(rom.read_bytes())
        self._town_maps: dict[tuple[str, str], Image.Image] = {}

    def town_map(self, town: str, phase: str) -> Image.Image:
        key = (town, phase)
        if key not in self._town_maps:
            source = Image.open(self.maps / f"{town}-{phase}.png").convert("RGBA")
            # SNESMaps adds a 158-pixel title/header above the 32x32-cell map.
            self._town_maps[key] = source.crop((0, 158, 512, 670))
        return self._town_maps[key]

    def map_crop(self, town: str, phase: str, x: int, y: int,
                 width: int = 1, height: int = 1) -> OriginalArt:
        art = self.town_map(town, phase).crop(
            (x * 16, y * 16, (x + width) * 16, (y + height) * 16))
        town_title = town.capitalize() if town != "fillmore" else "Fillmore"
        cell_end = (f"{x},{y}" if width == height == 1 else
                    f"{x},{y}-{x + width - 1},{y + height - 1}")
        return OriginalArt(
            art,
            f"Authentic {town_title} {phase.upper()} map crop",
            f"cells {cell_end}",
        )

    def structure_metatile(self, metatile: int, detail: str) -> OriginalArt:
        return OriginalArt(
            self.snapshot.render_metatile("structure", metatile),
            f"ROM structure metatile MT ${metatile:02X}",
            detail,
        )

    def terrain_metatile(self, snapshot: Snapshot, metatile: int,
                         detail: str) -> OriginalArt:
        return OriginalArt(
            snapshot.render_metatile("terrain", metatile),
            f"ROM terrain metatile MT ${metatile:02X}",
            detail,
        )

    def draw_list(self, index: int, detail: str) -> OriginalArt:
        item = self.draw_lists[index]
        return OriginalArt(
            draw_list_preview(self.snapshot, item),
            f"ROM draw list #{index} at $03:{item.address:04X}",
            detail,
        )

    def house(self, file: str) -> OriginalArt:
        match = re.fullmatch(r"house-(\d)-(\d)-(?:(front)|(alternate))\.bmp", file)
        if not match:
            raise ValueError(file)
        town = int(match.group(1))
        tier = int(match.group(2))
        alternate = bool(match.group(4))
        family = HOUSE_FAMILIES[town][tier]
        metatile = family // 2 * 8 + (3 if alternate else 2)
        facing = "alternate" if alternate else "front"
        return self.structure_metatile(
            metatile,
            f"{TOWNS[town]} tier {tier + 1} {facing}; family ${family:02X}",
        )

    def for_file(self, file: str) -> OriginalArt:
        if file.startswith("house-"):
            return self.house(file)

        # Landmark plots are the exact source-cell footprints used by the SIM
        # map.  Keeping their surroundings out of the crop makes silhouette
        # differences easy to audit.
        map_sources = {
            "cathedral-temperate.bmp": ("fillmore", "b", 13, 13, 2, 2),
            "cathedral-snow.bmp": ("northwall", "b", 9, 21, 2, 2),
            "bloodpool-castle.bmp": ("bloodpool", "a", 6, 16, 2, 2),
            "kasandora-pyramid.bmp": ("kasandora", "a", 20, 4, 2, 2),
            "marahna-temple.bmp": ("marahna", "a", 6, 20, 2, 2),

            # Representative cells use the town's authentic palette.  The
            # underlying evergreen/broadleaf atlas families are shared by all
            # regions, just as the game does.
            "tree-town-1.bmp": ("fillmore", "b", 20, 12, 1, 1),
            "tree-town-2.bmp": ("bloodpool", "b", 28, 5, 1, 1),
            "tree-town-3.bmp": ("kasandora", "b", 26, 1, 1, 1),
            "tree-town-4.bmp": ("aitos", "b", 6, 8, 1, 1),
            "broad-tree-kasandora.bmp": ("kasandora", "b", 24, 29, 1, 1),
            "broad-tree-marahna.bmp": ("marahna", "b", 3, 1, 1, 1),
            "northwall-story-tree.bmp": ("northwall", "b", 26, 14, 2, 2),

            "bridge-temperate-ew.bmp": ("fillmore", "b", 6, 10, 1, 1),
            "bridge-temperate-ns.bmp": ("fillmore", "b", 8, 18, 1, 1),
            "bridge-snow-ew.bmp": ("northwall", "b", 16, 22, 1, 1),
            "bridge-snow-ns.bmp": ("northwall", "b", 9, 26, 1, 1),
        }
        if file in map_sources:
            return self.map_crop(*map_sources[file])

        built_draw_lists = {
            "windmill-phase-0.bmp": (77, "built windmill blade phase 1"),
            "windmill-phase-1.bmp": (78, "built windmill blade phase 2"),
            "windmill-phase-2.bmp": (76, "built windmill blade phase 3"),
            "windmill-snow.bmp": (77, "same built source; Northwall model palette"),
            "factory-temperate.bmp": (89, "completed factory"),
            "factory-snow.bmp": (89, "same completed source; Northwall model palette"),
            "construction-windmill-0.bmp": (73, "construction phase 1"),
            "construction-windmill-1.bmp": (74, "construction phase 2"),
            "construction-windmill-2.bmp": (75, "construction phase 3"),
            "construction-factory.bmp": (88, "factory construction frame"),
        }
        if file in built_draw_lists:
            return self.draw_list(*built_draw_lists[file])

        if file == "construction-house.bmp":
            return self.structure_metatile(0, "first house construction frame")
        if file == "clearable-shrub.bmp":
            return self.terrain_metatile(
                self.snapshot, 0x01, "shared clearable-bush source art")
        if file == "marahna-palm.bmp":
            return self.terrain_metatile(
                self.mar_snapshot, 0x09, "Marahna clearable-palm source art")
        if file == "tree-town-5.bmp":
            return self.terrain_metatile(
                self.mar_snapshot, 0x0B,
                "shared evergreen source in Marahna palette")
        if file == "tree-town-6.bmp":
            return self.terrain_metatile(
                self.snapshot, 0x0B,
                "shared evergreen silhouette; Northwall model palette")

        raise KeyError(f"no authentic original source mapped for {file}")


def wrap_pixels(draw: ImageDraw.ImageDraw, text: str, face,
                max_width: int) -> list[str]:
    words = text.split()
    lines: list[str] = []
    current = ""
    for word in words:
        candidate = word if not current else current + " " + word
        if draw.textbbox((0, 0), candidate, font=face)[2] <= max_width:
            current = candidate
        else:
            if current:
                lines.append(current)
            current = word
    if current:
        lines.append(current)
    return lines


def place_pixel_art(panel: Image.Image, art: Image.Image) -> None:
    art = art.convert("RGBA")
    max_w, max_h = panel.width - 28, panel.height - 24
    integer_scale = max(1, min(max_w // art.width, max_h // art.height))
    scaled = art.resize((art.width * integer_scale,
                         art.height * integer_scale), Image.Resampling.NEAREST)
    x = (panel.width - scaled.width) // 2
    y = (panel.height - scaled.height) // 2
    panel.alpha_composite(scaled, (x, y))


def fit_voxel(panel: Image.Image, source: Image.Image) -> None:
    source = source.convert("RGBA")
    scale = min(panel.width / source.width, panel.height / source.height)
    size = (max(1, round(source.width * scale)),
            max(1, round(source.height * scale)))
    scaled = source.resize(size, Image.Resampling.LANCZOS)
    panel.alpha_composite(
        scaled,
        ((panel.width - scaled.width) // 2, (panel.height - scaled.height) // 2),
    )


def draw_card(sheet: Image.Image, x: int, y: int, width: int, height: int,
              number: int, row: ManifestRow, original: OriginalArt,
              voxel: Image.Image) -> None:
    draw = ImageDraw.Draw(sheet)
    card = (x, y, x + width - 1, y + height - 1)
    draw.rounded_rectangle(card, radius=10, fill="#171d22", outline="#39434a",
                           width=2)

    draw.rounded_rectangle((x + 14, y + 13, x + 72, y + 61), radius=8,
                           fill="#f1c45b")
    draw.text((x + 43, y + 37), f"{number:02d}", anchor="mm",
              fill="#16191c", font=font(25, True))

    label_face = font(20, True)
    label_lines = wrap_pixels(draw, row.label, label_face, width - 104)[:2]
    draw.multiline_text((x + 88, y + 14), "\n".join(label_lines),
                        fill="#f4f6f7", font=label_face, spacing=2)

    panel_y = y + 94
    panel_w = (width - 54) // 2
    panel_h = 218
    left_x, right_x = x + 18, x + 36 + panel_w
    draw.text((left_x, y + 74), "ORIGINAL 2D", fill="#84c7e8",
              font=font(14, True))
    draw.text((right_x, y + 74), "PRODUCTION VOXEL", fill="#d8b86a",
              font=font(14, True))

    original_panel = Image.new("RGBA", (panel_w, panel_h), "#46535c")
    voxel_panel = Image.new("RGBA", (panel_w, panel_h), "#59646b")
    place_pixel_art(original_panel, original.image)
    fit_voxel(voxel_panel, voxel)
    sheet.alpha_composite(original_panel, (left_x, panel_y))
    sheet.alpha_composite(voxel_panel, (right_x, panel_y))
    draw.rectangle((left_x, panel_y, left_x + panel_w - 1,
                    panel_y + panel_h - 1), outline="#6d7c86", width=1)
    draw.rectangle((right_x, panel_y, right_x + panel_w - 1,
                    panel_y + panel_h - 1), outline="#6d7c86", width=1)

    source_face = font(13)
    source_lines = wrap_pixels(draw, original.source, source_face,
                               width - 36)[:1]
    detail_lines = wrap_pixels(draw, original.detail, source_face,
                               width - 36)[:1]
    draw.text((x + 18, y + 325), source_lines[0], fill="#aeb9bf",
              font=source_face)
    draw.text((x + 18, y + 345), detail_lines[0], fill="#7f8b92",
              font=source_face)


def build_sheet(rows: list[ManifestRow], library: OriginalLibrary,
                render_dir: Path, output: Path, index_output: Path) -> None:
    sections: list[str] = []
    by_section: dict[str, list[ManifestRow]] = {}
    for row in rows:
        if row.section not in by_section:
            sections.append(row.section)
            by_section[row.section] = []
        by_section[row.section].append(row)

    width = 2680
    columns = 4
    margin = 32
    gutter = 18
    card_w = (width - margin * 2 - gutter * (columns - 1)) // columns
    card_h = 374
    title_h = 166
    section_h = 58
    section_gap = 24
    row_gap = 16
    footer_h = 112
    total_h = title_h + footer_h
    for section in sections:
        section_rows = math.ceil(len(by_section[section]) / columns)
        total_h += section_h + section_rows * card_h
        total_h += max(0, section_rows - 1) * row_gap + section_gap

    sheet = Image.new("RGBA", (width, total_h), "#0a0e12")
    draw = ImageDraw.Draw(sheet)
    draw.text((margin, 32), "SIM MODE MODEL VISUAL AUDIT",
              fill="#f5f7f8", font=font(46, True))
    draw.text((margin, 92), "Authentic original 2D graphics  ↔  production voxel models",
              fill="#b4bec5", font=font(25))
    draw.text((width - margin, 45), "Reply with item numbers",
              anchor="ra", fill="#f1c45b", font=font(20, True))
    draw.text((width - margin, 80), "Shared original art is labeled explicitly",
              anchor="ra", fill="#89969e", font=font(16))

    index_rows: list[tuple[int, ManifestRow, OriginalArt]] = []
    current_y = title_h
    number = 0
    for section in sections:
        draw.rectangle((0, current_y, width, current_y + section_h),
                       fill="#222a31")
        draw.text((margin, current_y + section_h // 2), section,
                  anchor="lm", fill="#f1c96b", font=font(28, True))
        current_y += section_h
        members = by_section[section]
        for pos, row in enumerate(members):
            grid_x, grid_y = pos % columns, pos // columns
            x = margin + grid_x * (card_w + gutter)
            y = current_y + grid_y * (card_h + row_gap)
            number += 1
            original = library.for_file(row.file)
            voxel_path = render_dir / row.file
            if not voxel_path.is_file():
                raise FileNotFoundError(voxel_path)
            voxel = Image.open(voxel_path)
            draw_card(sheet, x, y, card_w, card_h, number, row,
                      original, voxel)
            index_rows.append((number, row, original))
        current_y += math.ceil(len(members) / columns) * card_h
        current_y += max(0, math.ceil(len(members) / columns) - 1) * row_gap
        current_y += section_gap

    draw.line((margin, total_h - footer_h, width - margin,
               total_h - footer_h), fill="#334049", width=2)
    draw.text((width // 2, total_h - footer_h // 2),
              "Originals are exact ROM metatiles/draw lists or tightly cropped authentic town-map pixels.  "
              "No original art is synthesized.",
              anchor="mm", fill="#8f9ba3", font=font(18))

    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.convert("RGB").save(output, optimize=True)

    with index_output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, delimiter="\t")
        writer.writerow(("number", "section", "label", "voxel_file",
                         "original_source", "original_detail"))
        for item_number, row, original in index_rows:
            writer.writerow((item_number, row.section, row.label, row.file,
                             original.source, original.detail))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--renders", type=Path, required=True)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--marahna-snapshot", type=Path, required=True)
    parser.add_argument("--rom", type=Path, default=Path("ar.sfc"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = read_manifest(args.manifest)
    library = OriginalLibrary(args.maps, args.snapshot,
                              args.marahna_snapshot, args.rom)
    build_sheet(rows, library, args.renders, args.output, args.index)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
