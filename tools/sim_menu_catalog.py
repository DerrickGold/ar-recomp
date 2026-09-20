#!/usr/bin/env python3
"""Collect the verified USA SIM menu's identities, icon parts, and text routes.

Read-only ROM research; no game patching and no retail text or pixel export.
See docs/sim-menu-reference.md for control flow and interpretation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from sim_object_catalog import Rom, composition, walk_spawn_script


ROOT = Path(__file__).resolve().parents[1]
ROUTES = ROOT / "tools/data/localization"
PROFILE = ROOT / "installer/internal/localization/data/catalog-profiles.json"

# Hand-reviewed entries of $01:81D7's DEC/branch dispatch, in native action order.
HANDLERS = (
    0x822E, 0x8239, 0x8244, 0x8284, 0x8290, 0x82FB, 0x8366, 0x8431,
    0x83D1, 0x8491, 0x84B2, 0x8530, 0x853B, 0x854A, 0x8559,
)
MIRACLES = (
    (5, "lightning", 1, 10, True),
    (6, "rain", 2, 20, True),
    (7, "sun", 3, 30, True),
    (8, "wind", 5, 80, False),
    (9, "earthquake", 4, 160, False),
)


def pc(address: int, bank: int = 1) -> str:
    return f"${bank:02X}:{address:04X}"


def load_routes(name: str) -> dict:
    return json.loads((ROUTES / f"us-runtime-{name}-routes-v1.json").read_text())


def icon(rom: Rom, family: int) -> dict:
    variants = rom.u16(0xA227 + family * 2)
    result = {"family": family, "variant_table": pc(variants), "variants": []}
    for variant in (0, 1):
        script = rom.u16(variants + variant * 2)
        frames = walk_spawn_script(rom, script)["frames"]
        if not frames:
            raise ValueError(f"menu icon family {family} has no drawable frame")
        result["variants"].append({
            "variant": variant,
            "role": "selected" if variant == 0 else "unselected",
            "script": pc(script),
            "frames": [{
                "duration": frame["duration"],
                "composition_pc24": pc(frame["composition"]),
                "composition": composition(rom, frame["composition"]),
            } for frame in frames],
        })
    return result


def collect(rom_path: Path) -> dict:
    rom = Rom(rom_path)
    # Match the headerless, verified USA image used by the runtime route census.
    if len(rom.data) % 0x8000 == 512:
        rom.data = rom.data[512:]
    digest = hashlib.sha256(rom.data).hexdigest()
    compose = load_routes("compose")
    dialogue = load_routes("dialogue")
    if digest != compose["rom_sha256"] or digest != dialogue["rom_sha256"]:
        raise ValueError("ROM does not match the verified USA runtime route census")
    # Prove the reviewed action-entry map against the native branch chain.
    for index, handler in enumerate(HANDLERS):
        at = 0x81D7 + index * 6
        if index < 14:
            if rom.bytes(at, 4) != b"\x3a\xd0\x03\x82":
                raise ValueError("unexpected action dispatch shape")
            branch = at + 3
        else:
            branch = at
            if rom.u8(branch) != 0x82:
                raise ValueError("unexpected final action branch")
        if (branch + 3 + rom.u16(branch + 1)) & 0xFFFF != handler:
            raise ValueError("reviewed action handler differs from ROM dispatch")
    profiles = json.loads(PROFILE.read_text())
    # Keep semantic identity owned by the existing localization catalogue.
    tables = profiles["composer_pointer_routes"]
    by_id = {route["semantic_id"]: route for route in compose["routes"]}
    categories, actions = [], []
    cursor, record_index = 0xF32E, 0
    init = rom.u16(0xAB20)
    for group, semantic_id in enumerate(tables["sim_choice"]):
        entries = []
        while rom.u8(cursor) != 0xFF:
            value = rom.u8(cursor)
            if value >> 4 != group:
                raise ValueError("unexpected category encoding")
            action_id = value & 0x0F
            entry_id = tables["sim_root"][action_id - 1] if action_id else semantic_id
            label_pointer = 0xF34C + (action_id - 1) * 2 if action_id else 0xF36A + group * 2
            if by_id[entry_id]["source_pc24"] != pc(rom.u16(label_pointer)):
                raise ValueError("menu label pointer mismatch")
            at = init + record_index * 6
            family = rom.u16(at + 4) & 0xFF
            entry = {
                "semantic_id": entry_id,
                "selection_pointer": pc(cursor),
                "selection_byte": value,
                "fixed_record_wram": f"$7E:{0x06A0 + record_index * 0x12:04X}",
                "native_anchor": [rom.u16(at), rom.u16(at + 2)],
                "icon": icon(rom, family),
                "label_route": by_id[entry_id],
            }
            if action_id:
                entry["native_action_id"] = action_id
                entry["handler_pc24"] = pc(HANDLERS[action_id - 1])
                actions.append(entry)
                entries.append(entry_id)
            else:
                entry["actions"] = entries
                categories.append(entry)
            cursor += 1
            record_index += 1
        cursor += 1
    if (len(categories), len(actions), rom.u8(cursor)) != (6, 15, 0xFF):
        raise ValueError("unexpected menu shape")
    if [row["native_action_id"] for row in actions] != list(range(1, 16)):
        raise ValueError("unexpected action dispatch order")
    for action_id, name, kind, cost, targeted in MIRACLES:
        row = actions[action_id - 1]
        row["miracle"] = {"kind": kind, "sp_cost": cost, "target_picker": targeted}
        row["dialogue_routes"] = [route for route in dialogue["routes"]
                                  if route["semantic_id"].startswith(f"sim.miracle.{name}.")]
        # Verify the reviewed SP cost against the immediate after the SP read.
        body = rom.bytes(HANDLERS[action_id - 1], 32)
        if b"\xaf\x82\x02\x00\xc9" + cost.to_bytes(2, "little") not in body:
            raise ValueError(f"{name}: SP check does not match the reviewed code")
    items = []
    for slot, semantic_id in enumerate(tables["selected_possession"]):
        record = rom.u16(0xF08E + slot * 2)
        route = by_id[semantic_id]
        if route["source_pc24"] != pc(record + 1):
            raise ValueError("possession label pointer mismatch")
        items.append({
            "item_id": slot + 1,
            "semantic_id": semantic_id,
            "label_route": route,
            "icon": icon(rom, rom.u8(record)),
            "use_handler_pc24": pc(rom.u16(0x9C94 + slot * 2) + 1),
            "receipt_dialogue": [r for r in dialogue["routes"]
                                 if r["semantic_id"] == f"dialogue.offering.slot_{slot:02d}"],
        })
    return {
        "schema": "actraiser-sim-menu-research-v1",
        "rom_sha256": digest,
        "evidence": "USA ROM tables plus reviewed control flow; not live UI qualification",
        "notes": [
            "SNES program addresses are release-specific, not portable action IDs.",
            "Icon parts need scene-correct OBJ VRAM/CGRAM to become pixels.",
            "Icon palette variants describe selection, not action availability.",
            "Text route caller_pc24 is the continuation after JSR, not the call opcode.",
            "Receipt dialogue is post-transfer acknowledgement, not guaranteed standalone help.",
        ],
        "tables": {"selection": pc(0xF32E), "label_descriptor": pc(0xF34A),
                   "action_labels": pc(0xF34C), "category_labels": pc(0xF36A),
                   "icon_initialization": pc(init), "icon_families": pc(0xA227),
                   "possessions": pc(0xF08E), "item_use_dispatch": pc(0x9C94),
                   "item_receipt_dialogue": pc(0xC6AE, 4)},
        "categories": categories,
        "actions": actions,
        "items": items,
        "related_dialogue_routes": [r for r in dialogue["routes"]
                                    if r["semantic_id"].startswith(("sim.", "system.save.",
                                       "system.message_speed.", "dialogue.offering."))],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--out", type=Path, help="optional JSON output; otherwise stdout")
    args = parser.parse_args()
    try:
        output = json.dumps(collect(args.rom), indent=2) + "\n"
    except (ValueError, OSError) as error:
        parser.exit(1, f"sim-menu-catalog: {error}\n")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(output)
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
