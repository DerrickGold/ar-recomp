#!/usr/bin/env python3
"""Dump a range of ROM as 4bpp SNES CHR tiles into a PNG contact sheet.

Used to LOCATE candidate UI icons in the ROM (the Sky Palace status/menu
graphics) so they can be picked and wired into the settings overlay. Tiles are
8x8, 32 bytes each, arranged `cols` per row. Pixel value 0..15 is shown as a
grayscale ramp so shapes are visible regardless of the real palette; a 16x16
icon appears as a 2x2 block of tiles.

Usage:
  python3 tools/dump_chr.py ar.sfc 0x68000 0x8000 out.png [cols]
"""
import sys
import struct
import zlib


def decode_4bpp_tile(data, off):
    """Return an 8x8 list-of-rows of pixel values (0..15)."""
    px = [[0] * 8 for _ in range(8)]
    for y in range(8):
        p0 = data[off + y * 2]
        p1 = data[off + y * 2 + 1]
        p2 = data[off + 16 + y * 2]
        p3 = data[off + 16 + y * 2 + 1]
        for x in range(8):
            bit = 7 - x
            v = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | \
                (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3)
            px[y][x] = v
    return px


def write_png(path, width, height, rgb):
    """Minimal RGB PNG writer (no external deps)."""
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + \
            struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * width * 3:(y + 1) * width * 3])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    rom = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 0)
    length = int(sys.argv[3], 0)
    out = sys.argv[4]
    cols = int(sys.argv[5]) if len(sys.argv) > 5 else 16

    tiles = length // 32
    rows = (tiles + cols - 1) // cols
    # 1px gap between tiles so icon boundaries are readable.
    cell = 9
    W = cols * cell + 1
    H = rows * cell + 1
    rgb = bytearray(W * H * 3)
    # faint blue grid background so gaps read like the game's slot frames.
    for i in range(0, len(rgb), 3):
        rgb[i], rgb[i + 1], rgb[i + 2] = 16, 18, 28

    for t in range(tiles):
        px = decode_4bpp_tile(rom, start + t * 32)
        cx = (t % cols) * cell + 1
        cy = (t // cols) * cell + 1
        for y in range(8):
            for x in range(8):
                v = px[y][x]
                g = v * 17
                o = ((cy + y) * W + (cx + x)) * 3
                rgb[o], rgb[o + 1], rgb[o + 2] = g, g, g
    write_png(out, W, H, rgb)
    print(f"{out}: {tiles} tiles, {rows}x{cols} grid, ROM ${start:06X}-${start+length:06X}")


if __name__ == "__main__":
    main()
