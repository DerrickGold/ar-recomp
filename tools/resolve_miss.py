#!/usr/bin/env python3
"""resolve_miss.py — automated dispatch-miss triage -> cfg patch proposals.

The mechanized DEBUG.md §1 registration decision tree. Feed it AR_TRACE_WATCH
anomaly dumps (saves/anom_*.jsonl) and/or saves/dump_dispatch_log.json; for
every missed target it runs the full hazard-class analysis and emits either a
ready-to-paste cfg line with evidence, or a DO-NOT verdict:

  1. construct-ret guard  — `ret:` of an active indirect_dispatch (B8C2 class:
     registering causes nested re-entry / stack overflow).
  2. paired-return guard  — JSR/JSL-return of a decoded paired C call site
     (§7.17 class: registering DOUBLE-EXECUTES the continuation).
  3. already-registered   — variant present (stale trace) or width missing.
  4. shape classification — disassemble from the target at the observed (m,x):
     single-shot to RTS/RTL, loop-continue (backward BRA/BRL), or suspect data.
  5. table evidence       — scan the bank for `target-1` words (PHA/RTS
     handler tables): siblings you probably need to register together.

Default is a dry-run report. --apply appends the SAFE `func` lines to
recomp/bankXX.cfg under a dated evidence comment (review with git diff, then
tools/regen.sh). AMBIGUOUS verdicts are never auto-applied.

Examples:
  tools/resolve_miss.py saves/anom_*.jsonl
  tools/resolve_miss.py saves/dump_dispatch_log.json --apply
"""
import argparse
import collections
import datetime
import glob
import json
import os
import sys

from ar_lib import (ROOT, load_rom, load_meta, fmt24, disasm,
                    paired_return_site, construct_ret_guard, FLOW_END)

MAX_WALK = 96


def collect_misses(paths):
    """-> {tgt_hex: {'srcs': Counter, 'mx': Counter, 'n': int}}"""
    out = collections.defaultdict(lambda: {'srcs': collections.Counter(),
                                           'mx': collections.Counter(), 'n': 0})
    for path in paths:
        if path.endswith('.json'):        # dump_dispatch_log.json
            j = json.load(open(path))
            for e in j.get('dispatch_log', {}).get('events', []):
                if e.get('found'):
                    continue
                t = e['pc24'].upper()
                mx = int(e.get('mx', 0))
                out[t]['srcs'][e.get('source_pc24', '??????').upper()] += 1
                out[t]['mx'][((mx >> 1) & 1, mx & 1)] += 1
                out[t]['n'] += 1
        else:                             # anom_*.jsonl (AR_TRACE watch dump)
            for line in open(path):
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if e.get('ch') != 'dispmiss':
                    continue
                t = e['to'].upper()
                out[t]['srcs'][e.get('from', '??????').upper()] += 1
                out[t]['mx'][(e.get('mnow'), e.get('xnow'))] += 1
                out[t]['n'] += 1
    return out


def classify_shape(rom, tgt_hex, m, x):
    """Walk from target; -> (kind, detail). kind in single-shot|loop-continue|suspect."""
    pc24 = int(tgt_hex, 16)
    a0 = pc24 & 0xFFFF
    last = None
    n = 0
    for ins in disasm(rom, pc24, m, x, count=MAX_WALK):
        n += 1
        last = ins
        if ins.mnem in FLOW_END:
            return ('single-shot',
                    f"runs {n} insn(s) to {ins.mnem} at {fmt24(ins.pc24)}")
        if ins.mnem in ('BRA', 'BRL') and ins.target is not None \
                and (ins.target & 0xFFFF) < a0:
            return ('loop-continue',
                    f"{ins.mnem} back to {fmt24(ins.target)} after {n} insn(s) "
                    f"(the $9D8E resume class)")
        if ins.mnem in ('JMP', 'JML'):
            return ('single-shot', f"tail {ins.text} after {n} insn(s)")
        if ins.mnem in ('STP', 'WDM', 'BRK') and n <= 3:
            return ('suspect', f"hits {ins.mnem} at insn {n} — target may be data")
    return ('suspect', f"no flow exit within {MAX_WALK} insns (last: {last.text if last else '?'})")


def table_evidence(rom, tgt_hex):
    """Handler-1 / exact words for the target inside its own bank."""
    bank, a = int(tgt_hex[:2], 16), int(tgt_hex[2:], 16)
    seg = rom[(bank & 0x7F) * 0x8000:(bank & 0x7F) * 0x8000 + 0x8000]
    hits = []
    for want, tag in (((a - 1) & 0xFFFF, 'handler-1'), (a, 'exact')):
        pat = bytes((want & 0xFF, want >> 8))
        j = 0
        while True:
            k = seg.find(pat, j)
            if k < 0:
                break
            hits.append((tag, (bank << 16) | (0x8000 + k)))
            j = k + 1
    return hits[:6]


def triage(tgt, info, rom, meta):
    """-> (verdict, lines, cfg_line|None). verdict: SAFE|DO-NOT|AMBIGUOUS|REGISTERED."""
    bank, addr = tgt[:2], tgt[2:]
    mx = info['mx'].most_common(1)[0][0] if info['mx'] else (0, 0)
    m, x = (mx[0] or 0), (mx[1] or 0)
    srcs = ', '.join(s for s, _ in info['srcs'].most_common(3))
    lines = [f"x{info['n']}  (m={m} x={x} at the miss; sources: {srcs})"]

    d = construct_ret_guard(meta, bank, addr)
    if d:
        lines.append(f"DO-NOT: `ret:` of ACTIVE construct (bank{d['bank']}.cfg:{d['line']}: "
                     f"{d['text']}) — B8C2 nested-reentry class; miss is benign.")
        return 'DO-NOT', lines, None

    pr = paired_return_site(tgt, rom)
    if pr:
        kind, site, callee = pr
        hosts = (meta or {}).get('labels', {}).get(tgt, [])
        known = meta and (callee in meta['functions'] or callee in meta.get('labels', {}))
        if hosts and known:
            lines.append(f"DO-NOT: {kind}-return of decoded `${bank}:{site:04X} {kind} "
                         f"${callee}` inside {hosts[0]} — paired-resume DOUBLE class "
                         f"(§7.17); miss is benign unwind-and-resume. Engine-level "
                         f"(ancestor-skip) if work is genuinely lost.")
            return 'DO-NOT', lines, None
        lines.append(f"WARN: preceding bytes decode as `{kind} ${callee}` at "
                     f"${bank}:{site:04X} — possible paired-return; verify site is code.")

    variants = (meta or {}).get('functions', {}).get(tgt)
    want = f"_M{m}X{x}"
    if variants and want in variants:
        lines.append(f"REGISTERED with {want} already — trace predates last regen, or benign.")
        return 'REGISTERED', lines, None

    kind, detail = classify_shape(rom, tgt, m, x)
    lines.append(f"shape: {kind} — {detail}")
    tbl = table_evidence(rom, tgt)
    if tbl:
        lines.append("table evidence: " + ', '.join(f"{t}@{fmt24(p)}" for t, p in tbl))

    cfg = f"func bank_{bank}_{addr} {addr} entry_mx:{m},{x}"
    if kind == 'suspect':
        lines.append(f"AMBIGUOUS — manual review before: {cfg}")
        return 'AMBIGUOUS', lines, None
    if variants:
        lines.append(f"width variant missing (has {', '.join(variants)}): {cfg}")
    else:
        lines.append(f"PROPOSE: {cfg}")
    return 'SAFE', lines, cfg


def apply_cfg(safe, evidence):
    today = datetime.date.today().isoformat()
    by_bank = collections.defaultdict(list)
    for tgt, cfg in safe:
        by_bank[tgt[:2]].append((tgt, cfg))
    for bank, items in sorted(by_bank.items()):
        path = os.path.join(ROOT, 'recomp', f'bank{bank.lower()}.cfg')
        if not os.path.exists(path):
            print(f"!! {path} missing — skipping {len(items)} line(s)", file=sys.stderr)
            continue
        with open(path, 'a') as f:
            f.write(f"\n# resolve_miss.py {today}: auto-triaged dispatch-miss targets.\n"
                    f"# Guards passed: not construct-ret (B8C2), not paired-return (§7.17).\n")
            for tgt, cfg in items:
                ev = '; '.join(evidence[tgt][:2])
                f.write(f"# ${tgt}: {ev}\n{cfg}\n")
        print(f"appended {len(items)} func line(s) to {path}")
    print("next: bash tools/regen.sh && cmake --build build -j8  (then re-test)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('inputs', nargs='+', help='anom_*.jsonl and/or dump_dispatch_log.json')
    ap.add_argument('--apply', action='store_true', help='append SAFE lines to recomp/*.cfg')
    ap.add_argument('--rom', help='ROM path')
    args = ap.parse_args()

    paths = [p for pat in args.inputs for p in sorted(glob.glob(pat))] or args.inputs
    rom = load_rom(args.rom)
    meta = load_meta()
    if not meta:
        print("note: no saves/gen_meta.json — guards degraded; run v2regen metadata",
              file=sys.stderr)

    misses = collect_misses(paths)
    if not misses:
        print("no dispatch-miss events in inputs")
        return

    order = sorted(misses.items(), key=lambda kv: -kv[1]['n'])
    safe, evidence = [], {}
    counts = collections.Counter()
    print(f"=== resolve_miss: {len(order)} distinct targets from {len(paths)} file(s) ===\n")
    for tgt, info in order:
        verdict, lines, cfg = triage(tgt, info, rom, meta)
        counts[verdict] += 1
        evidence[tgt] = [l for l in lines if not l.startswith('PROPOSE')]
        print(f"[{verdict}] {fmt24(int(tgt, 16))}")
        for l in lines:
            print(f"    {l}")
        print()
        if verdict == 'SAFE' and cfg:
            safe.append((tgt, cfg))

    print("=== summary:", ', '.join(f"{k}:{v}" for k, v in counts.items()), "===")
    if safe:
        print("\nPROPOSED CFG PATCH:")
        for _, cfg in safe:
            print(f"  {cfg}")
        if args.apply:
            apply_cfg(safe, evidence)
        else:
            print("(dry run — pass --apply to append to recomp/*.cfg)")


if __name__ == '__main__':
    sys.exit(main())
