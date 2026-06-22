#!/usr/bin/env python3
"""Timing-independent value-SEQUENCE diff.

The two emulators don't run in game-frame lockstep (the recomp doesn't
reproduce snes9x's vblank "lag frames", so it advances a few % more
game-frames over the same input). That defeats any frame-aligned WRAM diff.

But the trace only records *changes*, so each address's ordered sequence of
written values is naturally timing-independent: a lag frame writes nothing,
and an A->B->A toggle yields the same [A,B,A,...] on both sides no matter how
many frames pad it. So if execution is equivalent, every address's value
sequence on one side should be a PREFIX of the other (one side just got
further). Addresses whose sequences genuinely DIVERGE in content/order — not
merely length — are the real behavioral divergence, with timing removed.

Reports those, earliest-real-divergence first (by the recomp frame at which
the first mismatching value was written).

Usage:
    diff_seq.py oracle.jsonl recomp.jsonl [--top N] [--skip-zp]
                [--lo 0xA --hi 0xB] [--min-prefix N]
"""
import sys, json, argparse
from collections import defaultdict


def load_seqs(path):
    """addr -> list of (frame, value), consecutive duplicates collapsed."""
    seq = defaultdict(list)
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            a = int(r["adr"], 16)
            v = int(r["val"], 16)
            fr = r["f"]
            s = seq[a]
            if not s or s[-1][1] != v:
                s.append((fr, v))
    return seq


def fmt_addr(a):
    if a >= 0x10000:
        return f"$7F{a - 0x10000:04X}"
    return f"$7E{a:04X}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle")
    ap.add_argument("recomp")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--skip-zp", action="store_true",
                    help="ignore $0000-$01FF (zero-page scratch + stack)")
    ap.add_argument("--lo", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--hi", type=lambda x: int(x, 0), default=0x1FFFF)
    ap.add_argument("--min-prefix", type=int, default=0,
                    help="only report addrs whose sequences agreed for at "
                         "least this many values before diverging")
    args = ap.parse_args()

    o = load_seqs(args.oracle)
    r = load_seqs(args.recomp)

    def keep(a):
        if a in (0x88, 0x89):
            return False
        if args.skip_zp and a <= 0x01FF:
            return False
        return args.lo <= a <= args.hi

    common = [a for a in (set(o) & set(r)) if keep(a)]
    consistent = 0          # one seq is a prefix of the other
    divergent = []          # (recompFrame, seqIdx, addr, oval, rval, prefixlen)
    for a in common:
        os_, rs_ = o[a], r[a]
        n = min(len(os_), len(rs_))
        div_i = -1
        for i in range(n):
            if os_[i][1] != rs_[i][1]:
                div_i = i
                break
        if div_i < 0:
            consistent += 1
        else:
            if div_i >= args.min_prefix:
                divergent.append((rs_[div_i][0], div_i, a,
                                  os_[div_i][1], rs_[div_i][1], div_i))

    print(f"oracle addrs: {len(o)}   recomp addrs: {len(r)}   "
          f"common(kept): {len(common)}")
    print(f"sequence-consistent (prefix match): {consistent}   "
          f"genuinely divergent: {len(divergent)}")
    pct = 100.0 * consistent / len(common) if common else 0
    print(f"=> {pct:.1f}% of common addresses follow the SAME value sequence "
          f"(timing aside)\n")

    divergent.sort()
    print(f"=== earliest genuine sequence divergences (top {args.top}) ===")
    print(f"{'addr':>10} {'seqIdx':>6} {'recF':>6} {'oracleVal':>9} {'recompVal':>9}  prefix")
    for rf, idx, a, ov, rv, pl in divergent[:args.top]:
        print(f"{fmt_addr(a):>10} {idx:>6} {rf:>6} {ov:>#9x} {rv:>#9x}  "
              f"agreed {pl} vals first")


if __name__ == "__main__":
    main()
