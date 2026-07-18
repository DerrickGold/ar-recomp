#!/usr/bin/env python3
"""Slice/pivot an ar_trace unified JSONL trace (AR_TRACE=<file>).

One capture run holds every layer (func entries w/ m/x, all VRAM write paths,
VMADD sets, PPU regs, DMA), correlated by a monotonic `seq`. Query it locally
instead of doing many small targeted game runs.

Examples:
  trace_slice.py seal.jsonl --summary
  trace_slice.py seal.jsonl --misdecodes           # live m/x misdecodes
  trace_slice.py seal.jsonl --vram 0000-00ff       # who wrote this VRAM range
  trace_slice.py seal.jsonl --vmadd                # VMADD ($2116) sets + func
  trace_slice.py seal.jsonl --fn 8053 --ch func,vmadd
  trace_slice.py seal.jsonl --around 12345 --window 20
"""
import argparse, json, sys, collections

def load(path):
    out = []
    for line in open(path):
        line = line.strip()
        if line:
            try: out.append(json.loads(line))
            except json.JSONDecodeError: pass
    return out

def parse_range(s):
    if s is None: return None
    if '-' in s:
        a, b = s.split('-', 1); return (int(a, 16), int(b, 16))
    v = int(s, 16); return (v, v)

def fmt(e):
    st = ""
    if 'mnow' in e:
        st = f" m={e['mnow']} S=${e.get('S','?')} DB=${e.get('DB','?')}"
    base = f"seq={e['seq']:>7} hf={e['hf']} gf={e['gf']} {e['ch']:<8}{st} fn={e['fn']}"
    ch = e['ch']
    if ch == 'func':
        tag = "  <<< MISDECODE" if e.get('misdecode') else ""
        return f"{base} pc={e['pc']} m={e['m']} x={e['x']} (exp m={e['em']} x={e['ex']}){tag}"
    if ch == 'call':
        tag = "  <<< M/X LEAK (runtime != decoder-expected)" if e.get('leak') else ""
        return f"{base} site={e['site']} m={e['m']} x={e['x']} (exp m={e['em']} x={e['ex']}){tag}"
    if ch == 'vram':
        return f"{base} va=${e['va']} val=${e['val']} path={e['path']}"
    if ch == 'vmadd':
        return f"{base} VMADD=${e['vmadd']} ({e['how']})"
    if ch == 'reg':
        return f"{base} reg=${e['reg']} val=${e['val']}"
    if ch == 'dma':
        return f"{base} ch{e['dch']} bAdr=${e['bAdr']} src=${e['src']} size={e['size']} fromB={e['fromB']}"
    if ch == 'dispmiss':
        return f"{base} DISPATCH-MISS {e['from']} -> {e['to']} (RTS-trick/unregistered target)"
    if ch == 'garbage':
        return f"{base} GARBAGE-VARIANT {e['variant']} @ {e['pc']} m={e['m']} x={e['x']}"
    if ch == 'wram':
        return f"{base} $7E:{e['off']} {e['old']}->{e['val']} (w{e['w']})"
    if ch == 'stack':
        return f"{base} PUSH ${e['off']} = {e['val']} (was {e['old']}, w{e['w']})"
    if ch == 'hwread':
        return f"{base} HWREAD ${e['reg']} = ${e['val']}"
    if ch == 'ppumem':
        return f"{base} {e['mem'].upper()}[${e['addr']}] = ${e['val']}"
    if ch == 'frame':
        return f"{base} ---- {e['what'].upper()} ----"
    return base

def load_meta(explicit=None):
    """gen_meta.json sidecar (`v2regen metadata`) — static decode facts."""
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    cands = ([explicit] if explicit else []) + [
        os.path.join(here, '..', 'saves', 'gen_meta.json'),
        'saves/gen_meta.json',
    ]
    for c in cands:
        if c and os.path.exists(c):
            return json.load(open(c))
    return None


def _paired_return_site(tgt):
    """Canonical guard lives in ar_lib (shared with resolve_miss.py)."""
    try:
        from ar_lib import paired_return_site, load_rom
        return paired_return_site(tgt, load_rom())
    except (ImportError, SystemExit):
        return None   # no ROM / lib -> guard degrades, suggestions still print


def _meta_note(meta, tgt, m, x, sources=()):
    """Static-fact annotation + suggested cfg line for a dispmiss target."""
    bank, addr = tgt[:2], tgt[2:]
    suggest = f"func bank_{bank}_{addr} {addr} entry_mx:{m},{x}"
    if not meta:
        return [f"SUGGESTED FIX:  {suggest}   (no gen_meta.json — run v2regen metadata to verify)"]
    out = []
    # ── paired-resume-double guard (3rd hazard class, DEBUG.md §7.17 2026-07-07) ──
    # If the target is the RETURN address of a decoded JSR/JSL (site+3 / site+4)
    # with live C fall-through (it's a label inside a decoded function), the miss
    # is the engine's BENIGN unwind-and-resume: the paired call site restores S
    # and falls through, running the continuation exactly once. Registering it
    # runs the continuation TWICE (nested dispatch + natural resume) — this
    # double-executed the sim actor tick (enemies 2x) before being caught.
    pr = _paired_return_site(tgt)
    if pr:
        kind, site, callee = pr
        callee_disp = f"{callee[:2]}:{callee[2:]}" if kind == 'JSL' else callee[2:]
        hosts_pr = meta['labels'].get(tgt, [])
        callee_known = (callee in meta['functions'] or callee in meta['labels'])
        if hosts_pr and callee_known:
            return [f"DO NOT REGISTER: ${addr} is the {kind}-return of the decoded call "
                    f"`${bank}:{site:04X} {kind} ${callee_disp}` inside "
                    f"{hosts_pr[0]} — a PAIRED host-C call site. The miss is the "
                    f"engine's benign unwind-and-resume (single execution); registering "
                    f"it DOUBLE-EXECUTES the continuation (nested dispatch + natural "
                    f"fall-through; the paired-resume-double class, DEBUG.md §7.17 ⚠️). "
                    f"If work is genuinely lost here, the fix is engine-level "
                    f"(ancestor-skip), not a cfg reg."]
        out.append(f"CAUTION: byte before target decodes as `{kind} ${callee_disp}` "
                   f"(site ${bank}:{site:04X}) — if that is a real decoded call, this is "
                   f"the paired-resume-double class (DEBUG.md §7.17): registering "
                   f"double-executes the continuation. Verify the site is code, not data, "
                   f"before registering.")
    # ── mid-loop-continuation guard (the B8C2 stack-overflow class) ──
    # If the target is the `ret:` of a cfg indirect_dispatch, the miss is the
    # construct working via benign host-unwind; registering it makes the
    # handler-RTS re-ENTER the live construct nested per record -> overflow.
    for d in meta.get('cfg', {}).get('indirect_dispatch', []):
        if f"RET:{addr.upper()}" in d['text'].upper() and d['bank'] == bank:
            return [f"DO NOT REGISTER: ${addr} is the `ret:` continuation of an ACTIVE "
                    f"dispatch construct (bank{d['bank']}.cfg:{d['line']}: {d['text']}). "
                    f"The miss is the construct's benign host-unwind — registering it "
                    f"causes nested re-entry per record (stack overflow; DEBUG.md §1 ⚠️). "
                    f"If m/x at the miss ({m},{x}) matches the loop width, nothing leaks."]
    # Heuristic for the same trap without a cfg marker: every miss source sits
    # inside the target's own web (within ±$200) → likely a mid-loop return.
    near_srcs = [s for s in sources
                 if s[:2] == bank and abs(int(s[2:], 16) - int(addr, 16)) <= 0x200]
    if sources and len(near_srcs) == len(sources):
        out.append(f"CAUTION: all miss sources sit within ±$200 of the target — "
                   f"possible mid-loop continuation (the B8C2 class). Verify the target is "
                   f"single-shot (runs to its own RTS / plain trampoline) BEFORE registering; "
                   f"see DEBUG.md §1 ⚠️.")
    variants = meta['functions'].get(tgt)
    want = f"_M{m}X{x}"
    if variants:
        if want in variants:
            out.append(f"ALREADY REGISTERED as func with variant {want} — this miss should be "
                       f"impossible with the CURRENT gen; the trace likely predates the last "
                       f"regen, OR the miss is benign (post-fix unwind at a caller continuation).")
        else:
            out.append(f"registered func but variant {want} MISSING (has: {', '.join(variants)}) "
                       f"→ width mismatch; verify the continuation's SEP/REP and add the variant:")
            out.append(f"SUGGESTED FIX:  {suggest}")
    else:
        out.append(f"NOT registered → unregistered RTS-trick continuation (the $9D8E class).")
        out.append(f"SUGGESTED FIX:  {suggest}")
    hosts = meta['labels'].get(tgt, [])
    if hosts:
        out.append(f"target is a local label inside: {', '.join(hosts[:4])}"
                   + (" …" if len(hosts) > 4 else "")
                   + "  (the continuation of that function's dispatch web)")
    return out


def diagnose(ev, meta=None):
    """Correlate the root-event channels into a ranked hypothesis.

    The m-leak decision tree, mechanized (DEBUG.md §1). Two independent root
    classes, both handled first-class:
      (a) dispatch-miss (with or without a visible leak) — an RTS-trick
          continuation the recomp can't resume → register a cfg `func`. The
          stderr tripwire HIDES this class when S<$0200 (the $9D8E bug).
      (b) m/x leak with NO nearby miss — ambiguous decoded exit → `exit_mx_at`
          (verify by decoding the leaked region from ROM at the expected m:
          garbage=exit-mx, coherent=keep digging).
    Static facts come from the gen_meta.json sidecar when present.
    """
    leaks = [e for e in ev if e['ch'] == 'call' and e.get('leak')
             and 'Handler' not in e.get('fn', '')]
    dm    = [e for e in ev if e['ch'] == 'dispmiss']
    gb    = [e for e in ev if e['ch'] == 'garbage']
    print(f"=== DIAGNOSE ({len(ev)} events, hf {ev[0]['hf']}..{ev[-1]['hf']}) ===")
    print(f"  m/x leaks: {len(leaks)} | dispatch-misses: {len(dm)} | garbage-variants: {len(gb)}"
          + ("" if meta else "   [no gen_meta.json — run v2regen metadata for static joins]")
          + "\n")

    if not leaks and not dm and not gb:
        print("  No leak/dispmiss/garbage signals → NOT the misdecode/dispatch class.")
        print("  Read the symptom's own channel (vram/wram/ppumem/hwread) or §11 silent-drops.")
        return

    ranked = []  # (score, text)
    end_seq = ev[-1]['seq']

    # ── (a) dispatch-misses, aggregated per target — first-class, no leak needed ──
    by_tgt = collections.defaultdict(list)
    for d in dm:
        by_tgt[d['to']].append(d)
    for tgt, hits in by_tgt.items():
        n = len(hits)
        first, last = hits[0], hits[-1]
        # modal runtime (m,x) at the miss = the width the continuation runs at
        mx = collections.Counter((h.get('mnow'), h.get('xnow')) for h in hits).most_common(1)[0][0]
        m, x = mx if mx != (None, None) else ('?', '?')
        hidden = any(int(h.get('S', 'FFFF'), 16) < 0x200 for h in hits)
        near_end = (end_seq - last['seq']) < max(50, len(ev) // 20)
        srcs = sorted({h['from'] for h in hits})
        score = 40 + min(n, 40) + (50 if hidden else 0) + (30 if near_end else 0)
        lines = [
            f"DISPATCH-MISS target {tgt}  x{n}  (m={m} x={x} at the miss; "
            f"sources: {', '.join(srcs[:3])}{' …' if len(srcs) > 3 else ''})"
            + ("   <-- HIDDEN by stderr tripwire (S<$0200)" if hidden else "")
            + ("   <-- LAST events before capture end (watchdog/crash proximity)" if near_end else ""),
        ]
        lines += [f"    {t}" for t in _meta_note(meta, tgt, m, x, sources=srcs)]
        ranked.append((score, "\n  ".join(lines)))

    # ── leak ↔ nearest-preceding-miss pairing (correlation strengthens (a)) ──
    for lk in leaks:
        prior = [d for d in dm if d['seq'] < lk['seq']]
        near = prior[-1] if prior else None
        if near:
            score = 100 - min(99, lk['seq'] - near['seq'])
            ranked.append((score,
                f"LEAK↔MISS correlation: leak at {lk['fn']} site ${lk['site']} "
                f"(runtime m={lk['m']} != expected m={lk['em']}) follows dispatch-miss "
                f"{near['from']} -> {near['to']} by {lk['seq']-near['seq']} events — "
                f"the miss above is the root; fix that target first."))
        else:
            # ── (b) leak with no miss → ambiguous decoded exit candidate ──
            ranked.append((25,
                f"Ambiguous decoded exit (candidate for exit_mx_at):\n"
                f"    leak at {lk['fn']} site ${lk['site']} (runtime m={lk['m']} != expected m={lk['em']})\n"
                f"    NO dispatch-miss precedes it → the callee likely has two static exits at\n"
                f"    different m. DECODE the leaked region from ROM at the EXPECTED m first:\n"
                f"    garbage/BRK → exit_mx_at <callee> <m> <x>;  coherent → NOT exit-mx, keep digging."))
    for g in gb:
        ranked.append((30,
            f"Entered a known-garbage variant: {g['variant']} @ {g['pc']} (m={g['m']} x={g['x']})\n"
            f"    → a flag leaked BEFORE this dispatch; walk back to the last clean call site."))

    ranked.sort(key=lambda t: -t[0])
    for i, (_, txt) in enumerate(ranked[:10], 1):
        print(f"  [{i}] {txt}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--misdecodes', action='store_true', help='func entries where runtime m/x != expected')
    ap.add_argument('--leaks', action='store_true', help='call sites where runtime m/x != decoder expectation (the m-leak boundary)')
    ap.add_argument('--diagnose', action='store_true', help='ranked root-cause hypothesis correlating leaks x dispmiss x garbage, with a suggested cfg fix')
    ap.add_argument('--meta', help='path to gen_meta.json (default: saves/gen_meta.json if present)')
    ap.add_argument('--vram', help='VRAM word-addr or lo-hi range (hex), show writers')
    ap.add_argument('--wram', help='WRAM g_ram-offset or lo-hi range (hex), show writers (incl. indirect + DMA-adjacent)')
    ap.add_argument('--vmadd', action='store_true', help='show VMADD ($2116) sets')
    ap.add_argument('--ch', help='comma channels to include (func,vram,vmadd,reg,dma)')
    ap.add_argument('--fn', help='only events whose fn contains this substring')
    ap.add_argument('--around', type=int, help='center on this seq')
    ap.add_argument('--window', type=int, default=15, help='+/- events for --around')
    ap.add_argument('--limit', type=int, default=200)
    args = ap.parse_args()
    ev = load(args.file)

    if args.summary:
        chc = collections.Counter(e['ch'] for e in ev)
        print(f"{len(ev)} events, hf {ev[0]['hf']}..{ev[-1]['hf']}, seq {ev[0]['seq']}..{ev[-1]['seq']}")
        for k, v in chc.most_common(): print(f"  {k:<6} {v}")
        mis = [e for e in ev if e['ch'] == 'func' and e.get('misdecode')]
        print(f"  entry-misdecodes: {len(mis)}"
              + (f"  (first: {mis[0]['fn']} @ {mis[0]['pc']})" if mis else ""))
        leaks = [e for e in ev if e['ch'] == 'call' and e.get('leak')]
        print(f"  M/X LEAKS (call-site): {len(leaks)}"
              + (f"  (first: {leaks[0]['fn']} @ site {leaks[0]['site']})" if leaks else ""))
        if leaks:
            bysite = collections.Counter((e['fn'], e['site']) for e in leaks)
            for (fn, site), n in bysite.most_common(8):
                print(f"      {fn} site ${site}  x{n}")
        dm = [e for e in ev if e['ch'] == 'dispmiss']
        gb = [e for e in ev if e['ch'] == 'garbage']
        if dm:
            hidden = [e for e in dm if 'S' in e and int(e['S'], 16) < 0x200]
            print(f"  DISPATCH-MISSES: {len(dm)}  (first: {dm[0]['from']} -> {dm[0]['to']})")
            if hidden:
                # the class the stderr tripwire (gated S>=$0200) HIDES — the $9D8E bug
                print(f"    !! {len(hidden)} with S<$0200 — HIDDEN by the stderr tripwire; "
                      f"prime RTS-trick-continuation suspects:")
                for e in dict.fromkeys((h['to'] for h in hidden)):
                    print(f"       -> {e}")
        if gb: print(f"  GARBAGE-VARIANTS: {len(gb)}  (first: {gb[0]['variant']})")
        print("  (run --diagnose for a ranked root-cause hypothesis + suggested cfg fix)")
        return

    if args.diagnose:
        diagnose(ev, load_meta(args.meta)); return

    sel = ev
    if args.ch:
        chs = set(args.ch.split(',')); sel = [e for e in sel if e['ch'] in chs]
    if args.fn:
        sel = [e for e in sel if args.fn in e['fn']]
    if args.misdecodes:
        sel = [e for e in sel if e['ch'] == 'func' and e.get('misdecode')]
    if args.leaks:
        sel = [e for e in sel if e['ch'] == 'call' and e.get('leak')]
    if args.vmadd:
        sel = [e for e in sel if e['ch'] == 'vmadd']
    if args.vram:
        lo, hi = parse_range(args.vram)
        sel = [e for e in sel if e['ch'] == 'vram' and lo <= int(e['va'], 16) <= hi]
    if args.wram:
        lo, hi = parse_range(args.wram)
        sel = [e for e in sel if e['ch'] in ('wram', 'stack') and lo <= int(e['off'], 16) <= hi]
    if args.around is not None:
        sel = [e for e in ev if abs(e['seq'] - args.around) <= args.window]

    for e in sel[:args.limit]:
        print(fmt(e))
    if len(sel) > args.limit:
        print(f"... ({len(sel)-args.limit} more; raise --limit)")

if __name__ == '__main__':
    main()
