#!/usr/bin/env python3
"""Fixpoint scan of object-handler state chains the static decoder can't follow.

Action-stage object handlers are reached only via runtime handler-pointer writes
(spawn-data initial handlers + the `JSR $8657` coroutine-yield idiom, whose
return address becomes the object's $1E resume handler). The recompiler can't
see these statically, so the target routines stay unconverted -> dispatch miss
-> the object freezes (e.g. the Fillmore bridge never extends).

Given seed handler addresses, follow bank-0 control flow (m=0,x=0, the object
loop's REP #$30 state) and collect every reachable code entry, flagging those
not yet emitted as functions so they can be registered in recomp/bank00.cfg.

Yield model: `JSR $8657` does NOT return to site+3 in this invocation — site+3
becomes a separate handler entry (next-frame resume). So we STOP linear decode
there and add site+3 as a new seed.
"""
import sys, re, glob

rom = open('ar.sfc','rb').read()
def rd(a): return rom[a-0x8000]
def rd16(a): return rom[a-0x8000] | (rom[a-0x8000+1]<<8)

# converted function entries (bank 0)
emitted=set()
for f in glob.glob('src/gen/*.c'):
    for m in re.finditer(r'bank_00_([0-9A-F]{4})_M[0-9]X[0-9]\(CpuState \*cpu\) \{', open(f).read()):
        emitted.add(int(m.group(1),16))

YIELDS={0x8657,0x8668,0x8669}  # JSR targets that yield (return addr -> $1E resume handler)
# 65816 length by addressing mode; immediate adjusts with m (A ops) / x (idx ops)
# We run m=0,x=0 -> 16-bit immediates (3-byte) for both.
def insn_len(op, a):
    # returns (length, kind, target)  kind in {seq,branch,jump,call,call8657,stop}
    b=op
    # branches (rel8)
    if b in (0x10,0x30,0x50,0x70,0x90,0xB0,0xD0,0xF0,0x80):
        rel=rd(a+1); tgt=a+2+(rel-256 if rel>127 else rel)
        return (2,'stop' if b==0x80 else 'branch','jump' if b==0x80 else None, tgt if True else None, tgt)
    if b==0x82:  # BRL rel16
        rel=rd16(a+1); tgt=a+3+(rel-65536 if rel>32767 else rel); return (3,'jump',tgt)
    if b in (0x4C,):  # JMP abs
        return (3,'jump',rd16(a+1))
    if b in (0x6C,0x7C,0xDC):  # JMP indirect/indexed/long-indirect -> unknown
        return (3 if b!=0xDC else 3,'stop',None)
    if b==0x5C: return (4,'stop',None)  # JML
    if b in (0x60,0x6B,0x40): # RTS,RTL,RTI -> terminal
        return (1,'stop',None)
    if b in (0x00,0x02): # BRK/COP inline syscall -> continues (op + signature byte)
        return (2,'seq',None)
    if b==0x20:  # JSR abs
        t=rd16(a+1)
        return (3,'call8657' if t in YIELDS else 'call', t)
    if b==0x22:  # JSL long
        return (4,'call', rd16(a+1))  # bank ignored (assume same/bank2 helper returns)
    # immediate ops (m: A-imm; x: idx-imm) -> 16-bit here
    A_IMM={0x09,0x29,0x49,0x69,0x89,0xA9,0xC9,0xE9}      # ORA/AND/EOR/ADC/BIT/LDA/CMP/SBC #
    X_IMM={0xA0,0xA2,0xC0,0xE0}                          # LDY/LDX/CPY/CPX #
    if b in A_IMM or b in X_IMM: return (3,'seq',None)
    # common 3-byte abs / abs,X / abs,Y
    THREE={0x0D,0x0E,0x1D,0x1E,0x19,0x2D,0x2C,0x3D,0x3C,0x4D,0x5D,0x6D,0x7D,0x8D,0x8C,0x8E,0x9D,0x9C,0x99,0x9E,0xAD,0xAC,0xAE,0xBD,0xBC,0xBE,0xB9,0xCD,0xCC,0xDD,0xED,0xEE,0xEC,0xFD,0xFE,0x0C,0x1C}
    if b in THREE: return (3,'seq',None)
    FOUR={0x0F,0x2F,0x4F,0x6F,0x8F,0xAF,0xCF,0xEF,0x1F,0x3F,0x5F,0x7F,0x9F,0xBF,0xDF,0xFF}  # long abs,(X)
    if b in FOUR: return (4,'seq',None)
    TWO={0x05,0x06,0x25,0x26,0x45,0x46,0x65,0x66,0x85,0x86,0x84,0xA5,0xA6,0xA4,0xC5,0xC6,0xE5,0xE6,0x04,0x14,0x15,0x16,0x24,0x35,0x36,0x55,0x56,0x75,0x76,0x95,0x94,0x96,0xB5,0xB6,0xB4,0xD5,0xD6,0xF5,0xF6,0x01,0x11,0x12,0x21,0x31,0x32,0x41,0x51,0x52,0x61,0x71,0x72,0x81,0x91,0x92,0xA1,0xB1,0xB2,0xC1,0xD1,0xD2,0xE1,0xF1,0xF2,0xE2,0xC2,0x07,0x17,0x27,0x37,0x47,0x57,0x67,0x77,0x87,0x97,0xA7,0xB7,0xC7,0xD7,0xE7,0xF7,0xD4,0x62,0x44,0x54,0xF4,0xD0}
    if b in TWO: return (2,'seq',None)
    # implied 1-byte
    return (1,'seq',None)

def scan(seeds):
    """Return only REGISTERABLE dispatch entries = seeds + their `JSR $8657`
    yield-continuations. We traverse each entry's internal control flow
    (branches/jumps/fall-through) ONLY to discover yields — branch/jump targets
    are internal blocks the recompiler decodes automatically from the entry, so
    they are NOT registered (registering shared subroutines was the over-reach)."""
    seen=set(); entries=set(seeds); work=list(seeds)
    while work:
        stack=[work.pop()]
        while stack:
            pc=stack.pop()
            while 0x8000<=pc<=0xFFFF and pc not in seen:
                seen.add(pc)
                op=rd(pc); res=insn_len(op,pc); ln=res[0]; kind=res[1]
                if kind=='seq': pc+=ln; continue
                if kind=='branch':
                    t=res[-1]
                    if 0x8000<=t<=0xFFFF: stack.append(t)
                    pc+=ln; continue
                if kind=='jump':
                    t=res[-1]
                    if t and 0x8000<=t<=0xFFFF: stack.append(t)
                    break
                if kind=='call': pc+=ln; continue   # subroutine returns; keep going
                if kind=='call8657':
                    cont=pc+ln                        # yield: continuation is a new dispatch entry
                    if 0x8000<=cont<=0xFFFF and cont not in entries:
                        entries.add(cont); work.append(cont)
                    break
                if kind=='stop': break
    return entries

# Seeds: hex handler addresses from argv (e.g. `find_handler_chain.py AC11 AC41`),
# else the bridge defaults. Tip: get seeds from a crash snapshot by scanning the
# object table ($06A0 stride $40, >=64 slots) for active objs whose $12 handler
# isn't an emitted bank_00_* function.
def derive_table_seeds():
    """Seeds from the 8 per-level handler tables ($9557's $95DD dispatch list).
    Each table entry is a per-object RECORD base B; the spawn dispatcher sets the
    object handler $12 = B+0x0C (an init `JSR`), and after the one-time init the
    steady-state handler is B+0x0F. base+0x0C are all already converted; the
    JSR-gated base+0x0F continuations are the gap. Returns those not yet emitted."""
    tables=[0x96AF,0xA8F6,0xB449,0xC11E,0xCD9B,0xD928,0xE722,0xF39A]
    out=set()
    for base in tables:
        a=base
        for _ in range(80):
            B=rd16(a)
            if not (0x8000<=B<=0xFFFF): break
            if rd(B+0x0C) in (0x20,0x22):      # base+0xC is JSR/JSL -> +0xF is a valid entry
                out.add((B+0x0F)&0xFFFF)
            a+=2
    return out

def derive_field14_seeds():
    """Field-$14 secondary handlers = record[0x0A] of each per-level record (the
    spawn dispatcher does `LDA rec[0x0A],Y; STA $14,X`, and some object types later
    copy $14 -> the dispatch slot). Field $14 is POLYMORPHIC (handler for some
    object types, data — counter/coord/velocity — for others) and value alone can't
    tell them apart (the 65816 has no illegal opcodes), so we apply three filters:
      (a) value in code range $8000-$FFFF,
      (b) NOT part of a consecutive-address cluster (the data signature: e.g.
          $8502..$8506 are offsets into one routine, an index, not 5 handlers),
      (c) first opcode is handler-shaped AND it decodes coherently (reaches an
          RTS/RTL/yield within ~100 insns without leaving the bank).
    Conservative by design — it skips ambiguous values (COP/BNE/BRK starts) rather
    than risk registering data as code; those rare cases fall back to runtime
    discovery (F2 snapshot + a single-seed run)."""
    HSTART={0x08,0xA9,0xBD,0x9E,0x9D,0x9C,0xE2,0xC2,0x20,0x22,0xFE,0xA0,0xA2,0x98,
            0xBC,0xAD,0xB9,0xAE,0x64,0x74,0xDA,0x5A,0xA5,0xA6,0xA4}
    tables=[0x96AF,0xA8F6,0xB449,0xC11E,0xCD9B,0xD928,0xE722,0xF39A]
    raw=set()
    for base in tables:
        a=base
        for _ in range(80):
            B=rd16(a)
            if not (0x8000<=B<=0xFFFF): break
            v=rd16(B+0x0A)
            if 0x8000<=v<=0xFFFF: raw.add(v)
            a+=2
    def coherent(v):
        pc=v
        for _ in range(120):
            if not (0x8000<=pc<=0xFFFF): return False
            op=rd(pc); res=insn_len(op,pc); k=res[1]
            if k=='stop': return True               # hit RTS/RTL/RTI/JML/etc.
            if k=='call8657': return True            # hit a yield = definitely a handler
            if k in ('seq','call','branch'): pc+=res[0]; continue
            if k=='jump':
                t=res[-1]
                if t and 0x8000<=t<=0xFFFF: pc=t; continue
                return True
        return False
    out=set()
    for v in raw:
        if (v-1) in raw or (v+1) in raw: continue   # (b) consecutive cluster = data
        if rd(v) not in HSTART: continue             # (c1) handler-shaped start
        if not coherent(v): continue                 # (c2) decodes coherently
        out.add(v)
    return out

if __name__=='__main__':
    if '--field14' in sys.argv:
        seeds=sorted(derive_field14_seeds())
    elif '--all-yields' in sys.argv:
        # Comprehensive: seed from EVERY converted handler and follow all $8657/$8669
        # yields. Catches every JSR-$8657/$8669 continuation that isn't itself an
        # entry — regardless of how its container handler was reached (covers the
        # nested-$1E / field-$14 residual class that --tables can't, e.g. $ABE5).
        seeds=sorted(emitted)
    elif '--tables' in sys.argv:
        seeds=sorted(derive_table_seeds())
    else:
        seeds=[int(x,16) for x in sys.argv[1:]] or [0xACEA,0xAB19]
    ents=scan(seeds)
    unconv=sorted(e for e in ents if e not in emitted)
    print(f"# seeds: {len(seeds)}; reachable entries: {len(ents)}; UNCONVERTED: {len(unconv)}")
    for e in unconv:
        print(f"func bank_00_{e:04X} {e:04X} entry_mx:0,0")
