#!/usr/bin/env python3
"""quintet_lzss.py — faithful port of ActRaiser's LZSS decompressor `$02:C5C9`.

The format is **bit packed**, not byte aligned; the 256-byte ring buffer is
pre-filled with `$20` and its write cursor starts at `$EF`. A byte-aligned
Quintet LZSS decoder will consume the stream and emit plausible-looking bytes
while being wrong, so `--verify` checks real blobs byte-for-byte against a live
WRAM dump. Use it after any change here.

The three routines this ports:

  `$02:C5C9`  driver. Fills the ring at `$7E:2000` with `$20`, sets the write
              cursor `$AF` = `$EF` and the control-bit mask `$AE` = `$80`, then
              per iteration reads one control bit: SET = literal byte, CLEAR =
              (ring offset, length) match. Output count comes from `$B3`, which
              `$02:B69C` takes from the blob's own first word.
  `$02:C639`  read 8 bits at the current bit position. Reads a 16-bit word at
              `[$A5]`, `XBA`s it into `$B7/$B8`, shifts left by the current bit
              phase (derived by walking the `$AE` mask) and takes `$B8`. Then
              `INC $A5` — so the byte pointer advances by 1 and the bit phase is
              unchanged, i.e. exactly "consume 8 bits".
  `$02:C66C`  read the 4-bit match length. Two spellings depending on whether
              the nibble fits in the current byte (`$AE >= $10`) or straddles
              into the next one, but both are "consume 4 bits". Length = n + 2.

All three reduce to MSB-first reads from one bit stream, which is what this
implements; `--selfcheck` additionally verifies that claim against a literal
transcription of the two straddling cases.

Blob convention (per `$02:B69C`): the first word at the pointer is the
decompressed size and the bit stream starts at +2.

Examples:
  tools/quintet_lzss.py 0x0CD695                    # auto size from the header
  tools/quintet_lzss.py 0x0CD695 --out /tmp/blob.bin
  tools/quintet_lzss.py --verify                    # check against a WRAM dump
"""
import argparse
import sys

from ar_lib import load_rom

RING_FILL = 0x20        # `$02:C5DC`: LDA #$20, fill $7E:2000..$20FF
RING_START = 0xEF       # `$02:C5E4`: LDA #$EF, STA $AF
MIN_MATCH = 2           # `$02:C61C`: INC A twice


class BitReader:
    """MSB-first bit stream over a ROM slice.

    `$02:C5F5`'s control-bit read advances the phase by 1 (reloading `$AE` to
    `$80` and bumping `$A5` on wrap); `$02:C639` advances the byte pointer by 1
    while leaving the phase alone; `$02:C66C` advances the phase by 4 with a
    carry into the byte pointer. All three are `bitpos += n`.
    """

    def __init__(self, data, offset):
        self.data = data
        self.bitpos = offset * 8

    def read(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.bitpos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.bitpos & 7))) & 1)
            self.bitpos += 1
        return v

    @property
    def byte_pos(self):
        return self.bitpos >> 3


def decompress(rom, offset, size=None):
    """Decompress at `offset`. With `size=None`, take it from the blob header.

    Returns (output_bytes, compressed_length_in_bytes).
    """
    if size is None:
        size = rom[offset] | (rom[offset + 1] << 8)
        offset += 2
    br = BitReader(rom, offset)
    ring = bytearray([RING_FILL] * 256)
    wpos = RING_START
    out = bytearray()

    while len(out) < size:
        if br.read(1):
            # `$02:C604`: literal — 8 bits straight through to dest and ring.
            b = br.read(8)
            out.append(b)
            ring[wpos] = b
            wpos = (wpos + 1) & 0xFF
        else:
            # `$02:C614`: match — ring offset byte, then the 4-bit length.
            src = br.read(8)
            n = br.read(4) + MIN_MATCH
            for _ in range(n):
                b = ring[src]
                src = (src + 1) & 0xFF
                ring[wpos] = b
                wpos = (wpos + 1) & 0xFF
                out.append(b)
                if len(out) == size:
                    break
    return bytes(out), br.byte_pos - offset + 1


def selfcheck():
    """Verify the uniform bit reader against a literal transcription of the two
    `$02:C66C` spellings (the part of the format most likely to be misread)."""
    import random
    rnd = random.Random(1)
    data = bytes(rnd.randrange(256) for _ in range(64))
    for k in range(8):
        mask = 0x80 >> k
        br = BitReader(data, 0)
        br.bitpos = k
        uniform = br.read(4)
        pos = 0
        if mask >= 0x10:
            # `$02:C672`: $AE >>= 4; A16 = (byte<<8) | ($AE) ; LSR until carry
            ae = mask >> 4
            a = (data[pos] << 8) | ae
            for _ in range(4):
                carry = a & 1
                a >>= 1
                if carry:
                    break
            literal = (a >> 8) & 0x0F
        else:
            # `$02:C68E`: $AE <<= 4; word swapped into $B7/$B8; ASL by phase
            ae = (mask << 4) & 0xFF
            w = ((data[pos] << 8) | data[pos + 1])
            shift = {0x80: 0, 0x40: 1, 0x20: 2, 0x10: 3}[ae]
            w = (w << shift) & 0xFFFF
            literal = (w >> 8) & 0x0F
        if uniform != literal:
            print(f"  MISMATCH at bit phase {k}: uniform={uniform:X} "
                  f"literal={literal:X}")
            return False
        print(f"  bit phase {k} (mask ${mask:02X}): nibble ${uniform:X} — "
              f"uniform reader agrees with the $C66C transcription")
    return True


def verify(rom, wram_path):
    """Decompress the sources named by the asset script and compare to WRAM."""
    d = open(wram_path, 'rb').read()
    cases = [
        ("objects  -> $7E:4000", 0x0CD695, 0x4000),
        ("boss     -> $7E:5000", 0x03EFC7, 0x5000),
    ]
    ok = True
    for name, src, dest in cases:
        out, used = decompress(rom, src)
        want = d[dest:dest + len(out)]
        good = out == want
        ok &= good
        print(f"  {name}: linear ${src:06X} -> {len(out)} bytes "
              f"({used} compressed)  {'MATCH' if good else 'MISMATCH'}")
        if not good:
            for i, (a, b) in enumerate(zip(out, want)):
                if a != b:
                    print(f"     first difference at +{i:#x}: "
                          f"got ${a:02X} want ${b:02X}")
                    break
            print(f"     got  {out[:16].hex(' ')}")
            print(f"     want {bytes(want[:16]).hex(' ')}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('offset', nargs='?',
                    help='linear ROM file offset of the blob (e.g. 0x0CD695)')
    ap.add_argument('--size', type=lambda s: int(s, 0),
                    help='decompressed size; default = the blob header word')
    ap.add_argument('--out', help='write the decompressed bytes here')
    ap.add_argument('--verify', action='store_true',
                    help='check two known blobs against saves/dump_wram.bin')
    ap.add_argument('--selfcheck', action='store_true',
                    help='check the bit reader against the $C66C transcription')
    ap.add_argument('--wram', default='saves/dump_wram.bin')
    ap.add_argument('--rom', help='ROM path (default ar.sfc / $AR_ROM)')
    args = ap.parse_args()

    if args.selfcheck:
        print("bit-reader self-check:")
        if not selfcheck():
            return 1
    rom = load_rom(args.rom)
    if args.verify:
        print("verification against a live WRAM dump:")
        if not verify(rom, args.wram):
            return 1
    if args.offset:
        off = int(args.offset, 0)
        out, used = decompress(rom, off, args.size)
        print(f"linear ${off:06X} = ${off >> 15:02X}:{0x8000 | (off & 0x7FFF):04X}"
              f"  ->  {len(out)} bytes from {used} compressed")
        print("  first 32:", out[:32].hex(' '))
        if args.out:
            open(args.out, 'wb').write(out)
            print(f"  wrote {args.out}")
    if not (args.offset or args.verify or args.selfcheck):
        ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
