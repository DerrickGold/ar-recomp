"""bg_hle.py — native reimplementation of the action-stage BG decode, checked
against live VRAM.

    python3 tools/bg_hle.py runs/<run>/snapshots/snap_00_gf<N>

Reads <prefix>.wram.bin + <prefix>.vram.bin and rebuilds every tilemap cell the
snapshot's view displays, straight from WRAM, with no emulator in the loop. This
is the oracle for SPEC-bg-hle: a disagreement is either an HLE error or a
streaming bug (see docs/bug-ledger.md §37 for a case where it was the latter,
and docs/rendering-engine.md §12b for the addressing it implements).

HLE of $02:B8A0 -> $02:B95A, derived from disassembly. The level map is NOT in
ROM: level entry expands it into WRAM bank $7E as a flat array of 256-byte
pages, one byte per 16x16 metatile, 16x16 metatiles per page.
"""
import struct, sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
d = sys.argv[1]
W = open(f"{d}.wram.bin", "rb").read()          # $7E:0000.. (128K = 7E+7F)
V = struct.unpack("<32768H", open(f"{d}.vram.bin", "rb").read())
def r8(a):  return W[a]
def r16(a): return W[a] | (W[a+1] << 8)

for layer, name, tmbase in ((0, "BG1", 0x6000), (1, "BG2", 0x7000)):
    o = layer * 4
    camx, camy = r16(0x22+o), r16(0x24+o)
    width      = r16(0x2E+o)
    s46, s48   = r16(0x46+o), r16(0x48+o)
    s52, s54   = r16(0x52+o), r16(0x54+o)
    s6b        = r8(0x6B+o)
    or_word    = (s6b << 8)                      # DP $06/$07 = 0 | State6B<<8
    pages_wide = width >> 8

    def metatile_id(wx, wy):
        page = (wy >> 8) * pages_wide + (wx >> 8) + (s46 >> 8)
        return W[((page << 8) | (wy & 0xF0) | ((wx & 0xF0) >> 4)) & 0x1FFFF]

    def tiles(wx, wy):                            # -> TL, TR, BL, BR
        base = s52 + (metatile_id(wx, wy) << 3)
        return [((W[base+2*k] | (W[base+2*k+1] << 8)) & s54) | or_word
                for k in range(4)]

    def vram_cell(tx, ty):
        col, row = tx & 63, ty & 63
        return V[tmbase + (row & 31)*0x20 + (col & 31)
                 + (0x400 if col >= 32 else 0) + (0x800 if row >= 32 else 0)]

    # Compare every tile the current view displays.
    vl, vr = camx - 95, camx + 256 + 95
    top    = camy - 32
    ok = bad = 0; first = []
    for wy in range(max(top, 0), camy + 224, 8):
        for wx in range(max(vl, 0), vr, 8):
            t = tiles(wx & ~15, wy & ~15)
            want = t[((wy >> 3) & 1) * 2 + ((wx >> 3) & 1)]
            got  = vram_cell(wx >> 3, wy >> 3)
            if want == got: ok += 1
            else:
                bad += 1
                if len(first) < 4: first.append((wx, wy, want, got))
    print(f"{name}: cam=({camx},{camy}) width={width} pagesWide={pages_wide} "
          f"map7E=${s46>>8:02X}xx tbl=${s52:04X} mask=${s54:04X} or=${or_word:04X}")
    print(f"     match {ok}/{ok+bad} = {100*ok/(ok+bad):.2f}%")
    for wx, wy, want, got in first:
        print(f"     MISS world=({wx},{wy}) want={want:04X} got={got:04X}")
