#!/usr/bin/env python3
"""Render a numbered sheet of 16x16 icons from a VRAM+CGRAM snapshot.

Each icon is a 2x2 tile quad (the game stores the menu icons as 16x16 blocks in
a 16-tile-wide VRAM layout). Icons are laid out `per_row` across, scaled 3x,
each with a decimal number label so a human can pick "icon N".

Usage:
  python3 tools/icon_picker_sheet.py <vram.bin> <cgram.bin> <palette> \
      <base_tile_hex> <icon_count> out.png [per_row]
"""
import sys
import struct
import zlib

DIGITS = {
    '0': ["111", "101", "101", "101", "111"],
    '1': ["010", "110", "010", "010", "111"],
    '2': ["111", "001", "111", "100", "111"],
    '3': ["111", "001", "111", "001", "111"],
    '4': ["101", "101", "111", "001", "001"],
    '5': ["111", "100", "111", "001", "111"],
    '6': ["111", "100", "111", "101", "111"],
    '7': ["111", "001", "010", "010", "010"],
    '8': ["111", "101", "111", "101", "111"],
    '9': ["111", "101", "111", "001", "111"],
}


def palette(cgram, pal):
    out = []
    for i in range(16):
        c = cgram[(pal * 16 + i) * 2] | (cgram[(pal * 16 + i) * 2 + 1] << 8)
        out.append((((c & 0x1f) * 255 // 31),
                    (((c >> 5) & 0x1f) * 255 // 31),
                    (((c >> 10) & 0x1f) * 255 // 31)))
    return out


def tile_px(vram, tile):
    off = tile * 32
    px = [[0] * 8 for _ in range(8)]
    for y in range(8):
        p0, p1 = vram[off + y * 2], vram[off + y * 2 + 1]
        p2, p3 = vram[off + 16 + y * 2], vram[off + 16 + y * 2 + 1]
        for x in range(8):
            b = 7 - x
            px[y][x] = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | \
                (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3)
    return px


def icon_px(vram, base_tile, idx, per_grid=8):
    """16x16 index map for icon `idx`, laid out per_grid icons per VRAM row."""
    ic, ir = idx % per_grid, idx // per_grid
    tl = base_tile + (2 * ir) * 16 + (2 * ic)
    quads = {(0, 0): tl, (1, 0): tl + 1, (0, 1): tl + 16, (1, 1): tl + 17}
    out = [[0] * 16 for _ in range(16)]
    for (qx, qy), t in quads.items():
        tp = tile_px(vram, t)
        for y in range(8):
            for x in range(8):
                out[qy * 8 + y][qx * 8 + x] = tp[y][x]
    return out


def write_png(path, w, h, rgb):
    def chunk(tag, p):
        c = tag + p
        return struct.pack(">I", len(p)) + c + \
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


def put(rgb, W, x, y, c):
    if 0 <= x < W and y >= 0:
        o = (y * W + x) * 3
        rgb[o], rgb[o + 1], rgb[o + 2] = c


def label(rgb, W, x, y, text, scale=2):
    for ch in text:
        g = DIGITS.get(ch)
        if g:
            for ry, row in enumerate(g):
                for rx, on in enumerate(row):
                    if on == '1':
                        for sy in range(scale):
                            for sx in range(scale):
                                put(rgb, W, x + rx * scale + sx,
                                    y + ry * scale + sy, (255, 235, 120))
        x += 4 * scale


def main():
    vram = open(sys.argv[1], "rb").read()
    cgram = open(sys.argv[2], "rb").read()
    pal = int(sys.argv[3], 0)
    base = int(sys.argv[4], 0)
    count = int(sys.argv[5], 0)
    out = sys.argv[6]
    per_row = int(sys.argv[7]) if len(sys.argv) > 7 else 10

    colors = palette(cgram, pal)
    scale = 3
    icon_px_sz = 16 * scale
    pad, label_h = 6, 14
    cw = icon_px_sz + pad
    ch = icon_px_sz + label_h + pad
    rows = (count + per_row - 1) // per_row
    W, H = per_row * cw + pad, rows * ch + pad
    rgb = bytearray(W * H * 3)
    for i in range(0, len(rgb), 3):
        rgb[i], rgb[i + 1], rgb[i + 2] = 24, 26, 38

    for n in range(count):
        ipx = icon_px(vram, base, n)
        gx = pad + (n % per_row) * cw
        gy = pad + (n // per_row) * ch + label_h
        for y in range(16):
            for x in range(16):
                v = ipx[y][x]
                if v == 0:
                    continue
                r, g, b = colors[v]
                for sy in range(scale):
                    for sx in range(scale):
                        put(rgb, W, gx + x * scale + sx, gy + y * scale + sy,
                            (r, g, b))
        label(rgb, W, gx + 2, gy - label_h + 2, str(n))
    write_png(out, W, H, rgb)
    print(f"{out}: {count} icons from tile ${base:03X}, pal {pal}, {rows}x{per_row}")


if __name__ == "__main__":
    main()
