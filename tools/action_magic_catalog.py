#!/usr/bin/env python3
"""Decode the action-mode magic animation tables used by $00:9F25-$A034.

The action animation interpreter at $00:8E2F treats an animation bank as:

  word composition_pointer_table_offset
  word state_0_sequence_offset
  word state_1_sequence_offset
  ...

Each sequence is four-byte ``visual, delay, dx, dy`` entries terminated by a
visual byte of $FF.  A visual indexes a relative pointer table whose leaves are
the seven-byte action OAM compositions consumed by $00:8D68.  Each part holds
normal/flipped coordinate bytes; the active culling extent converts the selected
unsigned coordinate into a signed offset from the object's world-space hot point.

This tool intentionally reports geometry/timing summaries and exact parts rather
than rendering pixels; it is useful when attaching host effects to the authentic
object lifecycle.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ar_lib import lorom_off, read_le16


SPELLS = {
    "magical_fire": {"base": 0x07C000, "states": [2, 3]},
    "magical_stardust": {"base": 0x07C000, "states": [0, 1]},
    "magical_aura": {"base": 0x07C800, "states": [3]},
    "magical_light_center": {"base": 0x07C800, "states": [1]},
    "magical_light_sides": {"base": 0x07C800, "states": [0]},
}


def s8(value: int) -> int:
    return value - 0x100 if value & 0x80 else value


def decode_composition(data: bytes, base_file: int, relative: int) -> dict:
    start = base_file + relative
    left, right, top, bottom, count = data[start : start + 5]
    parts = []
    cursor = start + 5
    for _ in range(count):
        size, x, x_flipped, y, y_flipped, tile, attributes = data[cursor : cursor + 7]
        parts.append(
            {
                "size": "16x16" if size else "8x8",
                # The emitter chooses one byte from each coordinate pair based
                # on the object's H/V-flip bits.  Values are unsigned offsets
                # from the composition's top-left; subtracting the selected
                # culling extent converts them to object-anchor coordinates.
                "x": x,
                "x_flipped": x_flipped,
                "y": y,
                "y_flipped": y_flipped,
                "tile": tile,
                "attributes": attributes,
            }
        )
        cursor += 7
    geometry_bounds = None
    if parts:
        geometry_bounds = {
            "left": min(part["x"] - left for part in parts),
            "right": max(
                part["x"] - left + (16 if part["size"] == "16x16" else 8)
                for part in parts
            ),
            "top": min(part["y"] - top for part in parts),
            "bottom": max(
                part["y"] - top + (16 if part["size"] == "16x16" else 8)
                for part in parts
            ),
        }
    return {
        "relative": relative,
        # These four bytes are consumed as unsigned culling extents by $00:8E2F.
        # They are not always a useful geometric bounding box (Magical Light's
        # pre-beam visual deliberately contains a wrapped value), so also report
        # bounds calculated from the authored OAM parts.
        "cull_extents_raw": {
            "left": left,
            "right": right,
            "top": top,
            "bottom": bottom,
        },
        "normal_anchor_geometry": geometry_bounds,
        "parts": parts,
    }


def decode_state(data: bytes, base: int, state: int) -> dict:
    base_file = lorom_off(base)
    sequence_relative = read_le16(data, base_file + (state + 1) * 2)
    composition_table_relative = read_le16(data, base_file)
    cursor = base_file + sequence_relative
    steps = []
    visuals = {}
    while data[cursor] != 0xFF:
        visual, delay, dx, dy = data[cursor : cursor + 4]
        steps.append(
            {
                "visual": visual,
                "delay": delay,
                "dx": s8(dx),
                "dy": s8(dy),
            }
        )
        if visual not in visuals:
            pointer = u16(
                data,
                base_file + composition_table_relative + visual * 2,
            )
            visuals[visual] = decode_composition(data, base_file, pointer)
        cursor += 4
    visual_list = list(visuals.values())
    geometry_bounds = {
        "left": min(visual["normal_anchor_geometry"]["left"] for visual in visual_list),
        "right": max(visual["normal_anchor_geometry"]["right"] for visual in visual_list),
        "top": min(visual["normal_anchor_geometry"]["top"] for visual in visual_list),
        "bottom": max(visual["normal_anchor_geometry"]["bottom"] for visual in visual_list),
    }
    return {
        "state": state,
        "sequence_relative": sequence_relative,
        "summary": {
            # The interpreter displays the selected entry for delay+1 logic ticks.
            "nominal_ticks": sum(step["delay"] + 1 for step in steps),
            "frame_entries": len(steps),
            "net_velocity_delta": {
                "x": sum(step["dx"] for step in steps),
                "y": sum(step["dy"] for step in steps),
            },
            "visual_ids": sorted(visuals),
            "max_parts": max(len(visual["parts"]) for visual in visual_list),
            "geometry_union": geometry_bounds,
        },
        "steps": steps,
        "visuals": visuals,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", type=Path, default=Path("ar.sfc"))
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    data = args.rom.read_bytes()
    result = {}
    for name, spec in SPELLS.items():
        result[name] = {
            "animation_base": spec["base"],
            "states": [decode_state(data, spec["base"], state) for state in spec["states"]],
        }
    print(json.dumps(result, indent=None if args.compact else 2, sort_keys=True))


if __name__ == "__main__":
    main()
