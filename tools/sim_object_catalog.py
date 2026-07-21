#!/usr/bin/env python3
"""Crawl and render ActRaiser's simulation-mode object data.

This tool deliberately starts from ROM tables rather than recorded gameplay:

* $01:B8D0: 26 live-record dispatch classes and their state tables.
* $01:E099: 52 world-behavior scripts.
* $01:E7D9: 64 world visual identities/compositions.
* $01:A227: 73 spawn-list families and their animation-variant arrays.

The optional renderer applies those ROM composition records to an exact
VRAM/CGRAM snapshot produced by AR_VRAMDUMP_GF.  A snapshot is still needed
for character pixels and colors, but it does not determine which identities
are catalogued.

Examples:
  python3 tools/sim_object_catalog.py crawl --json docs/sim-object-catalog.json
  python3 tools/sim_object_catalog.py render \
      --snapshot runs/.../snapshots/vd_gf1000 \
      --out-dir docs/images/sim-object-catalog
"""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ROM_BANK = 0x01

TYPE_HANDLER_TABLE = 0xB8D0
TYPE_HANDLER_COUNT = 26
WORLD_BEHAVIOR_TABLE = 0xE099
WORLD_BEHAVIOR_COUNT = 52
WORLD_VISUAL_TABLE = 0xE7D9
WORLD_VISUAL_COUNT = 64
SPAWN_LIST_TABLE = 0xA227
SPAWN_LIST_COUNT = 73
SPAWN_SHARED_VARIANTS_START = 0xA2B9
SPAWN_SHARED_VARIANTS_END = 0xA399

KNOWN_RECORD_CLASSES = {
    0x00: "town actor / person family (observed)",
    0x0C: "angel special record (observed)",
    0x12: "Blue Dragon",
    0x13: "Napper Bat",
    0x14: "Red Demon",
    0x15: "Skull Head",
}

KNOWN_RECORD_CLASS_PLANES = {
    0x12: "flying_fixed",
    0x13: "flying_dynamic",
    0x14: "flying_fixed",
    0x15: "flying_fixed",
}

KNOWN_WORLD_VISUALS = {
    0x09: {"name": "building-zap lightning frame 1", "plane": "ground_targeted_effect"},
    0x0A: {"name": "building-zap lightning frame 2", "plane": "ground_targeted_effect"},
    0x0B: {"name": "building-zap lightning frame 3", "plane": "ground_targeted_effect"},
    0x2F: {"name": "people group frame 1", "plane": "grounded_group"},
    0x30: {"name": "people group frame 2", "plane": "grounded_group"},
    0x31: {"name": "people group frame 3", "plane": "grounded_group"},
    0x32: {"name": "people group frame 4", "plane": "grounded_group"},
    0x33: {"name": "people group frame 5", "plane": "grounded_group"},
    0x34: {"name": "ground fire frame 1", "plane": "ground_effect"},
    0x35: {"name": "ground fire frame 2", "plane": "ground_effect"},
    0x36: {"name": "ground fire frame 3", "plane": "ground_effect"},
    0x3A: {"name": "Napper ground-pluck frame 1", "plane": "semi_grounded"},
    0x3B: {"name": "Napper ground-pluck frame 2", "plane": "semi_grounded"},
    0x3C: {"name": "Napper ground-pluck frame 3", "plane": "semi_grounded"},
}

# Human-reviewed composition identities.  These are semantic facts the ROM's
# part records cannot express on their own; keep them keyed by stable ROM
# address so a fresh crawl retains the classification.
KNOWN_SPAWN_VISUALS = {
    0xD967: {
        "name": "angel arrow vertical composition A",
        "plane": "flying_projectile",
        "anchor": "record_origin",
        "casts_shadow": False,
    },
    0xD972: {
        "name": "angel arrow vertical composition B",
        "plane": "flying_projectile",
        "anchor": "record_origin",
        "casts_shadow": False,
    },
    0xD97D: {
        "name": "angel arrow horizontal composition A",
        "plane": "flying_projectile",
        "anchor": "record_origin",
        "casts_shadow": False,
    },
    0xD988: {
        "name": "angel arrow horizontal composition B",
        "plane": "flying_projectile",
        "anchor": "record_origin",
        "casts_shadow": False,
    },
    0xE940: {"name": "horse facing right, frame 1", "plane": "grounded"},
    0xE94B: {"name": "horse facing right, frame 2", "plane": "grounded"},
    0xE956: {"name": "horse facing left, frame 1", "plane": "grounded"},
    0xE961: {"name": "horse facing left, frame 2", "plane": "grounded"},
    0xE96C: {"name": "dog facing right, frame 1", "plane": "grounded"},
    0xE972: {"name": "dog facing right, frame 2", "plane": "grounded"},
    0xE978: {"name": "dog facing left, frame 1", "plane": "grounded"},
    0xE97E: {"name": "dog facing left, frame 2", "plane": "grounded"},
    0xE984: {"name": "sheep facing right, frame 1", "plane": "grounded"},
    0xE98A: {"name": "sheep facing right, frame 2", "plane": "grounded"},
    0xE990: {"name": "sheep facing left, frame 1", "plane": "grounded"},
    0xE996: {"name": "sheep facing left, frame 2", "plane": "grounded"},
    0xE99C: {"name": "sailboat facing down, sail frame 1", "plane": "water_plane"},
    0xE9A2: {"name": "sailboat facing down, sail frame 2", "plane": "water_plane"},
    0xE9A8: {"name": "sailboat facing up, sail frame 1", "plane": "water_plane"},
    0xE9AE: {"name": "sailboat facing up, sail frame 2", "plane": "water_plane"},
    0xE9B4: {"name": "sailboat facing right, sail frame 1", "plane": "water_plane"},
    0xE9BA: {"name": "sailboat facing right, sail frame 2", "plane": "water_plane"},
    0xE9C0: {"name": "sailboat facing left, sail frame 1", "plane": "water_plane"},
    0xE9C6: {"name": "sailboat facing left, sail frame 2", "plane": "water_plane"},
    0xEBE8: {"name": "people/horse scene metatile 1", "plane": "ground_scene_metatile"},
    0xEBF3: {"name": "people/horse scene metatile 2", "plane": "ground_scene_metatile"},
    0xEBFE: {"name": "people/horse scene metatile 3", "plane": "ground_scene_metatile"},
    0xEC09: {"name": "people/horse scene metatile 4", "plane": "ground_scene_metatile"},
    0xEC14: {"name": "angel cloud/miracle tile 1", "plane": "flying_effect"},
    0xEC1F: {"name": "angel cloud/miracle tile 2", "plane": "flying_effect"},
    0xEC2A: {"name": "angel cloud/miracle tile 3", "plane": "flying_effect"},
    0xEC35: {"name": "angel cloud/miracle tile 4", "plane": "flying_effect"},
}


def lorom_offset(bank: int, address: int) -> int:
    if address < 0x8000:
        raise ValueError(f"not ROM-mapped: ${bank:02X}:{address:04X}")
    return (bank & 0x7F) * 0x8000 + address - 0x8000


class Rom:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()

    def bytes(self, address: int, count: int, bank: int = ROM_BANK) -> bytes:
        offset = lorom_offset(bank, address)
        result = self.data[offset:offset + count]
        if len(result) != count:
            raise ValueError(f"read past ROM at ${bank:02X}:{address:04X}")
        return result

    def u8(self, address: int, bank: int = ROM_BANK) -> int:
        return self.bytes(address, 1, bank)[0]

    def u16(self, address: int, bank: int = ROM_BANK) -> int:
        return int.from_bytes(self.bytes(address, 2, bank), "little")


def signed8(value: int) -> int:
    return value - 0x100 if value >= 0x80 else value


def sim_part_offset(value: int) -> int:
    """Match the ROM: $81-$FF are negative, while $80 remains +128."""
    return value - 0x100 if value >= 0x81 else value


def composition(rom: Rom, address: int) -> dict:
    count = rom.u8(address)
    if count > 64:
        raise ValueError(f"implausible composition count {count} at $01:{address:04X}")
    parts = []
    for index in range(count):
        at = address + 1 + index * 5
        flags, raw_x, raw_y = rom.bytes(at, 3)
        attr = rom.u16(at + 3)
        size = 16 if flags & 1 else 8
        x = sim_part_offset(raw_x)
        y = sim_part_offset(raw_y)
        parts.append({
            "index": index,
            "flags": flags,
            "x": x,
            "y": y,
            "size": size,
            "tile": attr & 0x1FF,
            "palette": (attr >> 9) & 7,
            "priority": (attr >> 12) & 3,
            "hflip": bool(attr & 0x4000),
            "vflip": bool(attr & 0x8000),
            "attr": attr,
        })
    if parts:
        left = min(part["x"] for part in parts)
        top = min(part["y"] for part in parts)
        right = max(part["x"] + part["size"] for part in parts)
        bottom = max(part["y"] + part["size"] for part in parts)
    else:
        left = top = right = bottom = 0
    return {
        "address": address,
        "part_count": count,
        "bounds": [left, top, right, bottom],
        "priorities": sorted({part["priority"] for part in parts}),
        "parts": parts,
    }


def parse_world_behaviors(rom: Rom) -> list[dict]:
    pointers = [rom.u16(WORLD_BEHAVIOR_TABLE + index * 2)
                for index in range(WORLD_BEHAVIOR_COUNT)]
    end_pointers = pointers[1:] + [WORLD_BEHAVIOR_TABLE]
    behaviors = []
    for identity, (start, end) in enumerate(zip(pointers, end_pointers)):
        size = end - start
        if size < 1 or (size - 1) % 4:
            raise ValueError(
                f"behavior {identity:02X} has unexpected span ${start:04X}-${end:04X}")
        frames = []
        for at in range(start, end - 1, 4):
            visual, duration, dx, dy = rom.bytes(at, 4)
            frames.append({
                "address": at,
                "visual_id": visual,
                "duration": duration,
                "velocity_x": signed8(dx),
                "velocity_y": signed8(dy),
            })
        behaviors.append({
            "id": identity,
            "address": start,
            "terminator": rom.u8(end - 1),
            "frames": frames,
        })
    return behaviors


def find_state_table(rom: Rom, handler: int) -> int | None:
    if rom.u8(handler) == 0x60:  # stand-alone RTS/no-op class
        return None
    # Most wrappers start with LDY #table.  Skull Head first performs a world
    # bounds check, so allow a short prologue before the LDY/dispatch pair.
    raw = rom.bytes(handler, 40)
    for offset in range(len(raw) - 6):
        if raw[offset] != 0xA0:  # LDY #imm16, X=16 bit
            continue
        table = raw[offset + 1] | raw[offset + 2] << 8
        tail = raw[offset + 3:offset + 9]
        if tail.startswith(bytes((0x20, 0x4E, 0xD0))):  # JSR $D04E
            return table
        if tail.startswith(bytes((0x82,))):  # BRL $D04E
            displacement = int.from_bytes(tail[1:3], "little", signed=True)
            brl_pc = handler + offset + 3
            if (brl_pc + 3 + displacement) & 0xFFFF == 0xD04E:
                return table
    return None


def parse_state_table(rom: Rom, address: int) -> tuple[list[int], int]:
    # Entries contain target-1.  The first *local* handler follows the table,
    # but monster tables may also reuse an earlier class's handler.  Ignore
    # those backwards references when finding the boundary.
    cursor = address
    minimum_local = 0x10000
    while cursor < minimum_local:
        target = (rom.u16(cursor) + 1) & 0xFFFF
        if address < target < minimum_local:
            minimum_local = target
        cursor += 2
        if cursor - address > 128:
            break
    count = (minimum_local - address) // 2
    if (count < 1 or count > 64 or
            address + count * 2 != minimum_local):
        raise ValueError(f"cannot size state table $01:{address:04X}")
    targets = [((rom.u16(address + index * 2) + 1) & 0xFFFF)
               for index in range(count)]
    local_targets = [target for target in targets if target > address]
    if not local_targets or address + count * 2 != min(local_targets):
        raise ValueError(f"state table $01:{address:04X} failed boundary validation")
    return targets, count


def immediate_calls_in_range(rom: Rom, start: int, end: int,
                             callee: int) -> list[int]:
    raw = rom.bytes(start, max(0, end - start))
    result = []
    pattern_tail = bytes((0x20, callee & 0xFF, callee >> 8))
    for offset in range(max(0, len(raw) - 5)):
        if raw[offset] == 0xA9 and raw[offset + 3:offset + 6] == pattern_tail:
            result.append(raw[offset + 1] | raw[offset + 2] << 8)
    return sorted(set(result))


def immediate_state_writes(rom: Rom, start: int, end: int) -> list[int]:
    raw = rom.bytes(start, max(0, end - start))
    result = []
    # LDA #imm16; STA $0012,X
    for offset in range(max(0, len(raw) - 5)):
        if (raw[offset] == 0xA9 and
                raw[offset + 3:offset + 6] == b"\x9d\x12\x00"):
            result.append((raw[offset + 1] | raw[offset + 2] << 8) & 0x7FFF)
    return sorted(set(result))


def parse_record_classes(rom: Rom) -> list[dict]:
    raw_handlers = [rom.u16(TYPE_HANDLER_TABLE + index * 2)
                    for index in range(TYPE_HANDLER_COUNT)]
    handlers = [((value + 1) & 0xFFFF) for value in raw_handlers]
    tables = {}
    all_state_handlers = set()
    for handler in sorted(set(handlers)):
        table = find_state_table(rom, handler)
        if table is None:
            tables[handler] = None
            continue
        targets, count = parse_state_table(rom, table)
        tables[handler] = (table, targets, count)
        all_state_handlers.update(targets)

    boundaries = sorted(all_state_handlers | set(handlers) | {0xD036})
    classes = []
    for identity, (raw_handler, handler) in enumerate(zip(raw_handlers, handlers)):
        table_info = tables[handler]
        states = []
        if table_info:
            table, targets, _ = table_info
            for state, target in enumerate(targets):
                larger = [candidate for candidate in boundaries if candidate > target]
                end = min(larger) if larger else 0xD036
                behaviors = immediate_calls_in_range(rom, target, end, 0xD072)
                spawn_commands = immediate_calls_in_range(
                    rom, target, end, 0xCFF2)
                transitions = immediate_state_writes(rom, target, end)
                states.append({
                    "state": state,
                    "handler": target,
                    "scan_end": end,
                    "behavior_ids": [value for value in behaviors
                                     if value < WORLD_BEHAVIOR_COUNT],
                    "other_d072_values": [value for value in behaviors
                                           if value >= WORLD_BEHAVIOR_COUNT],
                    "spawn_commands": [{
                        "raw": value,
                        # CFF2 stores low A in $033D (variant), XBA, then
                        # stores high A in $033C (list id).
                        "list_id": (value >> 8) & 0xFF,
                        "variant": value & 0xFF,
                    } for value in spawn_commands],
                    "state_writes": transitions,
                })
        classes.append({
            "id": identity,
            "known_name": KNOWN_RECORD_CLASSES.get(identity),
            "plane": KNOWN_RECORD_CLASS_PLANES.get(identity),
            "handler_table_word": raw_handler,
            "handler": handler,
            "state_table": table_info[0] if table_info else None,
            "states": states,
        })
    return classes


def walk_spawn_script(rom: Rom, address: int) -> dict:
    pc = address
    loop_count = 0
    # AC36 initializes +06 to the script start.  FF can later replace it.
    loop_base = address
    frames = []
    controls = []
    visited = set()
    for _ in range(512):
        state = (pc, loop_count, loop_base)
        if state in visited:
            controls.append({"address": pc, "opcode": "cycle"})
            break
        visited.add(state)
        opcode = rom.u8(pc)
        if opcode == 0xFD:
            controls.append({"address": pc, "opcode": "hide"})
            break
        if opcode == 0xFF:
            loop_count = rom.u8(pc + 1)
            loop_base = pc + 2
            controls.append({
                "address": pc, "opcode": "set_loop", "count": loop_count,
            })
            pc += 2
            continue
        if opcode == 0xFE:
            controls.append({"address": pc, "opcode": "loop"})
            if loop_count == 0:
                # The 16-bit DEC underflows and repeats for 65535 passes.  For
                # catalog purposes this is an intentional cycle, not a reason
                # to unroll the same frames thousands of times.
                controls.append({"address": pc, "opcode": "cycle"})
                break
            loop_count = (loop_count - 1) & 0xFFFF
            if loop_count != 0:
                pc = loop_base
            else:
                pc += 1
            continue
        frame = rom.u16(pc + 1)
        frames.append({"address": pc, "duration": opcode,
                       "composition": frame})
        pc += 3
    else:
        controls.append({"address": pc, "opcode": "limit"})
    return {
        "address": address,
        "frames": frames,
        "controls": controls,
    }


def parse_spawn_lists(rom: Rom) -> tuple[list[dict], list[dict]]:
    lists = []
    script_addresses = set()
    for identity in range(SPAWN_LIST_COUNT):
        variants_address = rom.u16(SPAWN_LIST_TABLE + identity * 2)
        if SPAWN_SHARED_VARIANTS_START <= variants_address < SPAWN_SHARED_VARIANTS_END:
            # Town-structure lists point into a shared sliding array.  The
            # day-cycle selector consumes up to five entries, truncated at
            # the array boundary for its last few list IDs.
            variant_count = min(
                5, (SPAWN_SHARED_VARIANTS_END - variants_address) // 2)
        else:
            # Standalone arrays are immediately followed by their first
            # script.  Use the minimum target rather than entry zero because
            # some arrays deliberately share/reorder scripts.
            minimum_script = rom.u16(variants_address)
            cursor = variants_address
            while cursor < minimum_script:
                candidate = rom.u16(cursor)
                # Lists may also reference a shared script that precedes this
                # local pointer array (notably the people/construction list).
                # Such a target is valid but cannot mark this array's end.
                if variants_address < candidate < minimum_script:
                    minimum_script = candidate
                cursor += 2
            variant_count = (minimum_script - variants_address) // 2
            if variant_count < 1 or variant_count > 64:
                raise ValueError(
                    f"cannot size spawn-list {identity:02X} variant array "
                    f"$01:{variants_address:04X}")
        variants = [rom.u16(variants_address + index * 2)
                    for index in range(variant_count)]
        script_addresses.update(variants)
        lists.append({
            "id": identity,
            "variants_address": variants_address,
            "variant_count": variant_count,
            "script_addresses": variants,
        })
    scripts = [walk_spawn_script(rom, address)
               for address in sorted(script_addresses)]
    return lists, scripts


def crawl(rom: Rom) -> dict:
    behaviors = parse_world_behaviors(rom)
    behavior_refs = defaultdict(list)
    for behavior in behaviors:
        for frame in behavior["frames"]:
            behavior_refs[frame["visual_id"]].append(behavior["id"])

    visuals = []
    for identity in range(WORLD_VISUAL_COUNT):
        address = rom.u16(WORLD_VISUAL_TABLE + identity * 2)
        try:
            item = composition(rom, address)
        except ValueError as error:
            # Visual zero is a control/sentinel entry ($831C), not a normal
            # count-plus-parts composition.  Preserve it in the exhaustive
            # identity table without pretending it is drawable.
            item = {
                "address": address, "error": str(error), "parts": [],
                "part_count": 0, "bounds": [0, 0, 0, 0],
                "priorities": [],
            }
        item["id"] = identity
        item["behavior_ids"] = sorted(set(behavior_refs[identity]))
        if identity in KNOWN_WORLD_VISUALS:
            item["semantic"] = KNOWN_WORLD_VISUALS[identity]
        visuals.append(item)

    spawn_lists, spawn_scripts = parse_spawn_lists(rom)
    fixed_compositions = {}
    fixed_refs = defaultdict(list)
    for script in spawn_scripts:
        for frame in script["frames"]:
            address = frame["composition"]
            fixed_refs[address].append(script["address"])
            if address not in fixed_compositions:
                try:
                    fixed_compositions[address] = composition(rom, address)
                except ValueError as error:
                    fixed_compositions[address] = {
                        "address": address, "error": str(error), "parts": [],
                        "part_count": 0, "bounds": [0, 0, 0, 0],
                        "priorities": [],
                    }
    fixed_visuals = []
    for address in sorted(fixed_compositions):
        item = fixed_compositions[address]
        item["script_addresses"] = sorted(set(fixed_refs[address]))
        if address in KNOWN_SPAWN_VISUALS:
            item["semantic"] = KNOWN_SPAWN_VISUALS[address]
        fixed_visuals.append(item)

    return {
        "schema": "actraiser-sim-object-catalog-v1",
        "rom_tables": {
            "record_classes": "$01:B8D0",
            "world_behaviors": "$01:E099",
            "world_visuals": "$01:E7D9",
            "spawn_lists": "$01:A227",
        },
        "record_classes": parse_record_classes(rom),
        "world_behaviors": behaviors,
        "world_visuals": visuals,
        "spawn_lists": spawn_lists,
        "spawn_scripts": spawn_scripts,
        "spawn_visuals": fixed_visuals,
    }


def hexify_catalog(value):
    """Keep JSON numeric for processing; add no presentation-only hex strings."""
    return value


class Snapshot:
    def __init__(self, prefix: Path, obj_base1: int, obj_base2: int):
        self.vram = Path(str(prefix) + ".vram.bin").read_bytes()
        self.cgram = Path(str(prefix) + ".cgram.bin").read_bytes()
        if len(self.vram) != 0x10000:
            raise ValueError(f"expected 64 KiB VRAM, got {len(self.vram)} bytes")
        if len(self.cgram) != 0x200:
            raise ValueError(f"expected 512-byte CGRAM, got {len(self.cgram)} bytes")
        self.obj_base1 = obj_base1
        self.obj_base2 = obj_base2

    def vram_word(self, address: int) -> int:
        offset = (address & 0x7FFF) * 2
        return self.vram[offset] | self.vram[offset + 1] << 8

    def color(self, index: int) -> tuple[int, int, int, int]:
        offset = (index & 0xFF) * 2
        color = (self.cgram[offset] | self.cgram[offset + 1] << 8) & 0x7FFF
        expand = lambda value: ((value & 31) << 3) | ((value & 31) >> 2)
        return expand(color), expand(color >> 5), expand(color >> 10), 255

    def pixel(self, word_address: int, x: int, y: int) -> int:
        shift = 7 - (x & 7)
        value = 0
        for pair in range(2):
            planes = self.vram_word(word_address + pair * 8 + (y & 7))
            value |= ((planes >> shift) & 1) << (pair * 2)
            value |= ((planes >> (shift + 8)) & 1) << (pair * 2 + 1)
        return value


def render_composition(snapshot: Snapshot, item: dict):
    try:
        from PIL import Image
    except ImportError as error:
        raise SystemExit(
            "rendering requires Pillow; use the bundled workspace Python runtime") from error

    left, top, right, bottom = item["bounds"]
    width = max(1, right - left)
    height = max(1, bottom - top)
    result = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    pixels = result.load()
    for part in item["parts"]:
        size = part["size"]
        attr = part["attr"]
        palette = (attr >> 9) & 7
        base = snapshot.obj_base2 if attr & 0x100 else snapshot.obj_base1
        for py in range(size):
            for px in range(size):
                used_x = size - 1 - px if attr & 0x4000 else px
                used_y = size - 1 - py if attr & 0x8000 else py
                tile = (((((attr & 0xFF) >> 4) + (used_y >> 3)) << 4) |
                        (((attr & 15) + (used_x >> 3)) & 15)) & 0xFF
                pixel = snapshot.pixel(base + tile * 16,
                                       used_x & 7, used_y & 7)
                if not pixel:
                    continue
                x = part["x"] - left + px
                y = part["y"] - top + py
                if 0 <= x < width and 0 <= y < height:
                    pixels[x, y] = snapshot.color(0x80 + palette * 16 + pixel)
    return result


def write_contact_pages(snapshot: Snapshot, items: list[dict], output: Path,
                        prefix: str, labels, per_page: int = 64) -> list[Path]:
    from PIL import Image, ImageDraw, ImageFont

    output.mkdir(parents=True, exist_ok=True)
    font = ImageFont.load_default()
    columns = 8
    cell_w, cell_h = 144, 112
    paths = []
    for page_index in range((len(items) + per_page - 1) // per_page):
        page_items = items[page_index * per_page:(page_index + 1) * per_page]
        rows = (len(page_items) + columns - 1) // columns
        sheet = Image.new("RGBA", (columns * cell_w, rows * cell_h),
                          (28, 31, 36, 255))
        draw = ImageDraw.Draw(sheet)
        for cell, item in enumerate(page_items):
            cx = (cell % columns) * cell_w
            cy = (cell // columns) * cell_h
            for yy in range(cy + 19, cy + cell_h - 3, 8):
                for xx in range(cx + 3, cx + cell_w - 3, 8):
                    shade = 52 if ((xx // 8) + (yy // 8)) & 1 else 63
                    draw.rectangle((xx, yy, min(xx + 7, cx + cell_w - 4),
                                    min(yy + 7, cy + cell_h - 4)),
                                   fill=(shade, shade, shade, 255))
            sprite = render_composition(snapshot, item)
            if sprite.getbbox():
                scale = min(4, max(1, min((cell_w - 12) // sprite.width,
                                          (cell_h - 27) // sprite.height)))
                sprite = sprite.resize((sprite.width * scale, sprite.height * scale),
                                       Image.Resampling.NEAREST)
                sheet.alpha_composite(sprite, (cx + (cell_w - sprite.width) // 2,
                                                cy + 20 + (cell_h - 24 - sprite.height) // 2))
            else:
                draw.text((cx + 7, cy + 49), "(blank in snapshot)",
                          font=font, fill=(230, 170, 90, 255))
            draw.text((cx + 4, cy + 4), labels(item), font=font,
                      fill=(240, 243, 247, 255))
            draw.rectangle((cx, cy, cx + cell_w - 1, cy + cell_h - 1),
                           outline=(92, 99, 110, 255))
        path = output / f"{prefix}_{page_index + 1:02d}.png"
        sheet.convert("RGB").save(path)
        paths.append(path)
    return paths


def render_catalog(catalog: dict, prefix: Path, output: Path,
                   obj_base1: int, obj_base2: int) -> list[Path]:
    snapshot = Snapshot(prefix, obj_base1, obj_base2)
    paths = []
    paths += write_contact_pages(
        snapshot, catalog["world_visuals"], output, "world_visual_ids",
        lambda item: f"G{item['id']:02X}  ${item['address']:04X}  "
                     f"{item['part_count']}p")
    paths += write_contact_pages(
        snapshot, catalog["spawn_visuals"], output, "spawn_compositions",
        lambda item: f"${item['address']:04X}  {item['part_count']}p")

    # A compact human-review sheet: appearances that are plausibly actors or
    # map-bound objects but whose semantic role is not proven by table shape.
    # Obvious menu icons, cursors, clouds, fire, and known enemy bodies stay on
    # the exhaustive sheets instead of crowding this classification pass.
    review = []
    for identity in range(0x2F, 0x34):
        item = dict(catalog["world_visuals"][identity])
        item["review_label"] = f"G{identity:02X} people group {identity - 0x2E}"
        review.append(item)
    review_addresses = {
        0xE940, 0xE94B, 0xE956, 0xE961,
        0xE96C, 0xE972, 0xE978, 0xE97E,
        0xE984, 0xE98A, 0xE990, 0xE996,
    }
    for source in catalog["spawn_visuals"]:
        if source["address"] not in review_addresses:
            continue
        item = dict(source)
        name = item["semantic"]["name"]
        name = name.replace(" facing right, frame ", " R")
        name = name.replace(" facing left, frame ", " L")
        item["review_label"] = f"${source['address']:04X} {name}"
        review.append(item)
    paths += write_contact_pages(
        snapshot, review, output, "classification_review",
        lambda item: item["review_label"])
    return paths


def parse_simcat_logs(paths: list[Path]) -> list[dict]:
    observations = []
    for path in paths:
        for line in path.read_text(errors="replace").splitlines():
            if "[simcat]" not in line:
                continue
            fields = {}
            for key, value in re.findall(r"([a-zA-Z0-9_]+)=([^ ]+)", line):
                if key == "tier":
                    fields[key] = value
                elif key in ("gf", "town", "idx"):
                    fields[key] = int(value, 10)
                else:
                    fields[key] = int(value, 16)
            fields["source"] = str(path)
            observations.append(fields)
    return observations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    subparsers = parser.add_subparsers(dest="command", required=True)

    crawl_parser = subparsers.add_parser("crawl", help="crawl ROM tables")
    crawl_parser.add_argument("--json", type=Path)

    render_parser = subparsers.add_parser("render", help="render contact sheets")
    render_parser.add_argument("--snapshot", type=Path, required=True,
                               help="prefix before .vram.bin/.cgram.bin")
    render_parser.add_argument("--out-dir", type=Path, required=True)
    render_parser.add_argument("--obj-base1", type=lambda value: int(value, 0),
                               default=0x2000)
    render_parser.add_argument("--obj-base2", type=lambda value: int(value, 0),
                               default=0x3000)

    trace_parser = subparsers.add_parser("trace", help="normalize AR_SIMCAT logs")
    trace_parser.add_argument("logs", nargs="+", type=Path)
    trace_parser.add_argument("--json", type=Path)

    args = parser.parse_args()
    if args.command == "trace":
        result = parse_simcat_logs(args.logs)
        text = json.dumps(result, indent=2) + "\n"
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(text)
        else:
            print(text, end="")
        return 0

    rom = Rom(args.rom)
    catalog = crawl(rom)
    if args.command == "crawl":
        text = json.dumps(hexify_catalog(catalog), indent=2) + "\n"
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(text)
            print(f"wrote {args.json}")
        else:
            print(text, end="")
        return 0

    paths = render_catalog(catalog, args.snapshot, args.out_dir,
                           args.obj_base1, args.obj_base2)
    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
