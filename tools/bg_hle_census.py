#!/usr/bin/env python3
"""Census action-background decoder state and compare it with live VRAM.

Each input may be a full-snapshot prefix, a ``.wram.bin`` file, or a directory
containing snapshots. New snapshots also carry ``.ppu.json`` register metadata;
older captures remain useful for source-residency and state-based ring checks,
but are explicitly classified as having unknown PPU eligibility.

Examples:

    python3 tools/bg_hle_census.py runs/20260808-220824/snapshots
    python3 tools/bg_hle_census.py --format jsonl --strict runs/*/snapshots
"""

import argparse
import hashlib
import json
import os
import sys


WRAM_BYTES = 0x20000
VRAM_WORDS = 0x8000
AUTHENTIC_WIDTH = 256
AUTHENTIC_HEIGHT = 224
LAYER_STRIDE = 4
DEFINITION_BYTES = 0x800


class CensusError(Exception):
    """A snapshot is absent or structurally malformed."""


def read_u16(data, address):
    if address < 0 or address + 1 >= len(data):
        raise CensusError("16-bit read outside snapshot")
    return data[address] | (data[address + 1] << 8)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def normalize_prefix(path):
    suffixes = (
        ".wram.bin", ".vram.bin", ".cgram.bin", ".oam.bin",
        ".highoam.bin", ".ppu.json", ".ppm",
    )
    for suffix in suffixes:
        if path.endswith(suffix):
            return path[:-len(suffix)]
    return path


def discover_prefixes(paths):
    found = set()
    for raw_path in paths:
        path = os.path.normpath(raw_path)
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                for name in files:
                    if name.endswith(".wram.bin"):
                        found.add(os.path.join(root, name[:-9]))
            continue
        prefix = normalize_prefix(path)
        if os.path.isfile(prefix + ".wram.bin"):
            found.add(prefix)
        else:
            raise CensusError("no WRAM snapshot for %s" % raw_path)
    return sorted(found)


def load_binary(path, expected_size):
    try:
        with open(path, "rb") as source:
            data = source.read()
    except OSError as error:
        raise CensusError("%s: %s" % (path, error))
    if len(data) != expected_size:
        raise CensusError(
            "%s: expected %d bytes, found %d" %
            (path, expected_size, len(data)))
    return data


def load_ppu(prefix):
    path = prefix + ".ppu.json"
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as source:
            metadata = json.load(source)
    except (OSError, ValueError) as error:
        raise CensusError("%s: %s" % (path, error))
    required = ("bgmode", "inidisp", "bg_sc", "bg_tile_adr",
                "screen_main", "screen_sub")
    if not isinstance(metadata, dict) or any(
            field not in metadata for field in required):
        raise CensusError("%s: incomplete PPU metadata" % path)
    if not isinstance(metadata["bg_sc"], list) or len(metadata["bg_sc"]) != 4:
        raise CensusError("%s: bg_sc must contain four registers" % path)
    return metadata


def decode_layout(wram, layer):
    offset = layer * LAYER_STRIDE
    width = read_u16(wram, 0x2E + offset)
    height = read_u16(wram, 0x30 + offset)
    map_page = read_u16(wram, 0x46 + offset)
    tilemap_base = read_u16(wram, 0x48 + offset)
    table_start = read_u16(wram, 0x52 + offset)
    word_mask = read_u16(wram, 0x54 + offset)
    attributes = wram[0x6B + offset]
    camera_x = read_u16(wram, 0x22 + offset)
    camera_y = read_u16(wram, 0x24 + offset)

    errors = []
    if not width or width % 256:
        errors.append("width-not-page-aligned")
    if not height or height % 256:
        errors.append("height-not-page-aligned")
    pages_wide = width // 256 if width else 0
    pages_high = height // 256 if height else 0
    page_count = pages_wide * pages_high
    map_start = map_page & 0xFF00
    map_size = page_count * 256
    if map_start + map_size > len(wram):
        errors.append("map-out-of-wram")
    if table_start + DEFINITION_BYTES > len(wram):
        errors.append("definitions-out-of-wram")
    if tilemap_base + 0x1000 > VRAM_WORDS:
        errors.append("ring-out-of-vram")

    map_bytes = (wram[map_start:map_start + map_size]
                 if map_start + map_size <= len(wram) else b"")
    definitions = (wram[table_start:table_start + DEFINITION_BYTES]
                   if table_start + DEFINITION_BYTES <= len(wram) else b"")
    return {
        "layer": layer + 1,
        "camera": [camera_x, camera_y],
        "world_pixels": [width, height],
        "world_tiles": [width // 8, height // 8],
        "pages": [pages_wide, pages_high],
        "page_count": page_count,
        "map_page": map_page,
        "map_start": map_start,
        "map_size": map_size,
        "metatile_table": table_start,
        "word_mask": word_mask,
        "attributes": attributes,
        "tilemap_base": tilemap_base,
        "map_sha256": sha256_bytes(map_bytes) if map_bytes else None,
        "definitions_sha256": (
            sha256_bytes(definitions) if definitions else None),
        "nonzero_map_bytes": sum(value != 0 for value in map_bytes),
        "unique_metatiles": len(set(map_bytes)),
        "valid": not errors,
        "errors": errors,
    }


def ppu_layer_state(ppu, layout):
    if ppu is None:
        return {
            "metadata": "missing",
            "eligible": None,
            "reasons": ["ppu-metadata-missing"],
            "chr_base_word": None,
        }
    layer = layout["layer"] - 1
    bgsc = int(ppu["bg_sc"][layer])
    enabled = int(ppu["screen_main"]) | int(ppu["screen_sub"])
    reasons = []
    if int(ppu["inidisp"]) & 0x80:
        reasons.append("forced-blank")
    if int(ppu["bgmode"]) & 7 != 1:
        reasons.append("non-mode1")
    if not (enabled & (1 << layer)):
        reasons.append("layer-disabled")
    if bgsc & 3 != 3:
        reasons.append("tilemap-not-64x64")
    if (bgsc & 0xFC) << 8 != layout["tilemap_base"]:
        reasons.append("tilemap-base-mismatch")
    return {
        "metadata": "present",
        "eligible": not reasons and layout["valid"],
        "reasons": reasons,
        "bgsc": bgsc,
        "chr_base_word": (
            (int(ppu["bg_tile_adr"]) >> (layer * 4) & 0xF) << 12),
        "enabled_main": bool(int(ppu["screen_main"]) & (1 << layer)),
        "enabled_sub": bool(int(ppu["screen_sub"]) & (1 << layer)),
    }


def ring_address(tilemap_base, tile_x, tile_y):
    x = tile_x & 63
    y = tile_y & 63
    return (tilemap_base + (x & 31) + (y & 31) * 0x20 +
            (0x400 if x & 32 else 0) + (0x800 if y & 32 else 0))


def decoded_word(wram, layout, tile_x, tile_y):
    page = ((tile_y >> 5) * layout["pages"][0] + (tile_x >> 5))
    metatile = (((tile_y >> 1) & 15) * 16 + ((tile_x >> 1) & 15))
    metatile_id = wram[layout["map_start"] + page * 256 + metatile]
    quadrant = ((tile_y & 1) << 1) | (tile_x & 1)
    address = layout["metatile_table"] + metatile_id * 8 + quadrant * 2
    return ((read_u16(wram, address) & layout["word_mask"]) |
            (layout["attributes"] << 8))


def authentic_tile_bounds(camera_x, camera_y):
    """Return the exact tile interval sampled by native PPU scanout.

    Horizontal pixels are 0..255. ActRaiser's PPU scanlines are numbered
    1..224, so the vertical interval intentionally has different endpoints.
    Keep this arithmetic identical to ActRaiserActionBg_CompareLayer.
    """
    return (
        camera_x // 8,
        (camera_x + AUTHENTIC_WIDTH - 1) // 8,
        (camera_y + 1) // 8,
        (camera_y + AUTHENTIC_HEIGHT) // 8,
    )


def compare_view(wram, vram, layout):
    result = {
        "available": bool(vram) and layout["valid"],
        "compared": 0,
        "mismatches": 0,
        "outside_world": 0,
        "first_mismatch": None,
    }
    if not result["available"]:
        return result
    camera_x, camera_y = layout["camera"]
    first_x, last_x, first_y, last_y = authentic_tile_bounds(
        camera_x, camera_y)
    world_width, world_height = layout["world_tiles"]
    for tile_y in range(first_y, last_y + 1):
        for tile_x in range(first_x, last_x + 1):
            if (tile_x >= world_width or tile_y >= world_height):
                result["outside_world"] += 1
                continue
            expected = decoded_word(wram, layout, tile_x, tile_y)
            address = ring_address(layout["tilemap_base"], tile_x, tile_y)
            native = vram[address]
            result["compared"] += 1
            if expected == native:
                continue
            if result["first_mismatch"] is None:
                result["first_mismatch"] = {
                    "tile": [tile_x, tile_y],
                    "hle": expected,
                    "native": native,
                    "vram_word": address,
                }
            result["mismatches"] += 1
    return result


def analyze_snapshot(prefix, rom_sha256=None):
    wram = load_binary(prefix + ".wram.bin", WRAM_BYTES)
    vram_path = prefix + ".vram.bin"
    vram = None
    if os.path.isfile(vram_path):
        raw_vram = load_binary(vram_path, VRAM_WORDS * 2)
        vram = [raw_vram[i] | (raw_vram[i + 1] << 8)
                for i in range(0, len(raw_vram), 2)]
    ppu = load_ppu(prefix)
    layers = []
    for layer_index in range(2):
        layout = decode_layout(wram, layer_index)
        layout["ppu"] = ppu_layer_state(ppu, layout)
        layout["comparison"] = compare_view(wram, vram, layout)
        layers.append(layout)
    return {
        "format": 1,
        "record_type": "snapshot",
        "snapshot": prefix,
        "rom_sha256": rom_sha256,
        "map_group": wram[0x18],
        "map_number": wram[0x19],
        "game_frame": read_u16(wram, 0x88),
        "action_map": 1 <= wram[0x18] <= 7,
        "ppu_metadata": ppu is not None,
        "vram_present": vram is not None,
        "layers": layers,
    }


def build_summary(records):
    groups = {}
    for record in records:
        if not record["action_map"]:
            continue
        for layer in record["layers"]:
            key = "%02X/%02X/BG%d" % (
                record["map_group"], record["map_number"], layer["layer"])
            group = groups.setdefault(key, {
                "snapshots": 0,
                "map_hashes": set(),
                "definition_hashes": set(),
                "descriptors": set(),
                "eligible": {"yes": 0, "no": 0, "unknown": 0},
                "tiles_compared": 0,
                "mismatches": 0,
                "outside_world": 0,
            })
            group["snapshots"] += 1
            if layer["map_sha256"]:
                group["map_hashes"].add(layer["map_sha256"])
            if layer["definitions_sha256"]:
                group["definition_hashes"].add(layer["definitions_sha256"])
            group["descriptors"].add((
                tuple(layer["world_pixels"]), layer["map_start"],
                layer["map_size"], layer["metatile_table"],
                layer["word_mask"], layer["attributes"],
                layer["tilemap_base"]))
            eligibility = layer["ppu"]["eligible"]
            eligibility_key = (
                "yes" if eligibility is True else
                "no" if eligibility is False else "unknown")
            group["eligible"][eligibility_key] += 1
            comparison = layer["comparison"]
            group["tiles_compared"] += comparison["compared"]
            group["mismatches"] += comparison["mismatches"]
            group["outside_world"] += comparison["outside_world"]

    serialized_groups = {}
    for key, group in sorted(groups.items()):
        serialized_groups[key] = {
            "snapshots": group["snapshots"],
            "map_variants": len(group["map_hashes"]),
            "definition_variants": len(group["definition_hashes"]),
            "descriptor_variants": len(group["descriptors"]),
            "eligible": group["eligible"],
            "tiles_compared": group["tiles_compared"],
            "mismatches": group["mismatches"],
            "outside_world": group["outside_world"],
        }
    return {
        "format": 1,
        "record_type": "summary",
        "snapshots": len(records),
        "action_snapshots": sum(record["action_map"] for record in records),
        "action_maps": len(set(
            (record["map_group"], record["map_number"])
            for record in records if record["action_map"])),
        "ppu_metadata_snapshots": sum(
            record["ppu_metadata"] for record in records),
        "groups": serialized_groups,
    }


def snapshot_failed(record, require_ppu):
    if not record["action_map"]:
        return False
    if require_ppu and not record["ppu_metadata"]:
        return True
    for layer in record["layers"]:
        comparison = layer["comparison"]
        if not layer["valid"]:
            return True
        if layer["ppu"]["eligible"] is False:
            continue
        if not comparison["available"]:
            return True
        if comparison["mismatches"]:
            return True
    return False


def format_human(record):
    lines = [
        "%s map=%02X/%02X gf=%u%s" % (
            record["snapshot"], record["map_group"], record["map_number"],
            record["game_frame"],
            "" if record["action_map"] else " [non-action]"),
    ]
    if not record["action_map"]:
        return lines
    for layer in record["layers"]:
        comparison = layer["comparison"]
        ppu = layer["ppu"]
        eligibility = ("yes" if ppu["eligible"] is True else
                       "no" if ppu["eligible"] is False else "unknown")
        lines.append(
            "  BG%d %dx%d map=$%05X+%d table=$%04X ring=$%04X "
            "eligible=%s compare=%d mismatch=%d outside=%d" % (
                layer["layer"], layer["world_pixels"][0],
                layer["world_pixels"][1], layer["map_start"],
                layer["map_size"], layer["metatile_table"],
                layer["tilemap_base"], eligibility,
                comparison["compared"], comparison["mismatches"],
                comparison["outside_world"]))
        reasons = layer["errors"] + ppu["reasons"]
        if reasons:
            lines.append("      reasons=" + ",".join(reasons))
    return lines


def format_human_summary(summary):
    lines = [
        "summary snapshots=%d action=%d maps=%d ppu-metadata=%d" % (
            summary["snapshots"], summary["action_snapshots"],
            summary["action_maps"], summary["ppu_metadata_snapshots"]),
    ]
    for key, group in summary["groups"].items():
        lines.append(
            "  %s samples=%d variants=map:%d defs:%d descriptor:%d "
            "eligible=%d/%d/%d compare=%d mismatch=%d outside=%d" % (
                key, group["snapshots"], group["map_variants"],
                group["definition_variants"], group["descriptor_variants"],
                group["eligible"]["yes"], group["eligible"]["no"],
                group["eligible"]["unknown"], group["tiles_compared"],
                group["mismatches"], group["outside_world"]))
    return lines


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="snapshot prefixes or directories")
    parser.add_argument("--format", choices=("human", "jsonl"), default="human")
    parser.add_argument("--rom", default="ar.sfc",
                        help="ROM whose SHA-256 identifies the census (default: ar.sfc)")
    parser.add_argument("--strict", action="store_true",
                        help="fail on malformed action layers or tile mismatches")
    parser.add_argument("--require-ppu", action="store_true",
                        help="with --strict, also fail snapshots lacking .ppu.json")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        prefixes = discover_prefixes(args.inputs)
        rom_hash = file_sha256(args.rom) if os.path.isfile(args.rom) else None
        records = [analyze_snapshot(prefix, rom_hash) for prefix in prefixes]
    except (CensusError, OSError) as error:
        print("bg_hle_census: %s" % error, file=sys.stderr)
        return 2
    if not records:
        print("bg_hle_census: no snapshots found", file=sys.stderr)
        return 2
    summary = build_summary(records)
    for record in records:
        if args.format == "jsonl":
            print(json.dumps(record, sort_keys=True, separators=(",", ":")))
        else:
            for line in format_human(record):
                print(line)
    if args.format == "jsonl":
        print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
    else:
        for line in format_human_summary(summary):
            print(line)
    if args.strict and any(
            snapshot_failed(record, args.require_ppu) for record in records):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
