#!/usr/bin/env python3
"""Layer A — static link / decode-structure auditor.

Layer B (tools/opcode_diff.py) verifies opcode *semantics* in isolation.
This tool audits the *linking layer*: the call-graph structure the
recompiler stitches together. It works purely from the emitted src/gen
(no decode pipeline, no runtime), the same way tools/stub_census.py does.

What the C build/linker and the oracle already cover, and therefore what
this tool deliberately does NOT re-check:
  - symbol-level call resolution — a call to an undefined variant fails
    the link, so it can't reach a built binary;
  - runtime stack-frame / S accounting — the stack lives in WRAM, so the
    snes9x oracle's WRAM diff already sees those bugs.

What this tool DOES surface — the static-only signal nothing else gives:

  1. ORPHANS — a function body that is defined but never referenced
     anywhere (no direct call, not in the dispatch table, not a named
     root, not used by hand-written runtime C). These are dead carves:
     usually the decoder followed a garbled operand into data and emitted
     a bogus function. An orphan that ALSO contains an unresolved trap is
     high-confidence garbage that can be suppressed (vs a live trap that
     must be fixed).

  2. VARIANT COVERAGE — per PC, which of the 4 (M,X) variants are defined
     vs actually referenced. A variant defined but never referenced hints
     the decoder over-decoded that PC at an (M,X) state nothing reaches.

  3. TRAPS — folds in the stub census (unresolved gotos + dispatch_oob)
     and classifies each by whether its function is reachable.

Usage:
    tools/link_audit.py [--gen-dir src/gen] [--src-dir src] [--orphans] [-v]
"""
import argparse
import pathlib
import re
import sys
from collections import defaultdict

NAME = r'(?:bank_[0-9A-Fa-f]+_[0-9A-Fa-f]+|[A-Za-z]\w*)_M[01]X[01]'
_DEF_RE = re.compile(rf'^RecompReturn ({NAME})\(CpuState \*cpu\)\s*\{{')
_CALL_RE = re.compile(rf'\b({NAME})\(cpu\)')
_TOKEN_RE = re.compile(rf'\b({NAME})\b')
_PROTO_RE = re.compile(rf'^RecompReturn {NAME}\(CpuState \*cpu\);\s*$')
_VARIANT_RE = re.compile(r'_M([01])X([01])$')
_GOTO_TRAP = 'cpu_trace_unresolved_goto_trap'
_DISP_TRAP = 'cpu_trace_dispatch_oob'


def base_and_variant(name):
    m = _VARIANT_RE.search(name)
    return name[:m.start()], (int(m.group(1)), int(m.group(2)))


def scan(gen_dir, src_dirs):
    defined = set()              # variant names with a body
    referenced = set()           # variant names referenced as call/dispatch/runtime
    fn_traps = defaultdict(list)  # enclosing fn name -> [trap kind]
    bank_files = sorted(gen_dir.glob('bank*_v2.c'))
    dispatch_file = gen_dir / 'dispatch_v2.c'

    # Pass 1: definitions + traps (track enclosing function while scanning)
    for p in bank_files:
        cur = None
        for raw in p.open(encoding='utf-8', errors='replace'):
            d = _DEF_RE.match(raw)
            if d:
                cur = d.group(1)
                defined.add(cur)
                continue
            if cur:
                if _GOTO_TRAP in raw:
                    fn_traps[cur].append('goto')
                elif _DISP_TRAP in raw:
                    fn_traps[cur].append('dispatch')

    # Pass 2: references. A name is "referenced" if it is called as
    # NAME(cpu), listed in the dispatch table, or used by hand-written C.
    # Prototype/definition lines do NOT count as references.
    def collect_refs(path, dispatch=False):
        for raw in path.open(encoding='utf-8', errors='replace'):
            if _PROTO_RE.match(raw) or _DEF_RE.match(raw):
                continue
            if dispatch:
                # dispatch table data lines reference every non-NULL slot
                for tok in _TOKEN_RE.findall(raw):
                    referenced.add(tok)
            else:
                for tok in _CALL_RE.findall(raw):
                    referenced.add(tok)

    for p in bank_files:
        collect_refs(p)
    if dispatch_file.exists():
        collect_refs(dispatch_file, dispatch=True)
    for sd in src_dirs:
        if sd.is_dir():
            for p in list(sd.glob('*.c')) + list(sd.glob('*.h')):
                collect_refs(p)
                collect_refs(p, dispatch=True)  # also catch table-style/aliased uses

    return defined, referenced, fn_traps


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen-dir', default='src/gen', type=pathlib.Path)
    ap.add_argument('--src-dir', default='src', type=pathlib.Path)
    ap.add_argument('--runner-dir',
                    default='third_party/snesrecomp/runner/src', type=pathlib.Path)
    ap.add_argument('--orphans', action='store_true',
                    help='list every orphan function (default: summary + trap orphans)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if not args.gen_dir.is_dir():
        print(f"link_audit: {args.gen_dir} not found (run from repo root)", file=sys.stderr)
        return 2

    defined, referenced, fn_traps = scan(args.gen_dir, [args.src_dir, args.runner_dir])

    orphans = defined - referenced
    trap_fns = set(fn_traps)
    orphan_traps = orphans & trap_fns
    live_traps = trap_fns - orphans

    # per-PC variant coverage
    by_pc_def = defaultdict(set)
    by_pc_ref = defaultdict(set)
    for n in defined:
        b, mx = base_and_variant(n)
        by_pc_def[b].add(mx)
    for n in referenced:
        if n in defined:  # only count refs to things we define
            b, mx = base_and_variant(n)
            by_pc_ref[b].add(mx)
    unref_variants = {n for n in defined if n not in referenced}

    print("=== LINK AUDIT ===")
    print(f"  defined functions   : {len(defined)}")
    print(f"  reachable (referenced): {len(defined & referenced)}")
    print(f"  ORPHANS (dead carves): {len(orphans)}")
    print(f"  unreferenced variants: {len(unref_variants)}")
    print(f"  functions with traps : {len(trap_fns)}  "
          f"(orphan/garbage: {len(orphan_traps)}, LIVE/must-fix: {len(live_traps)})")

    if live_traps:
        print("\n--- LIVE trap functions (reachable → must resolve) ---")
        for n in sorted(live_traps):
            print(f"  {n}: {','.join(sorted(set(fn_traps[n])))}")
    if orphan_traps:
        print("\n--- orphan trap functions (unreachable → safe to suppress decode) ---")
        for n in sorted(orphan_traps):
            print(f"  {n}: {','.join(sorted(set(fn_traps[n])))}")

    if args.orphans:
        print(f"\n--- all {len(orphans)} orphan functions ---")
        for n in sorted(orphans):
            print(f"  {n}")
    elif orphans:
        print(f"\n  ({len(orphans)} orphans total; pass --orphans to list, "
              f"or see trap orphans above)")

    if args.verbose:
        partial = {b: (by_pc_def[b], by_pc_ref[b]) for b in by_pc_def
                   if by_pc_ref[b] and by_pc_def[b] - by_pc_ref[b]}
        print(f"\n--- PCs with defined-but-never-referenced variants ({len(partial)}) ---")
        for b, (dset, rset) in sorted(partial.items())[:40]:
            miss = sorted(dset - rset)
            print(f"  {b}: defined={sorted(dset)} unreferenced={miss}")

    # Non-zero exit if there are live traps (actionable), like the census.
    return 1 if live_traps else 0


if __name__ == '__main__':
    raise SystemExit(main())
