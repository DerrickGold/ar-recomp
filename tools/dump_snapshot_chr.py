#!/usr/bin/env python3
"""Render a VRAM snapshot's 4bpp tiles into a color contact sheet.

Feeds off an AR_VRAMDUMP snapshot (snap_*.vram.bin + snap_*.cgram.bin) so the
menu icons come out with their real in-game colors, which a static ROM dump
cannot give (the catalog tool notes character pixels + colors need a runtime
snapshot). Renders every tile with one CGRAM palette; sweep palettes to find
the one an icon uses.

Usage:
  python3 tools/dump_snapshot_chr.py <vram.bin> <cgram.bin> <palette> out.png [cols] [first] [count]
"""
import sys
import struct
import zlib


def cgram_palette(cgram, pal):
    colors = []
    for i in range(16):
        lo = cgram[(pal * 16 + i) * 2]
        hi = cgram[(pal * 16 + i) * 2 + 1]
        c = lo | (hi << 8)
        r = (c & 0x1f) * 255 // 31
        g = ((c >> 5) & 0x1f) * 255 // 31
        b = ((c >> 10) & 0x1f) * 255 // 31
        colors.append((r, g, b))
    return colors


def decode_tile(vram, off):
    px = [[0] * 8 for _ in range(8)]
    for y in range(8):
        p0, p1 = vram[off + y * 2], vram[off + y * 2 + 1]
        p2, p3 = vram[off + 16 + y * 2], vram[off + 16 + y * 2 + 1]
        for x in range(8):
            b = 7 - x
            px[y][x] = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | \
                (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3)
    return px


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + \
            struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y * w * 3:(y + 1) * w * 3])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    vram = open(sys.argv[1], "rb").read()
    cgram = open(sys.argv[2], "rb").read()
    pal = int(sys.argv[3], 0)
    out = sys.argv[4]
    cols = int(sys.argv[5]) if len(sys.argv) > 5 else 16
    first = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0
    count = int(sys.argv[7], 0) if len(sys.argv) > 7 else (len(vram) // 32) - first

    colors = cgram_palette(cgram, pal)
    rows = (count + cols - 1) // cols
    cell = 9
    W, H = cols * cell + 1, rows * cell + 1
    rgb = bytearray(W * H * 3)
    for i in range(0, len(rgb), 3):
        rgb[i], rgb[i + 1], rgb[i + 2] = 20, 22, 34  # index-0 backdrop
    for n in range(count):
        t = first + n
        if (t + 1) * 32 > len(vram):
            break
        px = decode_tile(vram, t * 32)
        cx, cy = (n % cols) * cell + 1, (n // cols) * cell + 1
        for y in range(8):
            for x in range(8):
                v = px[y][x]
                if v == 0:
                    continue  # transparent -> show backdrop
                r, g, b = colors[v]
                o = ((cy + y) * W + (cx + x)) * 3
                rgb[o], rgb[o + 1], rgb[o + 2] = r, g, b
    write_png(out, W, H, rgb)
    print(f"{out}: pal {pal}, tiles {first}..{first+count}, {rows}x{cols}")


if __name__ == "__main__":
    main()
