#!/usr/bin/env python3
"""Layer B — differential per-opcode correctness harness.

Tests the recompiler's emitted C for individual opcodes against the
Tom Harte SingleStepTests/65816 vectors (the de-facto reference 65816
oracle: 10,000 native + 10,000 emulation cases per opcode, each with
full pre/post processor + memory state).

How it works
------------
The recompiler is *static* — operands are baked into the emitted C at
decode time — so a single compiled function only handles one operand.
To test against vectors with varying operands we therefore:

  1. read the instruction bytes from each vector's `initial.ram` at
     PBR:PC,
  2. decode -> lower -> emit C for that exact instruction (codegen.py),
  3. wrap each as a tiny `t_<i>(CpuState*)` function,
  4. concatenate N of them into ONE translation unit, compile once,
  5. run: load each vector's initial CPU+RAM, call t_<i>, compare the
     final CPU+RAM against the vector.

Memory backend
--------------
Harte assumes a single flat 24-bit address space. The recompiler's
emitted code, however, routes direct-page / low-RAM accesses to bank
$7E (the SNES WRAM mirror). The flat backend here therefore aliases
bank $00 <-> $7E so a vector's bank-0 operand and the recomp's $7E
access land in the same cell. Everything else is plain flat RAM with
all writes allowed (no MMIO, no ROM read-only) — i.e. exactly Harte's
model.

Scope
-----
v1 covers the data-processing opcodes that are self-contained (no
RecompStack / dispatch deps): immediate / accumulator / implied — the
ALU, flag, shift, transfer and REP/SEP logic where semantic bugs
concentrate (this is where the decimal-mode ADC/SBC bug lived).
Memory-addressing and control-flow opcodes are layered on later.

Divergences flagged here are either real recompiler bugs OR places
where the recomp makes a game-specific simplifying assumption (e.g.
direct page always in WRAM because ActRaiser keeps D=0). Triage each.

Usage
-----
    tools/opcode_diff.py                 # default opcode set, 64 cases each
    tools/opcode_diff.py --count 500
    tools/opcode_diff.py --opcodes 69,E9 # just ADC#/SBC#
    tools/opcode_diff.py --mode native|emu
    tools/opcode_diff.py --keep          # keep generated C + binary
"""
import argparse
import json
import pathlib
import subprocess
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
RECOMP = REPO / 'third_party' / 'snesrecomp' / 'recompiler'
RUNNER_SRC = REPO / 'third_party' / 'snesrecomp' / 'runner' / 'src'
CACHE = REPO / 'tools' / 'oracle' / 'harte_cache'
BASE_URL = "https://raw.githubusercontent.com/SingleStepTests/65816/main/v1"

sys.path.insert(0, str(RECOMP))
import snes65816 as S            # noqa: E402
from v2.lowering import lower    # noqa: E402
from v2.codegen import emit_op   # noqa: E402
from v2.ir import Value          # noqa: E402

# v1 in-scope opcodes: immediate / accumulator / implied data-processing.
# No memory addressing, no control flow.
DEFAULT_OPCODES = [
    # immediate ALU / load / compare / bit
    0xA9, 0xA2, 0xA0,              # LDA# LDX# LDY#
    0x69, 0xE9,                    # ADC# SBC#
    0x29, 0x09, 0x49,              # AND# ORA# EOR#
    0xC9, 0xE0, 0xC0,              # CMP# CPX# CPY#
    0x89,                          # BIT#
    # accumulator shifts / inc / dec
    0x0A, 0x4A, 0x2A, 0x6A, 0x1A, 0x3A,
    # transfers
    0xAA, 0x8A, 0xA8, 0x98, 0x9B, 0xBB, 0xBA, 0x9A,
    0x5B, 0x7B, 0x1B, 0x3B,
    # index inc/dec
    0xE8, 0xC8, 0xCA, 0x88,
    # flag set/clear
    0x18, 0x38, 0x58, 0x78, 0xD8, 0xF8, 0xB8,
    # width / misc
    0xC2, 0xE2, 0xEB, 0xEA,        # REP SEP XBA NOP
]

# Mnemonics excluded from --all: control flow (own ABI layer), block
# moves, PC-relative push, and genuinely unmodeled ops. Everything else
# — all memory-addressing data ops + stack push/pull — is in scope.
SKIP_MNEM = frozenset({
    'JMP', 'JML', 'JSR', 'JSL', 'RTS', 'RTL', 'RTI',
    'BRK', 'COP', 'STP', 'WAI', 'WDM',
    'BRA', 'BRL', 'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ',
    'PER', 'MVN', 'MVP',
})


def all_supported_opcodes():
    ops = []
    for op in range(0x100):
        rec = S._OPCODES.get(op)
        if rec is None:
            continue
        mnem = rec[0]
        if mnem in SKIP_MNEM:
            continue
        ops.append(op)
    return ops


def vec_path(op, mode):
    suffix = 'n' if mode == 'native' else 'e'
    return CACHE / f"{op:02x}.{suffix}.json"


def fetch_vectors(op, mode):
    p = vec_path(op, mode)
    if not p.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        suffix = 'n' if mode == 'native' else 'e'
        url = f"{BASE_URL}/{op:02x}.{suffix}.json"
        print(f"  fetching {url} ...", flush=True)
        urllib.request.urlretrieve(url, p)
    with p.open() as f:
        return json.load(f)


def emit_for_insn(op, m, x, opbytes, pc, bank):
    """decode + lower + emit one instruction; return (insn, [c_lines]) or None."""
    data = bytes(opbytes) + b'\x00\x00\x00\x00'
    insn = S.decode_insn(data, 0, pc, bank, m, x)
    if insn is None or insn.opcode != op:
        return None
    insn.m_flag = m
    insn.x_flag = x
    counter = [0]

    def vf():
        counter[0] += 1
        return Value(vid=counter[0])

    ops = lower(insn, value_factory=vf)
    lines = []
    for o in ops:
        lines += emit_op(o)
    return insn, lines


C_PREAMBLE = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
/* cpu_trace.h provides inline no-op cpu_trace_event / cpu_trace_px_record
 * when SNESRECOMP_ENABLE_TRACE is undefined (the default), so the emit's
 * trace hooks compile to nothing. */
#include "cpu_trace.h"

/* Flat 24-bit memory with the SNES low-WRAM mirror: the emit routes
 * direct-page / low accesses to bank $7E, while Harte uses bank $00.
 * Aliasing $00 <-> $7E reconciles them. */
static uint8_t *MEM;
static inline uint32_t canon(uint8_t bank, uint16_t addr) {
    if (bank == 0x00 || bank == 0x7E) return 0x7E0000u | addr;
    if (bank == 0x7F)                 return 0x7F0000u | addr;
    return ((uint32_t)bank << 16) | addr;
}
uint8  cpu_read8 (CpuState *c, uint8 b, uint16 a) { (void)c; return MEM[canon(b,a)]; }
uint16 cpu_read16(CpuState *c, uint8 b, uint16 a) { (void)c; uint32_t f=canon(b,a);
    return (uint16)(MEM[f] | (MEM[(f+1)&0xFFFFFF] << 8)); }
void cpu_write8 (CpuState *c, uint8 b, uint16 a, uint8 v)  { (void)c; MEM[canon(b,a)] = v; }
void cpu_write16(CpuState *c, uint8 b, uint16 a, uint16 v) { (void)c; uint32_t f=canon(b,a);
    MEM[f]=(uint8)(v&0xFF); MEM[(f+1)&0xFFFFFF]=(uint8)(v>>8); }

typedef struct { uint16_t a,x,y,s,d; uint8_t dbr,pbr,p,e; } Regs;
typedef struct { uint32_t addr; uint8_t val; } RamPair;
typedef void (*TFn)(CpuState *);
'''

C_MAIN = r'''
int main(void) {
    MEM = (uint8_t*)calloc(0x1000000u, 1);
    if (!MEM) { fprintf(stderr, "OOM\n"); return 2; }
    int fails = 0;
    for (int i = 0; i < NTESTS; i++) {
        /* clear + load initial RAM (only touched cells) */
        for (int k = 0; k < IRAM_CNT[i]; k++) MEM[canon_pair(IRAM[IRAM_OFF[i]+k].addr)] = 0;
        for (int k = 0; k < FRAM_CNT[i]; k++) MEM[canon_pair(FRAM[FRAM_OFF[i]+k].addr)] = 0;
        for (int k = 0; k < IRAM_CNT[i]; k++) MEM[canon_pair(IRAM[IRAM_OFF[i]+k].addr)] = IRAM[IRAM_OFF[i]+k].val;

        CpuState cpu; memset(&cpu, 0, sizeof cpu);
        Regs in = INIT[i];
        cpu.A=in.a; cpu.X=in.x; cpu.Y=in.y; cpu.S=in.s; cpu.D=in.d;
        cpu.DB=in.dbr; cpu.PB=in.pbr; cpu.P=in.p; cpu.emulation=in.e;
        cpu_p_to_mirrors(&cpu);

        FNS[i](&cpu);
        cpu_mirrors_to_p(&cpu);

        Regs ex = FIN[i];
        int bad = 0;
        char buf[512]; int n=0;
        #define CK(field, got, exp) do { if ((unsigned)(got) != (unsigned)(exp)) { \
            n += snprintf(buf+n, sizeof(buf)-n, " %s=%X(exp %X)", field, (unsigned)(got), (unsigned)(exp)); bad=1; } } while(0)
        CK("A", cpu.A, ex.a);
        CK("X", cpu.X, ex.x);
        CK("Y", cpu.Y, ex.y);
        CK("S", cpu.S, ex.s);
        CK("D", cpu.D, ex.d);
        CK("DB", cpu.DB, ex.dbr);
        CK("P", cpu.P, ex.p);
        CK("E", cpu.emulation, ex.e);
        for (int k = 0; k < FRAM_CNT[i]; k++) {
            uint32_t a = FRAM[FRAM_OFF[i]+k].addr; uint8_t want = FRAM[FRAM_OFF[i]+k].val;
            uint8_t got = MEM[canon_pair(a)];
            if (got != want) { n += snprintf(buf+n, sizeof(buf)-n, " m[%06X]=%02X(exp %02X)", a, got, want); bad=1; }
        }
        if (bad) {
            fails++;
            if (fails <= MAXSHOW)
                printf("FAIL op=%02X test=%d:%s\n", OPCODE[i], TESTNO[i], buf);
        }
    }
    printf("RESULT %d/%d passed (%d failed)\n", NTESTS-fails, NTESTS, fails);
    return fails ? 1 : 0;
}
'''


def cval(v):
    return f"0x{v:X}"


def build_c(records, max_show):
    """records: list of dicts {idx, opcode, testno, lines, init, fin, iram, fram}."""
    out = [C_PREAMBLE]
    # canon for a flat address (already bank-encoded)
    out.append("static inline uint32_t canon_pair(uint32_t flat){"
               " return canon((uint8)((flat>>16)&0xFF), (uint16)(flat&0xFFFF)); }\n")

    for r in records:
        out.append(f"static void t_{r['idx']}(CpuState *cpu) {{")
        for ln in r['lines']:
            out.append("    " + ln)
        out.append("}")

    n = len(records)
    out.append(f"#define NTESTS {n}")
    out.append(f"#define MAXSHOW {max_show}")

    out.append("static TFn FNS[NTESTS] = {" + ",".join(f"t_{r['idx']}" for r in records) + "};")
    out.append("static int OPCODE[NTESTS] = {" + ",".join(str(r['opcode']) for r in records) + "};")
    out.append("static int TESTNO[NTESTS] = {" + ",".join(str(r['testno']) for r in records) + "};")

    def regs_init(key):
        parts = []
        for r in records:
            s = r[key]
            parts.append("{%s,%s,%s,%s,%s,%s,%s,%s,%s}" % (
                cval(s['a']), cval(s['x']), cval(s['y']), cval(s['s']), cval(s['d']),
                cval(s['dbr']), cval(s['pbr']), cval(s['p']), cval(s['e'])))
        return "{" + ",".join(parts) + "}"

    out.append(f"static Regs INIT[NTESTS] = {regs_init('init')};")
    out.append(f"static Regs FIN[NTESTS]  = {regs_init('fin')};")

    # flattened ram pair pools
    def ram_pool(key):
        pool = []
        offs = []
        cnts = []
        for r in records:
            offs.append(len(pool))
            pairs = r[key]
            cnts.append(len(pairs))
            for (a, v) in pairs:
                pool.append((a, v))
        body = ",".join("{0x%X,0x%X}" % (a, v) for (a, v) in pool) or "{0,0}"
        return body, offs, cnts

    ibody, ioff, icnt = ram_pool('iram')
    fbody, foff, fcnt = ram_pool('fram')
    out.append(f"static const RamPair IRAM[] = {{{ibody}}};")
    out.append("static int IRAM_OFF[NTESTS] = {" + ",".join(map(str, ioff)) + "};")
    out.append("static int IRAM_CNT[NTESTS] = {" + ",".join(map(str, icnt)) + "};")
    out.append(f"static const RamPair FRAM[] = {{{fbody}}};")
    out.append("static int FRAM_OFF[NTESTS] = {" + ",".join(map(str, foff)) + "};")
    out.append("static int FRAM_CNT[NTESTS] = {" + ",".join(map(str, fcnt)) + "};")

    out.append(C_MAIN)
    return "\n".join(out)


def gather(op, mode, count):
    vectors = fetch_vectors(op, mode)
    records = []
    skipped = 0
    for testno, t in enumerate(vectors):
        if len(records) >= count:
            break
        ini = t['initial']
        p = ini['p']
        m = (p >> 5) & 1
        x = (p >> 4) & 1
        pc = ini['pc'] & 0xFFFF
        bank = ini['pbr'] & 0xFF
        ramd = {a: v for (a, v) in ini['ram']}
        flatpc = (bank << 16) | pc
        opbytes = [ramd.get((flatpc + k) & 0xFFFFFF, 0) for k in range(4)]
        res = emit_for_insn(op, m, x, opbytes, pc, bank)
        if res is None:
            skipped += 1
            continue
        insn, lines = res
        fin = t['final']

        def regs(d):
            return {'a': d['a'], 'x': d['x'], 'y': d['y'], 's': d['s'], 'd': d['d'],
                    'dbr': d['dbr'], 'pbr': d['pbr'], 'p': d['p'], 'e': d['e']}
        records.append({
            'opcode': op, 'testno': testno, 'lines': lines,
            'init': regs(ini), 'fin': regs(fin),
            'iram': [(a & 0xFFFFFF, v & 0xFF) for (a, v) in ini['ram']],
            'fram': [(a & 0xFFFFFF, v & 0xFF) for (a, v) in fin['ram']],
        })
    return records, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--opcodes', help='comma-separated hex opcodes (default: built-in set)')
    ap.add_argument('--all', action='store_true',
                    help='test every supported opcode (all but control-flow/block-move)')
    ap.add_argument('--count', type=int, default=64, help='vectors per opcode (default 64)')
    ap.add_argument('--mode', choices=['native', 'emu'], default='native')
    ap.add_argument('--max-show', type=int, default=12, help='max failing-test lines to print')
    ap.add_argument('--keep', action='store_true', help='keep generated C + binary')
    args = ap.parse_args()

    if args.opcodes:
        opcodes = [int(s, 16) for s in args.opcodes.split(',')]
    elif args.all:
        opcodes = all_supported_opcodes()
    else:
        opcodes = DEFAULT_OPCODES

    records = []
    idx = 0
    per_op_skips = {}
    for op in opcodes:
        recs, skipped = gather(op, args.mode, args.count)
        per_op_skips[op] = skipped
        for r in recs:
            r['idx'] = idx
            idx += 1
            records.append(r)

    if not records:
        print("no records gathered", file=sys.stderr)
        return 2

    csrc = build_c(records, args.max_show)
    workdir = REPO / 'build' / 'opcode_diff'
    workdir.mkdir(parents=True, exist_ok=True)
    cpath = workdir / 'harness.c'
    bpath = workdir / 'harness'
    cpath.write_text(csrc)

    cc = ['cc', '-O1', '-std=gnu11', f'-I{RUNNER_SRC}',
          str(cpath), '-o', str(bpath)]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode != 0:
        print("=== COMPILE FAILED ===")
        print(r.stderr[:6000])
        print(f"(generated C kept at {cpath})")
        return 2

    run = subprocess.run([str(bpath)], capture_output=True, text=True)
    print(run.stdout, end='')
    if run.stderr:
        print(run.stderr, file=sys.stderr)

    if not args.keep:
        try:
            cpath.unlink(); bpath.unlink()
        except OSError:
            pass

    # per-opcode skip summary (decode/scope mismatches)
    noisy = {f"{op:02X}": s for op, s in per_op_skips.items() if s}
    if noisy:
        print("skipped (decode/opcode mismatch):", noisy)
    return run.returncode


if __name__ == '__main__':
    raise SystemExit(main())
