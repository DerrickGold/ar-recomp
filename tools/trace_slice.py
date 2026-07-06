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

def diagnose(ev):
    """Correlate the root-event channels into a ranked hypothesis.

    The m-leak decision tree, mechanized: an m/x LEAK (call site where runtime
    m/x != decoder-expected) is a SYMPTOM. Its cause is usually one of:
      (a) an unregistered RTS-trick continuation — a `dispmiss` (host-unwind)
          just before the leak whose target lands inside a function → the RTS
          skipped a SEP/REP → register the target as a cfg `func`. The stderr
          tripwire HIDES this class when S<$0200 (the $9D8E lair-seal bug).
      (b) an ambiguous decoded exit — no dispmiss near the leak → the callee has
          two static exits at different m → `exit_mx_at`. (Verify by decoding the
          leaked region from ROM at the expected m: garbage=exit-mx, coherent=other.)
    """
    # NMI/IRQ handler entries trip the leak flag benignly (interrupt wrappers
    # run with a fixed x that differs from the interrupted variant) — filter them.
    leaks = [e for e in ev if e['ch'] == 'call' and e.get('leak')
             and 'Handler' not in e.get('fn', '')]
    dm    = [e for e in ev if e['ch'] == 'dispmiss']
    gb    = [e for e in ev if e['ch'] == 'garbage']
    print(f"=== DIAGNOSE ({len(ev)} events, hf {ev[0]['hf']}..{ev[-1]['hf']}) ===")
    print(f"  m/x leaks: {len(leaks)} | dispatch-misses: {len(dm)} | garbage-variants: {len(gb)}\n")

    if not leaks and not dm and not gb:
        print("  No leak/dispmiss/garbage signals → NOT the misdecode/dispatch class.")
        print("  Read the symptom's own channel (vram/wram/ppumem/hwread) or §11 silent-drops.")
        return

    ranked = []  # (score, text)
    # Pair each leak with the nearest PRECEDING dispmiss (the likely root event).
    for lk in leaks:
        prior = [d for d in dm if d['seq'] < lk['seq']]
        near = prior[-1] if prior else None
        if near:
            tgt = near['to']; bank = tgt[:2]; addr = tgt[2:]
            s = int(near.get('S', 'FFFF'), 16)
            hidden = s < 0x200
            m = near.get('mnow', near.get('m', '?')); x = near.get('xnow', near.get('x', '?'))
            score = 100 - (lk['seq'] - near['seq'])  # closer = stronger
            if hidden: score += 50
            ranked.append((score,
                f"RTS-trick continuation (register a cfg func):\n"
                f"    leak at {lk['fn']} site ${lk['site']} (runtime m={lk['m']} != expected m={lk['em']})\n"
                f"    caused by dispatch-miss  {near['from']} -> {tgt}  (S=${near.get('S','?')}"
                + ("  <-- HIDDEN by stderr tripwire (S<$0200)" if hidden else "") + ")\n"
                f"    SUGGESTED FIX:  func bank_{bank}_{addr} {addr} entry_mx:{m},{x}\n"
                f"    (verify: the RTS target ${tgt} should resume its function's loop/REP;\n"
                f"     confirm the continuation runs at m={m},x={x} from the surrounding SEP/REP)"))
        else:
            ranked.append((10,
                f"Ambiguous decoded exit (candidate for exit_mx_at):\n"
                f"    leak at {lk['fn']} site ${lk['site']} (runtime m={lk['m']} != expected m={lk['em']})\n"
                f"    NO dispatch-miss precedes it → the callee likely has two static exits at\n"
                f"    different m. DECODE the leaked region from ROM at the EXPECTED m first:\n"
                f"    garbage/BRK → exit_mx_at <callee> <m> <x>;  coherent → keep digging."))
    for g in gb:
        ranked.append((30,
            f"Entered a known-garbage variant: {g['variant']} @ {g['pc']} (m={g['m']} x={g['x']})\n"
            f"    → a flag leaked BEFORE this dispatch; walk back to the last clean call site."))

    ranked.sort(key=lambda t: -t[0])
    for i, (_, txt) in enumerate(ranked[:8], 1):
        print(f"  [{i}] {txt}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--misdecodes', action='store_true', help='func entries where runtime m/x != expected')
    ap.add_argument('--leaks', action='store_true', help='call sites where runtime m/x != decoder expectation (the m-leak boundary)')
    ap.add_argument('--diagnose', action='store_true', help='ranked root-cause hypothesis correlating leaks x dispmiss x garbage, with a suggested cfg fix')
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
        diagnose(ev); return

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
