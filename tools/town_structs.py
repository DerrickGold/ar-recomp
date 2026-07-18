#!/usr/bin/env python3
"""town_structs.py — decode a town's 128-slot structure-record array from a
WRAM dump (saves/dump_wram.bin or dump_f<N>_wram.bin: raw 128 KiB g_ram,
$7E at 0x00000, $7F at 0x10000).

Record format (SEAMS.md town §7): base $7F:6BE7 + town*0x200, 128 x 4 bytes
{cell X, cell Y, flags/type, action|progress}. Flags: bit7 active, bit6 under
construction, bits 4-5 subtype, low nibble type class (0 house, 1 bridge,
2 field, 3/4 factory tier).

Usage:
  tools/town_structs.py saves/dump_wram.bin            # current town ($7F:7BF9)
  tools/town_structs.py saves/dump_wram.bin --town 0   # explicit town
  tools/town_structs.py saves/dump_wram.bin --all      # all six towns, summary
"""
import argparse
import sys

TOWNS = ["Fillmore", "Bloodpool", "Kasandora", "Aitos", "Marahna", "Northwall"]
CLASSES = {0: "house", 1: "BRIDGE", 2: "field", 3: "factory3", 4: "factory4",
           5: "class5", 6: "class6"}
WRAM_7F = 0x10000
RECORDS_BASE = 0x6BE7
RECORD_COUNT = 128


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x20000:
        sys.exit(f"{path}: expected 128 KiB WRAM dump, got {len(data)} bytes")
    return data


def records(data, town):
    base = WRAM_7F + RECORDS_BASE + town * 0x200
    for i in range(RECORD_COUNT):
        x, y, flags, action = data[base + i * 4: base + i * 4 + 4]
        yield i, x, y, flags, action


def summarize(data, town, verbose):
    active = 0
    by_class = {}
    print(f"== {TOWNS[town]} (town {town}, base $7F:{RECORDS_BASE + town * 0x200:04X}) ==")
    for i, x, y, flags, action in records(data, town):
        if not flags & 0x80:
            continue
        active += 1
        cls = flags & 0x0F
        by_class[cls] = by_class.get(cls, 0) + 1
        if verbose:
            name = CLASSES.get(cls, f"class{cls}")
            building = " building" if flags & 0x40 else ""
            sub = (flags >> 4) & 0x3
            act = f" action={action & 0x0F}" if action & 0x0F else ""
            prog = f" prog={(action >> 4) & 0x7}" if action & 0x70 else ""
            print(f"  [{i:3d}] cell {x:2d},{y:2d}  {name:8s} sub={sub}"
                  f"{building}{act}{prog}  (flags=${flags:02X})")
    parts = ", ".join(f"{CLASSES.get(c, c)}={n}" for c, n in sorted(by_class.items()))
    print(f"  {active}/128 slots active  ({parts})")
    if active >= RECORD_COUNT:
        print("  ** TABLE FULL — allocator at cap **")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--town", type=int, choices=range(6))
    ap.add_argument("--all", action="store_true")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="summary lines only, no per-record detail")
    args = ap.parse_args()

    data = load(args.dump)
    if args.all:
        for t in range(6):
            summarize(data, t, verbose=not args.quiet)
        return
    town = args.town
    if town is None:
        town = data[WRAM_7F + 0x7BF9]
        if town > 5:
            sys.exit(f"current-town byte $7F:7BF9 = {town} (not in a town?); "
                     "pass --town or --all")
        print(f"(current town from $7F:7BF9 = {town})")
    summarize(data, town, verbose=not args.quiet)


if __name__ == "__main__":
    main()
