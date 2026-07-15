#!/usr/bin/env python3
"""Yield-helper census: derive the helper list from the ROM, not from folklore.

Motivation (DEBUG.md §7.19, the Death Heim boss-rush pair of bugs): ActRaiser's
object system has a family of tiny "yield helpers" — routines a handler JSRs
into that POP THE CALLER'S RETURN ADDRESS and stash it in an object field, so
the object loop (or a later RTS) can resume the handler at JSR-site+3 frames
later. Every JSR site to such a helper makes site+3 a computed dispatch entry
that MUST be registered in the cfg, or the resume silently misses (the
dispatch-miss stderr tripwire gates S>=$0200 and the object loop runs at
S~$01F5, so nothing prints — the failure mode is an invisible per-frame skip,
i.e. a soft-lock, or an m-leak misdecode cascade if reached via a nested RTS).

Every prior registration pass keyed off a HAND-MAINTAINED helper list
($8623/$8657/$8669/$A673/$F8A6/$F8D2/$F977). That list was the single point of
failure, twice in one day:
  - $86FA (STA $24,X wait + PLA;INC;STA $12,X) was missing from it -> the
    Death Heim teleport-out sequencer's three continuations were unregistered
    -> post-boss soft-lock.
  - $F778 (PLA;STA $3E,X — a third stash field; re-pushed later by $F7C9 and
    consumed by $F807's RTS) was never even recognized as a helper -> boss 2+
    spawn-stub continuations unregistered -> B127-misdecode crash on entry.

This tool finds helpers by SHAPE instead. Two idioms, both = "capture the
caller's return address into an object field":
  PULL: a pull instruction (PLA/PLX/PLY) at PUSH-BALANCE ZERO (nothing pushed
    since entry, so it can only be pulling the caller's JSR frame), followed
    within TAIL_SCAN insns by a store of that value (directly or via one
    register transfer) into an object field (STA/STX/STY $00xx,X/Y with
    offset < $40, or dp,X). Consumes the frame: resume happens ONLY via the
    stored pointer. ($8657 $1E,X / $8669 PLY / $86FA $12,X / $F778 $3E,X)
  PEEK: LDA $nn,S (stack-relative) followed within TAIL_SCAN insns by
    (optional INC A +) the same object-field store. Does NOT consume the
    frame: the helper's RTS still returns normally, and the stored pointer
    re-enters the continuation on LATER frames. ($8623/$A673 $01,S->$12,X;
    $F8A6-family deeper offsets)
  Decode runs at m=0,x=0 (the object loop's widths).

For each confirmed helper it then enumerates every 'JSR helper' site in bank 0
and reports each continuation (site+3) that is not a registered cfg func.

Known limitation: linear decode, so a helper whose PLA is only reachable via a
branch may need eyes; candidates are printed with their evidence line so the
short list is reviewable. Run after any bank00.cfg change:

    python3 tools/find_yield_helpers.py            # census + missing report
    python3 tools/find_yield_helpers.py --lines    # emit ready-to-paste cfg lines
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ar_lib import load_rom, disasm, fmt24  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..'))
BANK = 0x00
BODY_SCAN = 28   # insns to scan for the balance-zero pull
TAIL_SCAN = 8    # insns after the pull for the object-field store

PULLS = {'PLA': 'A', 'PLX': 'X', 'PLY': 'Y'}
PUSHES = {'PHA', 'PHX', 'PHY', 'PHP', 'PHB', 'PHD', 'PHK', 'PEA', 'PEI', 'PER'}
XFERS = {('TAX', 'A', 'X'), ('TAY', 'A', 'Y'), ('TXA', 'X', 'A'),
         ('TYA', 'Y', 'A'), ('TXY', 'X', 'Y'), ('TYX', 'Y', 'X')}
STORES = {'STA': 'A', 'STX': 'X', 'STY': 'Y'}
# object-field store shapes: abs,X / abs,Y with a small offset, or dp,X
FIELD_MODES = {'absx', 'absy', 'dpx'}


def jsr_sites(rom):
    """All plausible 'JSR $xxxx' sites in bank 0 (byte scan; data hits get
    filtered because their targets won't decode into helper shape)."""
    sites = {}
    for i in range(0x8000 - 3):
        if rom[i] == 0x20:
            tgt = rom[i + 1] | (rom[i + 2] << 8)
            if 0x8000 <= tgt < 0x10000:
                sites.setdefault(tgt, []).append(0x8000 + i)
    return sites


def _field_store(insns, start, reg):
    """Scan TAIL_SCAN insns from `start` for a store of `reg` (following one
    register transfer / INC A) into an object field. Returns the Insn or None."""
    for jns in insns[start: start + TAIL_SCAN]:
        for (t, src, dst) in XFERS:
            if jns.mnem == t and src == reg:
                reg = dst
        if jns.mnem in STORES and STORES[jns.mnem] == reg and jns.mode in FIELD_MODES:
            if jns.mode == 'dpx' or (jns.operand is not None and jns.operand < 0x40):
                return jns
    return None


def helper_shape(rom, tgt):
    """Return evidence string if the routine at 00:tgt captures its caller's
    return address into an object field (PULL or PEEK idiom); else None."""
    balance = 0
    insns = list(disasm(rom, (BANK << 16) | tgt, m=0, x=0, count=BODY_SCAN))
    for idx, ins in enumerate(insns):
        if ins.mnem in PUSHES:
            balance += 1
        elif ins.mnem == 'LDA' and ins.mode == 'sr':
            # PEEK idiom: LDA $01,S at push-balance 0 is the caller's JSR
            # frame read in place. Any other offset/balance is the routine
            # reading its own pushed temps (e.g. $C09C's spawn-coords arg) —
            # those continuations are ordinary paired returns; registering
            # them would double-execute (DEBUG.md §7.17). Deep-offset
            # ancestor peeks ($F8A6-family LDA $FD,S) exist but have no JSR
            # callers; if one grows a JSR site, review it by hand.
            if balance == 0 and ins.raw[1] == 0x01:
                jns = _field_store(insns, idx + 1, 'A')
                if jns:
                    return (f"peek {ins.text.strip()} at {fmt24(ins.pc24)} "
                            f"-> store {jns.text.strip()} at {fmt24(jns.pc24)}")
        elif ins.mnem in PULLS:
            if balance > 0:
                balance -= 1
                continue
            # balance-zero pull = pulls the caller's JSR frame
            jns = _field_store(insns, idx + 1, PULLS[ins.mnem])
            if jns:
                return (f"pull {ins.mnem} at {fmt24(ins.pc24)} (balance 0) "
                        f"-> store {jns.text.strip()} at {fmt24(jns.pc24)}")
            return None
        elif ins.mnem in ('RTS', 'RTL', 'RTI', 'JMP', 'BRA', 'BRL', 'STP', 'BRK'):
            return None
    return None


def registered_funcs():
    cfg = open(os.path.join(ROOT, 'recomp', 'bank00.cfg')).read()
    return set(re.findall(r'^func bank_00_([0-9A-F]{4}) ', cfg, re.M))


def main():
    emit_lines = '--lines' in sys.argv
    rom = load_rom()
    sites = jsr_sites(rom)
    regs = registered_funcs()
    helpers = {}
    for tgt, calls in sorted(sites.items()):
        ev = helper_shape(rom, tgt)
        if ev:
            helpers[tgt] = (calls, ev)

    print(f"{len(helpers)} yield-helper-shaped JSR targets in bank 00:")
    missing = []
    for tgt, (calls, ev) in sorted(helpers.items()):
        print(f"\n  helper 00:{tgt:04X}  ({len(calls)} call sites)")
        print(f"    shape: {ev}")
        for s in calls:
            cont = s + 3
            mark = 'ok' if f'{cont:04X}' in regs else 'MISSING'
            if mark == 'MISSING':
                missing.append(cont)
            print(f"    site 00:{s:04X} -> continuation 00:{cont:04X}  [{mark}]")

    print()
    if missing:
        print(f"{len(missing)} UNREGISTERED continuations:")
        for c in sorted(missing):
            if emit_lines:
                print(f"func bank_00_{c:04X} {c:04X} entry_mx:0,0")
            else:
                print(f"  00:{c:04X}   (--lines for cfg lines)")
        sys.exit(1)
    print("all continuations registered ✓")


if __name__ == '__main__':
    main()
