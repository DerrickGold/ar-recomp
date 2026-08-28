"""bg_render_level.py — render a WHOLE action level straight out of WRAM.

    python3 tools/bg_render_level.py <snapshot-prefix> <layer 0|1> <out.ppm>

Decodes every 256x256 page of the level map in WRAM bank $7E through the
metatile table, then rasterises it with the snapshot's VRAM char data and CGRAM.
The emulator is not in the loop. Written to answer "is the whole level really
resident, or just a window around the camera?" -- if the output is coherent from
end to end while the camera sits in one place, it is resident. See
docs/bg-hle-census.md.

Caveat: CHRBASE is assumed 0 (correct for the Fillmore acts, where BG1 ids are
<$100 and BG2 $100-$1FF in one shared 4bpp region). Read BG12NBA if a level
renders as garbage.
"""
import struct, sys

from ar_lib import read_le16
d, layer, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
W = open(f"{d}.wram.bin","rb").read()
V = struct.unpack("<32768H", open(f"{d}.vram.bin","rb").read())
C = struct.unpack("<256H", open(f"{d}.cgram.bin","rb").read())
o = layer*4
width,height = read_le16(W, 0x2E+o), read_le16(W, 0x30+o)
s46,s52,s54 = (read_le16(W, 0x46+o), read_le16(W, 0x52+o),
               read_le16(W, 0x54+o))
s6b = W[0x6B+o]; orw = s6b<<8
pw = width>>8; base=(s46>>8)<<8
CHRBASE = 0x0000                      # 4bpp chars, word address

def tile_pixels(tid):                 # -> 8x8 list of palette indices 0..15
    a = CHRBASE + tid*16
    rows=[]
    for y in range(8):
        p0 = V[(a+y)&0x7fff]; p1 = V[(a+8+y)&0x7fff]
        row=[]
        for x in range(8):
            b=7-x
            v = ((p0>>b)&1) | (((p0>>(b+8))&1)<<1) | (((p1>>b)&1)<<2) | (((p1>>(b+8))&1)<<3)
            row.append(v)
        rows.append(row)
    return rows
cache={}
def tp(t):
    if t not in cache: cache[t]=tile_pixels(t)
    return cache[t]
def rgb(ci):
    c=C[ci]; r=(c&31)<<3; g=((c>>5)&31)<<3; b=((c>>10)&31)<<3
    return bytes((r,g,b))

img = bytearray(width*height*3)
for wy in range(0, height, 16):
    for wx in range(0, width, 16):
        page = (wy>>8)*pw + (wx>>8)
        mt = W[(base + (page<<8) + (wy&0xF0) + ((wx&0xF0)>>4)) & 0x1FFFF]
        for k in range(4):
            a = s52 + mt*8 + k*2
            word = ((W[a]|(W[a+1]<<8)) & s54) | orw
            tid = word & 0x3FF; pal=(word>>10)&7; fx=(word>>14)&1; fy=(word>>15)&1
            px = tp(tid)
            ox, oy = wx + (k&1)*8, wy + (k>>1)*8
            for y in range(8):
                sy = 7-y if fy else y
                for x in range(8):
                    sx = 7-x if fx else x
                    v = px[sy][sx]
                    if v==0: continue          # colour 0 = transparent
                    off = ((oy+y)*width + ox+x)*3
                    img[off:off+3] = rgb(pal*16+v)
open(out,"wb").write(b"P6\n%d %d\n255\n"%(width,height)+bytes(img))
print(f"wrote {out}  {width}x{height}  layer BG{layer+1}")
