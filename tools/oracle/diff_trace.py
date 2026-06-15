#!/usr/bin/env python3
"""Differential oracle diff: find where the recompiled run's WRAM diverges
from the snes9x reference.

Both sides emit per-frame WRAM-change JSONL (same shape):
    {"f":<frame>,"adr":"0x0abcd","old":"0x12","val":"0x34"}

The two emulators don't boot frame-for-frame identically and start from
different power-on WRAM, so we don't compare raw byte images. Instead we
reconstruct each side's cumulative written-value state and compare over the
set of addresses BOTH sides actually write — surfacing the earliest-diverging
addresses as root-cause anchors.

Usage:
    diff_trace.py oracle_trace.jsonl recomp_trace.jsonl [--top N]
"""
import sys, json, argparse
from collections import defaultdict


def load(path):
    final = {}            # addr -> last written value
    first_frame = {}      # addr -> frame first written
    last_frame = {}       # addr -> frame last written
    writes = defaultdict(int)
    max_frame = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            fr = r["f"]
            adr = int(r["adr"], 16)
            val = int(r["val"], 16)
            final[adr] = val
            if adr not in first_frame:
                first_frame[adr] = fr
            last_frame[adr] = fr
            writes[adr] += 1
            if fr > max_frame:
                max_frame = fr
    return dict(final=final, first=first_frame, last=last_frame,
                writes=writes, max_frame=max_frame)


def fmt_addr(a):
    # WRAM byte addr -> SNES address ($7E0000 + a, or $7F for a>=0x10000)
    if a >= 0x10000:
        return f"$7F{a-0x10000:04X}"
    return f"$7E{a:04X}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle")
    ap.add_argument("recomp")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    o = load(args.oracle)
    r = load(args.recomp)
    print(f"oracle: {len(o['final'])} addrs written, max frame {o['max_frame']}")
    print(f"recomp: {len(r['final'])} addrs written, max frame {r['max_frame']}")

    common = set(o["final"]) & set(r["final"])
    divergent = [a for a in common if o["final"][a] != r["final"][a]]
    only_recomp = sorted(set(r["final"]) - set(o["final"]))
    only_oracle = sorted(set(o["final"]) - set(r["final"]))

    print(f"\ncommon addrs: {len(common)}   "
          f"divergent final value: {len(divergent)}   "
          f"recomp-only: {len(only_recomp)}   oracle-only: {len(only_oracle)}")

    # Earliest-diverging common addresses = root anchors. Sort by the frame
    # the recomp first touched them (where the bad computation likely began).
    divergent.sort(key=lambda a: (r["first"][a], a))
    print(f"\n=== earliest-diverging common addresses (top {args.top}) ===")
    print(f"{'addr':>10} {'recompF':>8} {'oracleF':>8}  {'oracleVal':>9} {'recompVal':>9}")
    for a in divergent[:args.top]:
        print(f"{fmt_addr(a):>10} {r['first'][a]:>8} {o['first'][a]:>8}  "
              f"{o['final'][a]:>#9x} {r['final'][a]:>#9x}")

    # Addresses the recomp writes that the oracle never does = likely runaway
    # writes (e.g. a corrupted pointer/counter scribbling). Show the earliest.
    if only_recomp:
        only_recomp.sort(key=lambda a: (r["first"][a], a))
        print(f"\n=== recomp-only writes (oracle never wrote these; top {args.top}) ===")
        print(f"{'addr':>10} {'recompF':>8} {'writes':>7} {'val':>6}")
        for a in only_recomp[:args.top]:
            print(f"{fmt_addr(a):>10} {r['first'][a]:>8} {r['writes'][a]:>7} {r['final'][a]:>#6x}")


if __name__ == "__main__":
    main()
