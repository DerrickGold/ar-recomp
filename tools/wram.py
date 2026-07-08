#!/usr/bin/env python3
"""wram.py — WRAM snapshot/dump inspector for the ActRaiser recomp.

Works on saves/snapshots/*.wram.bin (F2) and saves/dump_wram.bin (exit dump),
with docs/ram-map.md as the symbol table. Replaces the ad-hoc python heredocs
of the bracket-snapshot protocol (DEBUG.md §2).

Commands:
  get   <file...> <addr|sym...>     read named bytes/words across files
  diff  <fileA> <fileB> [--all]     diff, low-page annotated, high clustered
  scan  <file> (--byte NN | --word NNNN) [--range LO-HI]
  syms  [filter]                    list known ram-map symbols

Addresses: WRAM offsets (0021, 0295, 2AC) or 7E/7F-prefixed (7E0021, 7F9101).
Symbols: any ram-map name fragment, e.g. 'magic-points', 'level'.

Examples:
  tools/wram.py get saves/dump_wram.bin 21 0295 02AC 18
  tools/wram.py get saves/snapshots/snap_0*.wram.bin 0295
  tools/wram.py diff saves/snapshots/snap_02*.wram.bin saves/snapshots/snap_03*.wram.bin
  tools/wram.py scan saves/dump_wram.bin --word 9832 --range 0000-2000
"""
import argparse
import glob
import sys

from ar_lib import load_symbols


def woff(s, by_name):
    t = s.strip().lstrip('$').replace(':', '').lower()
    for k, v in by_name.items():
        if t and not all(c in '0123456789abcdef' for c in t) and t in k:
            return v
    v = int(t, 16)
    if v >= 0x7F0000:
        return (v & 0xFFFF) + 0x10000
    if v >= 0x7E0000:
        return v & 0xFFFF
    return v


def fmtoff(off):
    return f"$7{'F' if off >= 0x10000 else 'E'}:{off & 0xFFFF:04X}"


def rd(path):
    d = open(path, 'rb').read()
    if len(d) not in (0x20000, 0x10000):
        print(f"warning: {path} is {len(d)} bytes (expected 128K WRAM)", file=sys.stderr)
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    g = sub.add_parser('get')
    g.add_argument('args', nargs='+', help='files then addrs/syms (files end in .bin)')

    d = sub.add_parser('diff')
    d.add_argument('a')
    d.add_argument('b')
    d.add_argument('--all', action='store_true', help='list every diff byte (no clustering)')
    d.add_argument('--low', type=lambda v: int(v, 16), default=0x800,
                   help='annotate individually below this offset (default 800)')

    s = sub.add_parser('scan')
    s.add_argument('file')
    s.add_argument('--byte', type=lambda v: int(v, 16))
    s.add_argument('--word', type=lambda v: int(v, 16))
    s.add_argument('--range', default='0000-1FFFF')

    y = sub.add_parser('syms')
    y.add_argument('filter', nargs='?', default='')

    args = ap.parse_args()
    by_addr, by_name = load_symbols()

    if args.cmd == 'syms':
        for off in sorted(by_addr):
            if args.filter.lower() in by_addr[off].lower():
                print(f"{fmtoff(off)}  {by_addr[off]}")
        return

    if args.cmd == 'get':
        files, addrs = [], []
        for a in args.args:
            (files if a.endswith('.bin') or glob.glob(a) else addrs).append(a)
        files = [f for pat in files for f in sorted(glob.glob(pat))] or files
        offs = [woff(a, by_name) for a in addrs]
        for f in files:
            d = rd(f)
            parts = []
            for o in offs:
                sym = by_addr.get(o)
                name = f"{fmtoff(o)}" + (f"[{sym}]" if sym else '')
                w = d[o] | (d[o + 1] << 8)
                parts.append(f"{name}={d[o]:02X} (w:{w:04X})")
            print(f"{f.split('/')[-1]:<28} " + '  '.join(parts))
        return

    if args.cmd == 'diff':
        A = sorted(glob.glob(args.a))[0] if glob.glob(args.a) else args.a
        B = sorted(glob.glob(args.b))[0] if glob.glob(args.b) else args.b
        da, db = rd(A), rd(B)
        n = min(len(da), len(db))
        diffs = [i for i in range(n) if da[i] != db[i]]
        print(f"{len(diffs)} byte diffs   ({A.split('/')[-1]} -> {B.split('/')[-1]})")
        low = [i for i in diffs if i < args.low]
        for i in low:
            sym = by_addr.get(i)
            print(f"  {fmtoff(i)}: {da[i]:02X} -> {db[i]:02X}" + (f"   {sym}" if sym else ''))
        rest = [i for i in diffs if i >= args.low]
        if args.all:
            for i in rest:
                print(f"  {fmtoff(i)}: {da[i]:02X} -> {db[i]:02X}")
        elif rest:
            groups, start, prev = [], rest[0], rest[0]
            for i in rest[1:]:
                if i - prev > 16:
                    groups.append((start, prev))
                    start = i
                prev = i
            groups.append((start, prev))
            print(f"  high clusters ({len(groups)}): "
                  + ', '.join(f"{fmtoff(s)}-{e & 0xFFFF:04X}" for s, e in groups[:32])
                  + (' …' if len(groups) > 32 else ''))
        return

    if args.cmd == 'scan':
        d = rd(args.file)
        lo, hi = (int(v, 16) for v in args.range.split('-'))
        if args.word is not None:
            for i in range(lo, min(hi, len(d) - 1)):
                if d[i] | (d[i + 1] << 8) == args.word:
                    sym = by_addr.get(i)
                    print(f"{fmtoff(i)} = {args.word:04X}" + (f"   {sym}" if sym else ''))
        elif args.byte is not None:
            hits = [i for i in range(lo, min(hi, len(d))) if d[i] == args.byte]
            for i in hits[:200]:
                print(f"{fmtoff(i)} = {args.byte:02X}")
            if len(hits) > 200:
                print(f"… {len(hits)} total")


if __name__ == '__main__':
    sys.exit(main())
