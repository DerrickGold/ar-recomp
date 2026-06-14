#!/usr/bin/env python3
"""ActRaiser ROM analysis tool.

Parses the ROM header, identifies data regions from known ROM map offsets,
and performs basic code/data heuristic analysis.

Usage: python3 tools/rom_info.py [rom_path]
"""
import struct
import sys
from pathlib import Path

ROM_PATH = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("ar.sfc")

EXPECTED_CHECKSUM = 0x83DB
EXPECTED_TITLE = "ACTRAISER-USA"

KNOWN_REGIONS = [
    (0x000000, 0x011ACC, "Code (bank 00-02 main program)"),
    (0x011ACD, 0x012621, "SPC700 audio driver program"),
    (0x012622, 0x01B40D, "Code (continued)"),
    (0x01B40E, 0x01B431, "Experience level requirements"),
    (0x01B432, 0x01B455, "Experience level max SP values"),
    (0x01B825, 0x01B8FC, "Lair enemy data"),
    (0x01DCFA, 0x01DFF9, "Town building/obstacle data (all 6 towns)"),
    (0x020000, 0x02000D, "Town name pointers"),
    (0x02000E, 0x020042, "Town names"),
    (0x020043, 0x02004A, "Enemy name pointers"),
    (0x02004B, 0x020076, "Enemy names"),
    (0x020077, 0x021396, "Angel dialogue"),
    (0x021523, 0x0246AD, "Town dialogue"),
    (0x0246AE, 0x024C99, "Offering descriptions"),
    (0x024C8A, 0x0258F2, "Ending sequence text"),
    (0x0258F3, 0x025EF2, "Text compression dictionary"),
    (0x028000, 0x028E3F, "Map metadata"),
    (0x02CE7F, 0x02EE7E, "Uncompressed graphics"),
    (0x02EE7F, 0x02FF7F, "Compressed graphics"),
    (0x02FF80, 0x02FFFF, "Map palettes"),
    (0x040000, 0x04FD2D, "Audio samples (BRR, 32 samples)"),
    (0x050000, 0x052FFF, "Town maps (base + obstacle layers)"),
    (0x060000, 0x06FFFF, "Uncompressed graphics (large block)"),
]


def read_rom(path):
    data = path.read_bytes()
    if len(data) % 0x8000 == 0x200:
        print(f"Stripping 512-byte copier header")
        data = data[0x200:]
    return data


def parse_header(rom):
    off = 0x7FC0
    title = rom[off:off+21].decode("ascii", errors="replace").strip()
    map_mode = rom[0x7FD5]
    rom_type = rom[0x7FD6]
    rom_size = rom[0x7FD7]
    sram_size = rom[0x7FD8]
    country = rom[0x7FD9]
    version = rom[0x7FDB]
    chk_comp = struct.unpack("<H", rom[0x7FDC:0x7FDE])[0]
    checksum = struct.unpack("<H", rom[0x7FDE:0x7FE0])[0]

    mapping_names = {0x20: "LoROM", 0x21: "HiROM", 0x30: "LoROM+FastROM", 0x31: "HiROM+FastROM"}
    chipsets = {0x00: "ROM only", 0x01: "ROM+RAM", 0x02: "ROM+RAM+Battery"}
    countries = {0x00: "Japan", 0x01: "USA/Canada", 0x02: "Europe"}

    print("=" * 50)
    print("ActRaiser ROM Header Analysis")
    print("=" * 50)
    print(f"Title:      {title}")
    print(f"Mapping:    {mapping_names.get(map_mode, f'Unknown (0x{map_mode:02X})')}")
    print(f"Chipset:    {chipsets.get(rom_type, f'Unknown (0x{rom_type:02X})')}")
    print(f"ROM size:   {1 << rom_size} KB")
    print(f"SRAM size:  {(1 << sram_size) if sram_size else 0} KB")
    print(f"Country:    {countries.get(country, f'Unknown (0x{country:02X})')}")
    print(f"Version:    {version}")
    print(f"Checksum:   0x{checksum:04X} (complement: 0x{chk_comp:04X})")

    valid = (chk_comp ^ checksum) == 0xFFFF
    computed = sum(rom) & 0xFFFF
    print(f"Valid:      {valid} (computed: 0x{computed:04X}, match: {computed == checksum})")

    if title != EXPECTED_TITLE:
        print(f"\nWARNING: Expected title '{EXPECTED_TITLE}', got '{title}'")
    if checksum != EXPECTED_CHECKSUM:
        print(f"WARNING: Expected checksum 0x{EXPECTED_CHECKSUM:04X}, got 0x{checksum:04X}")

    print()
    print("Interrupt Vectors:")
    vectors = [
        ("Native COP",   0x7FE4), ("Native BRK",   0x7FE6),
        ("Native ABORT", 0x7FE8), ("Native NMI",   0x7FEA),
        ("Native RESET", 0x7FEC), ("Native IRQ",   0x7FEE),
        ("Emu COP",      0x7FF4), ("Emu ABORT",    0x7FF8),
        ("Emu NMI",      0x7FFA), ("Emu RESET",    0x7FFC),
        ("Emu IRQ",      0x7FFE),
    ]
    for name, addr in vectors:
        vec = struct.unpack("<H", rom[addr:addr+2])[0]
        if vec != 0:
            print(f"  {name:14s}: $00:{vec:04X}")

    return checksum == EXPECTED_CHECKSUM


def analyze_regions(rom):
    print()
    print("=" * 50)
    print("Known ROM Regions")
    print("=" * 50)
    total_mapped = 0
    for start, end, desc in KNOWN_REGIONS:
        size = end - start + 1
        total_mapped += size
        print(f"  0x{start:06X}-0x{end:06X} ({size:6d} bytes): {desc}")

    unmapped = len(rom) - total_mapped
    print(f"\n  Mapped:   {total_mapped:7d} bytes ({total_mapped*100/len(rom):.1f}%)")
    print(f"  Unmapped: {unmapped:7d} bytes ({unmapped*100/len(rom):.1f}%)")


def analyze_banks(rom):
    print()
    print("=" * 50)
    print("Bank Analysis (LoROM, 32KB per bank)")
    print("=" * 50)
    bank_size = 0x8000
    num_banks = len(rom) // bank_size

    for bank in range(num_banks):
        offset = bank * bank_size
        chunk = rom[offset:offset+bank_size]

        non_zero = sum(1 for b in chunk if b not in (0x00, 0xFF))
        pct = non_zero / len(chunk) * 100

        jsl_count = 0
        jsr_count = 0
        rts_count = 0
        rtl_count = 0
        for i in range(len(chunk) - 2):
            if chunk[i] == 0x22:
                jsl_count += 1
            elif chunk[i] == 0x20:
                jsr_count += 1
            elif chunk[i] == 0x60:
                rts_count += 1
            elif chunk[i] == 0x6B:
                rtl_count += 1

        if jsl_count + jsr_count + rts_count + rtl_count > 50:
            likely = "CODE"
        elif pct < 5:
            likely = "EMPTY"
        else:
            likely = "DATA"

        snes_bank = bank
        snes_addr = f"${snes_bank:02X}:8000-${snes_bank:02X}:FFFF"
        print(f"  Bank {bank:02X} ({snes_addr}): {pct:5.1f}% used | "
              f"JSL:{jsl_count:3d} JSR:{jsr_count:3d} RTS:{rts_count:3d} RTL:{rtl_count:3d} | {likely}")


def find_jsl_targets(rom):
    """Find all JSL (long call) targets to identify cross-bank function entry points."""
    print()
    print("=" * 50)
    print("JSL Targets (cross-bank function calls)")
    print("=" * 50)
    targets = {}
    bank_size = 0x8000

    for i in range(len(rom) - 3):
        if rom[i] == 0x22:  # JSL opcode
            addr_lo = rom[i+1]
            addr_hi = rom[i+2]
            bank = rom[i+3]
            target = (bank << 16) | (addr_hi << 8) | addr_lo

            src_bank = i // bank_size
            src_offset = (i % bank_size) + 0x8000
            src_addr = (src_bank << 16) | src_offset

            if target not in targets:
                targets[target] = []
            targets[target].append(src_addr)

    sorted_targets = sorted(targets.items())
    print(f"  Found {len(sorted_targets)} unique JSL targets")
    print()

    by_bank = {}
    for target, callers in sorted_targets:
        bank = target >> 16
        if bank not in by_bank:
            by_bank[bank] = []
        by_bank[bank].append((target, len(callers)))

    for bank in sorted(by_bank.keys()):
        entries = by_bank[bank]
        print(f"  Bank ${bank:02X}: {len(entries)} entry points")
        for target, count in entries[:10]:
            print(f"    ${target:06X} (called {count}x)")
        if len(entries) > 10:
            print(f"    ... and {len(entries)-10} more")
        print()


def main():
    if not ROM_PATH.exists():
        print(f"Error: ROM not found at {ROM_PATH}")
        sys.exit(1)

    rom = read_rom(ROM_PATH)
    print(f"ROM file: {ROM_PATH} ({len(rom)} bytes)")
    print()

    parse_header(rom)
    analyze_regions(rom)
    analyze_banks(rom)
    find_jsl_targets(rom)


if __name__ == "__main__":
    main()
