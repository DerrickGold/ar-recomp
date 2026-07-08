#!/usr/bin/env python3
"""romxref.py — alignment-aware cross-reference scanner for the ActRaiser ROM.

Answers "who reads/writes/calls/branches-to/tables this address?" WITHOUT the
raw-byte-grep false positives (the `82 88` / `85 A0` class): every candidate is
validated by decoding the instruction AT the site and, for confidence, decoding
a few instructions forward to check the stream stays sane. gen_meta.json
proximity (site inside/near decoded code) is reported as extra evidence.

For WRAM targets, absolute forms match the 16-bit address and long forms match
bank $00/$7E mirrors automatically.

Examples:
  tools/romxref.py 0295 --kind write          # who writes persistent MP
  tools/romxref.py 00:8875 --kind call        # who calls the scroll-award helper
  tools/romxref.py 01:9CD6 --kind word        # handler tables containing $9CD6/-1
  tools/romxref.py 00:9DE1 --kind branch      # branches into the cast gate
"""
import argparse
import sys

from ar_lib import (load_rom, load_meta, parse_addr, fmt24, decode_one,
                    OPCODES, disasm)

WRITE_MNEMS = {'STA', 'STX', 'STY', 'STZ', 'INC', 'DEC', 'ASL', 'LSR', 'ROL',
               'ROR', 'TSB', 'TRB'}
READ_MNEMS = {'LDA', 'LDX', 'LDY', 'BIT', 'CMP', 'CPX', 'CPY', 'ADC', 'SBC',
              'ORA', 'AND', 'EOR'}
ABS_MODES = {'abs', 'absx', 'absy'}
LONG_MODES = {'absl', 'abslx'}
BAD_STREAM = {'STP', 'WDM'}   # decoding into these = probably data


def sanity(rom, pc24, m, x, n=4):
    """Decode n insns forward; score stream sanity 0..n."""
    good = 0
    for ins in disasm(rom, pc24, m, x, count=n):
        if ins.mnem in BAD_STREAM:
            break
        good += 1
    return good


def meta_near(meta, pc24, dist=0x60):
    """Nearest decoded fact within +-dist, or None."""
    if not meta:
        return None
    bank = f"{(pc24 >> 16) & 0xFF:02X}"
    a = pc24 & 0xFFFF
    best = None
    for k in list(meta['functions']) + list(meta.get('labels', {})):
        if k[:2] != bank:
            continue
        d = abs(int(k[2:], 16) - a)
        if d <= dist and (best is None or d < best[0]):
            best = (d, k)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('addr', help='target: BB:AAAA, or AAAA for a WRAM/16-bit target')
    ap.add_argument('--kind', default='all',
                    choices=['write', 'read', 'call', 'branch', 'word', 'all'])
    ap.add_argument('--banks', default='00-1F', help='bank range to scan (default 00-1F)')
    ap.add_argument('--mx', default='0,0', help='m,x assumed when validating sites')
    ap.add_argument('--rom', help='ROM path')
    args = ap.parse_args()

    rom = load_rom(args.rom)
    meta = load_meta()
    m, x = (int(v) for v in args.mx.split(','))

    t = args.addr.strip().lstrip('$').replace(':', '')
    t16 = int(t, 16) & 0xFFFF
    t24 = int(t, 16) if len(t) > 4 else None       # full pc24 given?
    tbank = (t24 >> 16) & 0xFF if t24 is not None else None
    is_wram = t24 is None or tbank in (0x7E, 0x7F, 0x00)

    b_lo, b_hi = (int(v, 16) for v in args.banks.split('-')) if '-' in args.banks \
        else (int(args.banks, 16),) * 2
    kinds = ({args.kind} if args.kind != 'all' else
             {'write', 'read', 'call', 'branch', 'word'})

    hits = []
    for bank in range(b_lo, b_hi + 1):
        seg_base = bank * 0x8000
        if seg_base >= len(rom):
            break
        for off in range(0, min(0x8000, len(rom) - seg_base)):
            pc24 = (bank << 16) | (0x8000 + off)
            op = rom[seg_base + off]
            mnem, mode = OPCODES[op]
            match = None
            if mode in ABS_MODES and (mnem in WRITE_MNEMS or mnem in READ_MNEMS
                                      or mnem in ('JSR', 'JMP')):
                ins = decode_one(rom, pc24, m, x)
                if ins and ins.operand == t16:
                    if mnem in ('JSR', 'JMP'):
                        if 'call' in kinds and (tbank is None or tbank == bank):
                            match = 'call'
                    elif mnem in WRITE_MNEMS and 'write' in kinds and is_wram:
                        match = 'write'
                    elif mnem in READ_MNEMS and 'read' in kinds and is_wram:
                        match = 'read'
            elif mode in LONG_MODES and (mnem in WRITE_MNEMS or mnem in READ_MNEMS
                                         or mnem in ('JSL', 'JML')):
                ins = decode_one(rom, pc24, m, x)
                if ins and ins.operand is not None:
                    ob, oa = (ins.operand >> 16) & 0xFF, ins.operand & 0xFFFF
                    ok = (ins.operand == t24) if t24 is not None else False
                    if is_wram and oa == t16 and ob in (0x00, 0x7E):
                        ok = True
                    if ok:
                        if mnem in ('JSL', 'JML'):
                            if 'call' in kinds:
                                match = 'call'
                        elif mnem in WRITE_MNEMS and 'write' in kinds:
                            match = 'write'
                        elif mnem in READ_MNEMS and 'read' in kinds:
                            match = 'read'
            elif mode in ('rel8', 'rel16') and 'branch' in kinds:
                ins = decode_one(rom, pc24, m, x)
                if ins and ins.target is not None and \
                        (ins.target & 0xFFFF) == t16 and \
                        (tbank is None or (ins.target >> 16) == tbank == bank):
                    match = 'branch'
            if match:
                ins = decode_one(rom, pc24, m, x)
                sc = sanity(rom, pc24, m, x)
                near = meta_near(meta, pc24)
                hits.append((match, pc24, ins.text, sc, near))
        if 'word' in kinds:
            for off in range(0, min(0x8000, len(rom) - seg_base) - 1):
                w = rom[seg_base + off] | (rom[seg_base + off + 1] << 8)
                if w == t16 or w == ((t16 - 1) & 0xFFFF):
                    if tbank is not None and bank != tbank:
                        continue
                    pc24 = (bank << 16) | (0x8000 + off)
                    tag = 'word' if w == t16 else 'word-1'
                    near = meta_near(meta, pc24)
                    hits.append((tag, pc24, f".dw ${w:04X}", -1, near))

    hits.sort(key=lambda h: (-h[3], h[1]))
    for kind, pc24, text, sc, near in hits:
        conf = ('' if sc < 0 else
                'HIGH' if sc >= 4 else 'med ' if sc >= 2 else 'LOW ')
        nn = f"  near {near[1]} (+{near[0]:X})" if near else '  (no decoded code nearby)'
        print(f"{kind:<7} {fmt24(pc24)}  {text:<22} {conf}{nn}")
    if not hits:
        print("no hits")


if __name__ == '__main__':
    sys.exit(main())
