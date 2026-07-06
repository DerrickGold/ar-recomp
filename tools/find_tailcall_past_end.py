#!/usr/bin/env python3
"""Static census of "tail-call past end" sites in the emitted gen — the
fingerprint of a mis-read pushed-continuation dispatch (the RTS-trick class).

When the decoder runs off a function's end into another function it emits a
`tail-call past end: into <fn>` exit. Most are benign (a function genuinely
falls through). The DANGEROUS subset is a **dispatch table** whose cases each do
`LDY #ret; PHY; BRL <handler>` (push a return address, branch to a shared
handler that RTSes back) — the decoder turns those BRLs into tail-calls and
LOSES the return-to-continuation, so the handler's RTS host-unwinds and skips
whatever SEP/REP the continuation would run. That was the lair-seal `$9D4D`→
`$A4F7` bug (7 tail-calls to the same target; RTS-to-$9D8E missed → m-leak).

Signal ranking:
  * A source function with MANY tail-calls to the SAME target = a dispatch table
    → the handler is reached with a pushed return the recomp doesn't honour.
  * Cross-check `--dispmiss <trace.jsonl>`: any tail-call target that ALSO shows
    up as a runtime dispatch-miss source is a confirmed RTS-trick — its RTS lands
    on an unregistered continuation. Register that continuation as a cfg `func`.

Usage:
  find_tailcall_past_end.py                       # census, ranked
  find_tailcall_past_end.py --min 2               # only sources w/ >=2 to one target
  find_tailcall_past_end.py --dispmiss seal.jsonl # cross-ref runtime misses
"""
import argparse, glob, json, os, re, collections

GEN = os.path.join(os.path.dirname(__file__), '..', 'src', 'gen')
FUNC_RE = re.compile(r'^RecompReturn (bank_[0-9A-Fa-f]{2}_[0-9A-Fa-f]+_M\dX\d)\(CpuState \*cpu\) \{')
TAIL_RE = re.compile(r'tail-call past end: into (bank_[0-9A-Fa-f]{2}_[0-9A-Fa-f]+_M\dX\d) at \$([0-9A-Fa-f]+)')

def census():
    # source_func -> Counter(target_func)
    by_src = collections.defaultdict(collections.Counter)
    tgt_addr = {}
    for path in glob.glob(os.path.join(GEN, '*.c')):
        cur = None
        for line in open(path):
            m = FUNC_RE.match(line)
            if m:
                cur = m.group(1); continue
            t = TAIL_RE.search(line)
            if t and cur:
                by_src[cur][t.group(1)] += 1
                tgt_addr[t.group(1)] = t.group(2)
    return by_src, tgt_addr

def load_dispmiss_targets(path):
    tset = collections.Counter()
    for line in open(path):
        line = line.strip()
        if not line: continue
        try: e = json.loads(line)
        except json.JSONDecodeError: continue
        if e.get('ch') == 'dispmiss':
            tset[e['to']] += 1   # 6-hex e.g. '039D8E'
    return tset

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--min', type=int, default=2,
                    help='min tail-calls from one source to one target to flag (default 2)')
    ap.add_argument('--dispmiss', help='a trace .jsonl; cross-ref runtime dispatch-miss targets')
    ap.add_argument('--all', action='store_true', help='list every site, not just the ranked suspects')
    args = ap.parse_args()

    by_src, tgt_addr = census()
    total = sum(sum(c.values()) for c in by_src.values())
    print(f"{total} tail-call-past-end sites across {len(by_src)} source functions.\n")

    # Rank: a (source -> target) pair with count >= --min is a dispatch-table suspect.
    suspects = []
    for src, cnts in by_src.items():
        for tgt, n in cnts.items():
            if n >= args.min or args.all:
                suspects.append((n, src, tgt))
    suspects.sort(reverse=True)

    dm = load_dispmiss_targets(args.dispmiss) if args.dispmiss else None

    # Confirmation: a suspect is a real RTS-trick if a runtime dispatch-miss TARGET
    # falls inside the SOURCE dispatcher's address range (the RTS lands on the
    # dispatcher's own continuation). Match on bank + the miss target's addr being
    # >= the source's entry addr (cheap proxy — the continuation is just past the
    # push site inside the dispatcher).
    def src_bank_addr(name):
        m = re.match(r'bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]+)_', name)
        return (m.group(1).upper(), int(m.group(2), 16)) if m else (None, None)
    miss_by_bank = collections.defaultdict(list)  # bank -> [addr]
    if dm:
        for t in dm:
            miss_by_bank[t[:2].upper()].append(int(t[2:], 16))

    print(f"=== dispatch-table suspects (>= {args.min} tail-calls to one target) ===")
    for n, src, tgt in suspects[:60]:
        addr = tgt_addr.get(tgt, '?')
        mark = ""
        if dm:
            sbank, saddr = src_bank_addr(src)
            near = [a for a in miss_by_bank.get(sbank, []) if saddr <= a <= saddr + 0x400]
            if near:
                mark = f"   <== CONFIRMED: dispatch-miss to ${sbank}{near[0]:04X} (register `func`)"
        print(f"  {n:>3}x  {src}  ->  {tgt} (${addr}){mark}")

    if dm:
        print(f"\n=== runtime dispatch-miss targets (from {os.path.basename(args.dispmiss)}) ===")
        print("  (register any that land inside a function as `func bank_BB_TTTT TTTT entry_mx:m,x`)")
        for t, n in dm.most_common(30):
            print(f"  {n:>3}x  -> {t}")

if __name__ == '__main__':
    main()
