#!/usr/bin/env python3
"""dis65.py — 65816 disassembler for the ActRaiser ROM (LoROM, headerless).

Replaces the eyeball-hex-decode workflow (DEBUG.md toolbox). Tracks m/x through
SEP/REP, annotates call/branch targets, and marks gen_meta.json facts (function
entries, decoder labels) inline so you can see registration state while reading.

Examples:
  tools/dis65.py 01:9C6F --mx 0,0 -n 24        # reward dispatcher
  tools/dis65.py 00:9DE1 --mx 0,0 --until-flow  # cast gate to its first exit
  tools/dis65.py 03:9D4D --mx 1,0 -n 40 --raw   # with hex bytes
"""
import argparse
import sys

from ar_lib import load_rom, load_meta, parse_addr, fmt24, disasm, decode_one, CALLS


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('addr', help='start address, BB:AAAA form')
    ap.add_argument('-n', '--count', type=int, default=24, help='instruction count (default 24)')
    ap.add_argument('--mx', default='0,0', help='entry m,x widths (default 0,0)')
    ap.add_argument('--until-flow', action='store_true',
                    help='stop after RTS/RTL/RTI or unconditional JMP/BRA/BRL')
    ap.add_argument('--raw', action='store_true', help='show raw hex bytes')
    ap.add_argument('--rom', help='ROM path (default ar.sfc / $AR_ROM)')
    args = ap.parse_args()

    rom = load_rom(args.rom)
    meta = load_meta()
    m, x = (int(v) for v in args.mx.split(','))
    pc24 = parse_addr(args.addr)

    count = None if args.until_flow else args.count
    for ins in disasm(rom, pc24, m, x, count=count,
                      until_flow=args.until_flow,
                      max_insns=args.count if args.until_flow else 512):
        key = f"{(ins.pc24 >> 16) & 0xFF:02X}{ins.pc24 & 0xFFFF:04X}"
        marks = []
        if meta:
            fv = meta['functions'].get(key)
            if fv:
                marks.append(f"FUNC[{','.join(v.lstrip('_') for v in fv)}]")
            elif key in meta.get('labels', {}):
                marks.append('label')
        if ins.target is not None and meta:
            tkey = f"{(ins.target >> 16) & 0xFF:02X}{ins.target & 0xFFFF:04X}"
            if ins.mnem in CALLS or ins.mnem in ('JMP', 'JML', 'BRL', 'BRA'):
                if tkey in meta['functions']:
                    marks.append(f"->FUNC {fmt24(ins.target)}")
                elif tkey in meta.get('labels', {}):
                    marks.append(f"->label {fmt24(ins.target)}")
        raw = ' '.join(f"{b:02X}" for b in ins.raw).ljust(12) if args.raw else ''
        note = ('   ; ' + ' '.join(marks)) if marks else ''
        print(f"{fmt24(ins.pc24)}  {raw}{ins.text:<20} m={ins.m} x={ins.x}{note}")


if __name__ == '__main__':
    sys.exit(main())
