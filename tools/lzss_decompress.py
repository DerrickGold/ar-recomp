#!/usr/bin/env python3
"""Quintet LZSS decompressor for ActRaiser.

Quintet games (ActRaiser, Soul Blazer, Illusion of Gaia, Terranigma) use
a standard LZSS compression with a 256-byte sliding window.

Based on the RAM map analysis:
  - 7E00A5-7E00A7: Long pointer to compressed input byte
  - 7E00AE: Bit weight (0x80, 0x40... 0x01)
  - 7E00AF-7E00B0: Sliding window position
  - 7E00B1-7E00B2: Source position in sliding window
  - 7E00B3-7E00B4: Output size
  - 7E00B5-7E00B6: Output destination
  - 7E2000-7E20FF: 256-byte sliding window buffer

Usage:
  python3 tools/lzss_decompress.py <rom_path> <offset_hex> [max_output_size]
  python3 tools/lzss_decompress.py ar.sfc 0x2EE7F
"""
import sys
from pathlib import Path


def decompress_quintet_lzss(data, offset, max_output=0x10000):
    """Decompress Quintet LZSS data starting at the given ROM offset.

    Returns (decompressed_bytes, bytes_consumed_from_input).
    """
    window = bytearray(256)
    window_pos = 0
    output = bytearray()
    pos = offset

    while len(output) < max_output and pos < len(data):
        flags = data[pos]
        pos += 1

        for bit in range(8):
            if len(output) >= max_output or pos >= len(data):
                break

            if flags & (0x80 >> bit):
                # Literal byte
                byte = data[pos]
                pos += 1
                output.append(byte)
                window[window_pos] = byte
                window_pos = (window_pos + 1) & 0xFF
            else:
                # Back-reference (window_offset, length)
                if pos + 1 >= len(data):
                    break
                b1 = data[pos]
                b2 = data[pos + 1]
                pos += 2

                ref_offset = b1
                length = (b2 & 0x0F) + 3
                # Some variants use different length encoding
                # Try the most common Quintet format first

                for _ in range(length):
                    byte = window[ref_offset]
                    output.append(byte)
                    window[window_pos] = byte
                    window_pos = (window_pos + 1) & 0xFF
                    ref_offset = (ref_offset + 1) & 0xFF

    return bytes(output), pos - offset


def main():
    if len(sys.argv) < 3:
        print("Usage: python3 lzss_decompress.py <rom_path> <offset_hex> [max_output_size]")
        print("Example: python3 lzss_decompress.py ar.sfc 0x2EE7F")
        sys.exit(1)

    rom_path = Path(sys.argv[1])
    offset = int(sys.argv[2], 16)
    max_size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x10000

    rom = rom_path.read_bytes()
    print(f"ROM: {rom_path} ({len(rom)} bytes)")
    print(f"Decompressing from offset 0x{offset:06X}...")

    decompressed, consumed = decompress_quintet_lzss(rom, offset, max_size)

    print(f"Input bytes consumed: {consumed}")
    print(f"Output size: {len(decompressed)} bytes")
    print()

    # Show first 256 bytes as hex dump
    print("First 256 bytes of decompressed data:")
    for i in range(0, min(256, len(decompressed)), 16):
        hex_str = " ".join(f"{decompressed[i+j]:02X}" for j in range(min(16, len(decompressed) - i)))
        ascii_str = "".join(
            chr(decompressed[i+j]) if 32 <= decompressed[i+j] < 127 else "."
            for j in range(min(16, len(decompressed) - i))
        )
        print(f"  {i:04X}: {hex_str:<48s} {ascii_str}")

    # Optionally save to file
    out_path = Path(f"decompressed_0x{offset:06X}.bin")
    out_path.write_bytes(decompressed)
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
