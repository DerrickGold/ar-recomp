#!/usr/bin/env python3
"""act_collision.py — action-stage terrain collision map, from ROM or a dump.

The action collision oracle is `$00:91C3`: given a 16px tile coordinate in
`$14`/`$16` it returns a 4-bit quadrant-solidity attribute. Two structures feed
it, both built at level entry from LZSS-compressed ROM blobs:

  $7E:8000  metatile-id map, one byte per 16px tile, stored in 16x16-tile
            chunks: index = (chunkRow*$2F + chunkCol)*256 + (ty&15)*16 + (tx&15)
  $7E:05A0  metatile id -> attribute, 256 bytes, built by `$02:BAC1` from bit 1
            of each of the metatile's four sub-tile tilemap words

Attribute = quadrant solidity mask, bit0 TL, bit1 TR, bit2 BL, bit3 BR:
  $00        empty
  $0F        fully solid (walls / ceiling; the `CMP #$0F` the probes test)
  bits 0..1  top half   -- `AND #$0003 == $0003` is the "stand on this" test
  bits 2..3  bottom half -- `AND #$000C` is the underside/step test
  $06, $09   the two DIAGONAL patterns, special-cased as SLOPES by `$00:8FE7`
             ($06 -> `$90A9` uses x&$0E, $09 -> `$9084` uses ~x&$0E)

Two sources, same answers:
  --map MODE,SUB   build from ROM alone by following the per-map asset script
                   ($05:8000) to the map + metatile blobs and decompressing them
                   with tools/quintet_lzss.py. Works for every action map.
  --wram <dump>    read the already-built structures out of a WRAM dump.
Verified equal: for $18=$01/$19=$01 the two paths agree on the attribute table
and on all 12288 tile attributes.

NOTE on --check: a placement's tileY is a GROUND LINE (`$00:96A4` subtracts the
object's down-extent at spawn), and the assumed 2-tile body height is a
heuristic — the real height is the animation frame's `$10`, which lives in the
per-act blob. Treat "airborne" and "blocked" as review flags, not errors.

See docs/SEAMS.md "Content / randomizer seams" §5b.

Examples:
  tools/act_collision.py --map 1,1 --check
  tools/act_collision.py --map 6,3 --png /tmp/northwall3.png
  tools/act_collision.py --wram saves/dump_wram.bin --probe 83,41
"""
import argparse
import collections
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ar_lib import load_rom

# Attribute values the collision module special-cases.
EMPTY = 0x00
SOLID = 0x0F
SLOPE_A = 0x06          # TR+BL diagonal
SLOPE_B = 0x09          # TL+BR diagonal
TOP_HALF = 0x03         # the platform / standable-surface mask


def classify(a):
    if a == EMPTY:
        return 'empty'
    if a == SOLID:
        return 'solid'
    if a == SLOPE_A:
        return 'slope(/)'
    if a == SLOPE_B:
        return 'slope(\\)'
    parts = []
    if a & 0x03 == 0x03:
        parts.append('top-half')
    if a & 0x0C:
        parts.append('bottom-half')
    return '+'.join(parts) or f'partial(${a:X})'


class Stage:
    """Collision view over one WRAM dump."""

    def __init__(self, path):
        self.d = open(path, 'rb').read()
        if len(self.d) < 0x10000:
            raise SystemExit(f"{path}: too short for a WRAM dump")
        self.mode = self.u8(0x18)
        self.sub = self.u8(0x19)
        self.w = self.u16(0x84)          # width in 16px tiles
        self.h = self.u16(0x86)          # height in 16px tiles
        self.chunk_cols = self.u8(0x2F)  # = (level width px) >> 8
        self.attr = self.d[0x05A0:0x05A0 + 256]
        self.px_w = self.u16(0x2E)
        self.px_h = self.u16(0x30)

    def u8(self, a):
        return self.d[a]

    def u16(self, a):
        return self.d[a] | (self.d[a + 1] << 8)

    def tile_id(self, tx, ty):
        idx = (((ty >> 4) * self.chunk_cols + (tx >> 4)) * 256
               + (ty & 15) * 16 + (tx & 15))
        return self.d[0x8000 + idx]

    def attr_at(self, tx, ty):
        """Mirror of `$00:91C3`, including its two out-of-bounds returns."""
        if tx >= self.w:
            return SOLID          # `$920A` — past the right edge reads as wall
        if ty >= self.h:
            return EMPTY          # `$9205` — below the map reads as empty
        return self.attr[self.tile_id(tx, ty)]

    def standable(self, tx, ty):
        """The `AND #$0003 == $0003` predicate the ground probes use."""
        a = self.attr_at(tx, ty)
        return a in (SLOPE_A, SLOPE_B) or (a & 0x03) == 0x03

    def floor_below(self, tx, ty, limit=48):
        for dy in range(0, limit):
            if ty + dy >= self.h:
                return None
            if self.standable(tx, ty + dy):
                return ty + dy
        return None

    def sane(self):
        """Cheap guard: a wrong dump (or a non-action mode) gives absurd dims."""
        return (self.mode != 0 and 0 < self.w <= 1024 and 0 < self.h <= 1024
                and self.chunk_cols > 0)


def report(st):
    print(f"mode $18=${st.mode:02X} $19=${st.sub:02X}")
    if isinstance(st, RomStage):
        print(f"source: ROM asset script — map blob ${st.map_ptr:06X}, "
              f"metatiles ${st.mt_ptr:06X}")
    print(f"level {st.px_w}x{st.px_h} px = {st.w}x{st.h} tiles "
          f"= {st.w // 16}x{st.h // 16} chunks   ($2F chunk columns = "
          f"{st.chunk_cols})")
    hist = collections.Counter(
        st.attr_at(tx, ty) for ty in range(st.h) for tx in range(st.w))
    print("\nattribute histogram over the map:")
    for a, n in sorted(hist.items()):
        print(f"  ${a:X}  {classify(a):<14} {n:6d} tiles")


def render(st, path, scale=3):
    shade = {EMPTY: 0x18, SOLID: 0xFF, SLOPE_A: 0xB0, SLOPE_B: 0xB0,
             TOP_HALF: 0x88}
    w, h = st.w * scale, st.h * scale
    rows = []
    for ty in range(st.h):
        line = bytearray()
        for tx in range(st.w):
            line += bytes([shade.get(st.attr_at(tx, ty), 0x60)] * scale)
        rows.extend([bytes(line)] * scale)
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(tag, data):
        c = tag + data
        return (struct.pack('>I', len(data)) + c
                + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)
    print(f"wrote {path} ({w}x{h})")


class RomStage:
    """Collision view built entirely from ROM, via the per-map asset script.

    Chain (all verified byte-exact against saves/dump_wram.bin for $18=01/$19=01):
      script $05:8000  -> bit-4 cmd, selector $01  -> metatile-id map blob
                       -> bit-5 cmd, selector $01  -> metatile definition blob
      map blob header  = [widthChunks][heightChunks][size16] then the stream;
                         widthChunks IS `$2F`, the chunk-column count
      metatile blob    = 2048 bytes, stored byte-SWAPPED relative to $7E:2100
      attribute table  = rebuilt exactly as `$02:BAC1` does (bit 1 of each of the
                         four sub-tile tilemap words, packed bit0 TL .. bit3 BR)

    Maps that carry no command of a given kind inherit the last one loaded, so
    the script is walked in order and the most recent blob is carried forward.
    """

    def __init__(self, rom, mode, sub):
        import act_content as ac
        import quintet_lzss as q
        map_ptr = mt_ptr = None
        found = False
        for m, s, cmds in ac.iter_script(rom):
            for c, ops in cmds:
                bit = ac.high_bit(c)
                if bit == 4 and len(ops) == 4 and ops[0] == 0x01:
                    map_ptr = ops[1] | (ops[2] << 8) | (ops[3] << 16)
                elif bit == 5 and len(ops) == 7 and ops[3] == 0x01:
                    mt_ptr = ops[4] | (ops[5] << 8) | (ops[6] << 16)
            if (m, s) == (mode, sub):
                found = True
                break
        if not found:
            raise SystemExit(f"no asset-script entry for "
                             f"$18=${mode:02X} $19=${sub:02X}")
        if map_ptr is None or mt_ptr is None:
            raise SystemExit(f"$18=${mode:02X} $19=${sub:02X}: no map/metatile "
                             f"blob in scope (map={map_ptr}, metatiles={mt_ptr})")
        self.mode, self.sub = mode, sub
        self.map_ptr, self.mt_ptr = map_ptr, mt_ptr
        self.chunk_cols = rom[map_ptr]
        chunk_rows = rom[map_ptr + 1]
        self.w = self.chunk_cols * 16
        self.h = chunk_rows * 16
        self.px_w, self.px_h = self.w * 16, self.h * 16
        self.tilemap, _ = q.decompress(rom, map_ptr + 2)
        mt, _ = q.decompress(rom, mt_ptr)
        mt = mt[:len(mt) & ~1]
        defs = bytearray(mt)
        defs[0::2], defs[1::2] = mt[1::2], mt[0::2]      # ROM stores it swapped
        # `$02:BAC1`: bit 1 of each sub-tile word's HIGH byte -> quadrant mask
        self.attr = bytearray(256)
        for mid in range(min(256, len(defs) // 8)):
            v = 0
            for quad in range(4):
                hi = defs[mid * 8 + quad * 2 + 1]
                v |= ((hi >> 1) & 1) << quad
            self.attr[mid] = v

    def tile_id(self, tx, ty):
        idx = (((ty >> 4) * self.chunk_cols + (tx >> 4)) * 256
               + (ty & 15) * 16 + (tx & 15))
        return self.tilemap[idx] if idx < len(self.tilemap) else 0

    attr_at = Stage.attr_at
    standable = Stage.standable
    floor_below = Stage.floor_below

    def sane(self):
        return self.w > 0 and self.h > 0 and self.chunk_cols > 0


def placements(rom, mode, sub):
    """The bank-$0A object placements for this stage (see act_content.py)."""
    def u8(a):
        return rom[0x0A * 0x8000 + (a - 0x8000)]

    def u16(a):
        o = 0x0A * 0x8000 + (a - 0x8000)
        return rom[o] | (rom[o + 1] << 8)

    key = (sub << 8) | mode
    a, base = 0xB100, None
    while u16(a) != 0xFFFF:
        if u16(a) == key:
            base = 0xB100 + u16(a + 2)
            break
        a += 4
    if base is None:
        return None, []
    y = base
    start = (u8(y), u8(y + 1))
    y += 3
    while u8(y) != 0xFF:
        y += 5
    y += 1
    out, seen = [], set()
    while y not in seen:
        seen.add(y)
        op = u8(y)
        if op == 0xFF:
            break
        if op == 0xFE:
            y += 5
            continue
        if op == 0xFD:
            y += 2
            continue
        if op == 0xFC:
            y = u16(y + 1)
            continue
        out.append((u8(y), u8(y + 1), u8(y + 2), u8(y + 3), y))
        y += 4
    return start, out


def inspect(st, tx, ty, body=2):
    """Classify one placement.

    A placement's tileY is a GROUND LINE, not a body position: `$00:96A4`
    subtracts the object's own down-extent (`$10`, set by the frame-0 load at
    `$00:969B`) from the spawn Y, so the sprite is lifted to rest on tile `ty`.
    A well-formed ground placement therefore has `ty` standable and the `body`
    tiles above it clear. `body` is an assumption: the real height is the
    frame's down-extent, which lives in the per-act `$7E:4000` blob.
    """
    flags = []
    if not st.standable(tx, ty):
        floor = st.floor_below(tx, ty)
        if floor is None:
            flags.append('airborne, no floor below')
        else:
            flags.append(f'airborne, floor {floor - ty} tiles down')
    for dy in range(1, body + 1):
        if st.attr_at(tx, ty - dy) == SOLID:
            flags.append(f'body blocked {dy} tile(s) up')
            break
    return flags


def check(st, rom, body=2):
    """Cross-check this stage's placements against the collision map."""
    start, objs = placements(rom, st.mode, st.sub)
    if start is None:
        print(f"no bank-$0A stage for $18=${st.mode:02X} $19=${st.sub:02X}")
        return
    print(f"\nplacement check ({len(objs)} objects; placement Y = ground line, "
          f"assuming a {body}-tile body):")
    f = inspect(st, start[0], start[1], body)
    print(f"  player start tile({start[0]},{start[1]}) "
          f"{classify(st.attr_at(*start))} — {', '.join(f) or 'OK'}")
    flagged = 0
    for tx, ty, param, typ, addr in objs:
        f = inspect(st, tx, ty, body)
        if not f:
            continue
        flagged += 1
        label = 'STATUE' if typ == 0x80 else f'type ${typ:02X}'
        print(f"  @$0A:{addr:04X} {label:<12} tile({tx:3d},{ty:3d}) "
              f"attr=${st.attr_at(tx, ty):X} "
              f"{classify(st.attr_at(tx, ty)):<12} {', '.join(f)}")
    print(f"  {flagged}/{len(objs)} flagged"
          + (" — all placements rest on standable ground with clear headroom"
             if not flagged else
             "  (airborne is legitimate for flying types; treat as review, "
             "not error)"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--wram',
                    help='WRAM dump (F2 snapshot or run-dir dump_wram.bin)')
    ap.add_argument('--map',
                    help='MODE,SUB (e.g. 1,1) — build from ROM instead, with '
                         'no dump needed')
    ap.add_argument('--png', help='write a collision-map PNG')
    ap.add_argument('--probe', help='tileX,tileY — report the attribute there')
    ap.add_argument('--check', action='store_true',
                    help="cross-check this stage's bank-$0A placements")
    ap.add_argument('--rom', help='ROM path (default ar.sfc / $AR_ROM)')
    args = ap.parse_args()

    if args.map:
        mode, sub = (int(v, 0) for v in args.map.split(','))
        st = RomStage(load_rom(args.rom), mode, sub)
    elif args.wram:
        st = Stage(args.wram)
    else:
        ap.error('need --wram <dump> or --map MODE,SUB')
    if not st.sane():
        print(f"{args.wram}: not an action-mode dump "
              f"($18=${st.mode:02X}, {st.w}x{st.h} tiles) — collision state is "
              f"only built on action-stage entry", file=sys.stderr)
        return 1
    report(st)
    if args.probe:
        tx, ty = (int(v, 0) for v in args.probe.split(','))
        a = st.attr_at(tx, ty)
        print(f"\ntile({tx},{ty}) id=${st.tile_id(tx, ty):02X} attr=${a:X} "
              f"{classify(a)}  standable={st.standable(tx, ty)}  "
              f"floor below={st.floor_below(tx, ty)}")
    if args.png:
        render(st, args.png)
    if args.check:
        check(st, load_rom(args.rom))
    return 0


if __name__ == '__main__':
    sys.exit(main())
