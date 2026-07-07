#!/usr/bin/env python3
"""Static census of the pushed-continuation RTS-dispatch idiom.

ActRaiser's engine (esp. the bank-03 town/scene code) nests this idiom
arbitrarily deep, and every layer we root-caused (DEBUG.md s7.13: the $9390
family, the $9ED3 build-step web, the $9CFB scanner) was discovered one
in-game repro + regen at a time. The idiom is byte-stereotyped, so this tool
finds the WHOLE class statically:

  continuation push:  A9 lo hi 48   (m=0: LDA #imm16 ; PHA)
                      A0 lo hi 5A   (x=0: LDY #imm16 ; PHY)
                      where imm16+1 is a plausible in-bank code address.
                      The pushed word is popped by some later RTS -> imm16+1
                      MUST be reachable as an rts_dispatch target (or an
                      indirect_dispatch ret:), or the runtime silently
                      unwinds mid-loop (the m-leak / skipped-subsystem class).

  dispatch site:      48 60         (PHA ; RTS) -- the jump itself.

For each bank it reports every hit, whether the continuation target is
covered by the bank cfg (rts_dispatch target lists, indirect_dispatch ret:,
or a func entry at target), and every PHA;RTS site with cfg coverage
(rts_dispatch/indirect_dispatch on the RTS pc).

False positives exist (data bytes that happen to match); each hit prints a
small decode preview so a human can triage. Err on the side of listing —
an uncovered TRUE hit costs a debugging session (see s7.13's three rounds).

Usage: python3 tools/find_rts_webs.py [--bank NN] [rom]
"""
import sys, re, glob, os

args = [a for a in sys.argv[1:]]
bank_filter = None
if '--bank' in args:
    i = args.index('--bank'); bank_filter = int(args[i+1], 16); del args[i:i+2]
# --suggest: for each UNCOVERED continuation push, emit a shape-classified cfg
# candidate line (the "closure loop" driver: regen -> census --suggest ->
# append accepted lines -> regen ... until the delta is empty). Classification:
#   * target == `ret:` of a cfg indirect_dispatch  -> DO NOT REGISTER (B8C2
#     class: the miss is the live construct's benign unwind; registering it
#     nests per record -> stack overflow).
#   * otherwise -> suggest `func ... entry_mx:m,0` with m from whichever width
#     decodes coherently at the continuation (plausible() tracks SEP/REP).
#     STILL a human decision — verify single-shot shape per DEBUG.md §1 ⚠️.
suggest = '--suggest' in args
if suggest: args.remove('--suggest')
ROM = args[0] if args else 'ar.sfc'
rom = open(ROM, 'rb').read()
NBANKS = len(rom) // 0x8000

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..',
                                'third_party', 'snesrecomp', 'recompiler'))
try:
    from snes65816 import decode_insn
except ImportError:
    decode_insn = None

def off(bank, pc): return (bank << 15) | (pc & 0x7fff)

def plausible(bank, pc, m, x, n=6):
    """Count coherently-decodable insns from pc; 0 if immediately garbage."""
    if decode_insn is None: return -1
    cnt = 0
    for _ in range(n):
        o = off(bank, pc)
        if o + 4 > len(rom): return cnt
        ins = decode_insn(rom, o, pc, bank, m, x)
        if ins is None or ins.mnem in ('BRK', 'COP'): return cnt
        if ins.mnem == 'SEP' and rom[o+1] & 0x20: m = 1
        if ins.mnem == 'REP' and rom[o+1] & 0x20: m = 0
        cnt += 1
        if ins.mnem in ('RTS', 'RTL', 'JMP', 'BRA', 'BRL', 'JML'): return cnt
        pc = (pc + ins.length) & 0xffff
    return cnt

def load_cfg_coverage():
    """Per-bank sets: rts_dispatch sites, all continuation/handler targets,
    indirect_dispatch ret: pcs, func entry pcs."""
    cov = {}
    for path in glob.glob('recomp/bank*.cfg'):
        m = re.search(r'bank([0-9a-f]{2})\.cfg', path)
        if not m: continue
        b = int(m.group(1), 16)
        sites, targets, rets, funcs = set(), set(), set(), set()
        for line in open(path):
            line = line.split('#')[0].strip()
            if line.startswith('rts_dispatch'):
                parts = line.split()
                sites.add(int(parts[1], 16))
                targets.update(int(p, 16) for p in parts[2:])
            elif line.startswith('indirect_dispatch'):
                parts = line.split()
                sites.add(int(parts[1], 16))
                for p in parts[2:]:
                    if p.startswith('ret:'): rets.add(int(p[4:], 16))
            elif line.startswith('func '):
                parts = line.split()
                for p in parts[1:]:
                    if re.fullmatch(r'[0-9A-Fa-f]{4}', p):
                        funcs.add(int(p, 16)); break
        cov[b] = (sites, targets, rets, funcs)
    return cov

cov = load_cfg_coverage()
banks = [bank_filter] if bank_filter is not None else range(NBANKS)

total_unc_push, total_unc_site = 0, 0
for b in banks:
    sites, targets, rets, funcs = cov.get(b, (set(), set(), set(), set()))
    pushes, dsites = [], []
    base = b << 15
    for o in range(base, base + 0x8000 - 4):
        pc = (o & 0x7fff) | 0x8000
        b0, b1, b2, b3 = rom[o], rom[o+1], rom[o+2], rom[o+3]
        # (PEA F4 was tried as a third push idiom 2026-07-06 and reverted:
        # F4 is too common as a data byte — 47 -> 935 UNC false positives.
        # Every web found so far pushes via LDA#/PHA or LDY#/PHY.)
        if (b0 == 0xA9 and b3 == 0x48) or (b0 == 0xA0 and b3 == 0x5A):
            imm = b1 | (b2 << 8)
            tgt = (imm + 1) & 0xffff
            if not (0x8000 <= tgt < 0xfff0): continue
            # continuation target should decode as code in at least one width
            sc = max(plausible(b, tgt, 0, 0), plausible(b, tgt, 1, 0))
            if sc < 2: continue
            covered = tgt in targets or tgt in rets or tgt in funcs
            pushes.append((pc, imm, tgt, sc, covered))
        if b0 == 0x48 and b1 == 0x60:
            rts_pc = (pc + 1) & 0xffff
            covered = rts_pc in sites
            # require the bytes BEFORE the PHA to look like code loading A
            dsites.append((rts_pc, covered))
    unc_p = [p for p in pushes if not p[4]]
    unc_s = [s for s in dsites if not s[1]]
    if not pushes and not dsites: continue
    if bank_filter is None and not unc_p and not unc_s: continue
    print(f"== bank {b:02x}: {len(pushes)} continuation-pushes "
          f"({len(unc_p)} UNCOVERED), {len(dsites)} PHA;RTS sites "
          f"({len(unc_s)} uncovered)")
    for pc, imm, tgt, sc, covered in pushes:
        if covered and bank_filter is None: continue
        mark = 'ok ' if covered else 'UNC'
        print(f"   [{mark}] push @{b:02x}:{pc:04x}  #${imm:04X} -> cont "
              f"${tgt:04X} (decode-score {sc})")
    for rts_pc, covered in dsites:
        if covered and bank_filter is None: continue
        mark = 'ok ' if covered else 'UNC'
        print(f"   [{mark}] PHA;RTS dispatch @{b:02x}:{rts_pc:04x}")
    total_unc_push += len(unc_p); total_unc_site += len(unc_s)
    if suggest and unc_p:
        print(f"   -- suggestions (bank {b:02x}) --")
        for pc, imm, tgt, sc, _cov in unc_p:
            if tgt in rets:
                print(f"   SKIP  ${tgt:04X}: `ret:` of a cfg indirect_dispatch "
                      f"(B8C2 class) — benign unwind, registering it recurses. "
                      f"DO NOT register.")
                continue
            s1 = plausible(b, tgt, 1, 0)
            s0 = plausible(b, tgt, 0, 0)
            em = 1 if s1 >= s0 else 0
            print(f"   func bank_{b:02X}_{tgt:04X} {tgt:04X} entry_mx:{em},0"
                  f"   # push @{b:02x}:{pc:04x}; decode m1={s1} m0={s0}; "
                  f"VERIFY single-shot shape (DEBUG.md §1 ⚠️) before applying")

print(f"\nTOTAL uncovered: {total_unc_push} continuation pushes, "
      f"{total_unc_site} PHA;RTS sites")
print("Triage: a push whose continuation does PLA/PLX/PLY of loop state "
      "needs rts_dispatch (NOT func); a PHA;RTS site needs rts_dispatch "
      "<site> <targets> or indirect_dispatch. See DEBUG.md s7.13.")
