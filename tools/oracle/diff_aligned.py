#!/usr/bin/env python3
"""Game-frame-aligned differential diff.

`diff_trace.py` compares cumulative *final* WRAM state, which can't say *when*
a divergence began: the two emulators don't share a host-frame clock (the
recomp batches boot into "frame 1"), so its frame numbers are misleading.

This tool aligns both traces by the game's OWN frame counter at $7E:0088/0089
(a 16-bit value the game increments once per logic frame, identically on both
sides for identical execution). It reconstructs each side's cumulative WRAM
state at the END of every game-frame, then walks game-frames in lockstep and
reports the FIRST game-frame at which a commonly-written address diverges —
i.e. the root, not the downstream symptom.

Only addresses BOTH sides have written by that game-frame are compared (avoids
power-on-RAM and boot-batching artifacts). The clock bytes $88/$89 are skipped.

Usage:
    diff_aligned.py oracle.jsonl recomp.jsonl [--top N] [--skip-zp]
                    [--lo 0xADDR --hi 0xADDR] [--from-gf G] [--to-gf G]
"""
import sys, json, argparse
from collections import defaultdict

CLOCK_LO, CLOCK_HI = 0x88, 0x89


def load_deltas(path):
    """Return (deltas, maxgf): deltas[gf] = {addr: last_val_written_during_gf}.

    Game-frame is tracked from writes to $7E:0088/0089. Writes are attributed
    to the game-frame current at the moment they occur (file order)."""
    deltas = defaultdict(dict)
    lo = hi = 0
    gf = 0
    maxgf = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            adr = int(r["adr"], 16)
            val = int(r["val"], 16)
            if adr == CLOCK_LO:
                lo = val
                gf = lo | (hi << 8)
            elif adr == CLOCK_HI:
                hi = val
                gf = lo | (hi << 8)
            else:
                deltas[gf][adr] = val
            if gf > maxgf:
                maxgf = gf
    return deltas, maxgf


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
    ap.add_argument("--lo", type=lambda x: int(x, 0), default=0,
                    help="only consider addresses >= this WRAM offset")
    ap.add_argument("--hi", type=lambda x: int(x, 0), default=0x1FFFF,
                    help="only consider addresses <= this WRAM offset")
    ap.add_argument("--from-gf", type=int, default=0)
    ap.add_argument("--to-gf", type=int, default=1 << 30)
    args = ap.parse_args()

    od, omax = load_deltas(args.oracle)
    rd, rmax = load_deltas(args.recomp)
    maxgf = min(max(omax, rmax), args.to_gf)
    print(f"oracle: max game-frame {omax}   recomp: max game-frame {rmax}")
    print(f"walking game-frames {args.from_gf}..{maxgf}\n")

    cumO, cumR = {}, {}
    first_div = {}      # addr -> (gf, oracleVal, recompVal)
    div_per_gf = defaultdict(int)

    def keep(a):
        if a in (CLOCK_LO, CLOCK_HI):
            return False
        if args.skip_zp and a <= 0x01FF:
            return False
        return args.lo <= a <= args.hi

    for gf in range(0, maxgf + 1):
        do = od.get(gf)
        dr = rd.get(gf)
        if do:
            cumO.update(do)
        if dr:
            cumR.update(dr)
        if gf < args.from_gf:
            continue
        # Only addresses that changed this gf on either side can newly diverge.
        touched = set()
        if do:
            touched |= do.keys()
        if dr:
            touched |= dr.keys()
        for a in touched:
            if a in first_div or not keep(a):
                continue
            if a in cumO and a in cumR and cumO[a] != cumR[a]:
                first_div[a] = (gf, cumO[a], cumR[a])
                div_per_gf[gf] += 1

    if not first_div:
        print("no divergence found over the compared addresses/range.")
        return

    # Earliest game-frame where divergence first appears.
    rows = sorted(first_div.items(), key=lambda kv: (kv[1][0], kv[0]))
    earliest_gf = rows[0][1][0]
    print(f"=== first divergence at game-frame {earliest_gf} "
          f"({div_per_gf[earliest_gf]} addr(s) that gf) ===")
    print(f"{'addr':>10} {'gf':>7} {'oracleVal':>9} {'recompVal':>9}")
    for a, (gf, ov, rv) in rows[:args.top]:
        print(f"{fmt_addr(a):>10} {gf:>7} {ov:>#9x} {rv:>#9x}")

    print(f"\n=== divergence onset by game-frame (first {args.top} gf) ===")
    for gf in sorted(div_per_gf)[:args.top]:
        print(f"  gf {gf:>6}: {div_per_gf[gf]:>5} new diverging addr(s)")


if __name__ == "__main__":
    main()
