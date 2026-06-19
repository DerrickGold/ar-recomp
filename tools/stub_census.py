#!/usr/bin/env python3
"""Standalone stub census for ActRaiserRecomp generated code.

Scans src/gen/*.c for every forbidden trap-stub idiom the recompiler can
emit when it fails to resolve control flow, and prints a classified,
deduplicated report. This is the same gate v2_regen applies at the end of
a regen (`_lint_stubs` + the unresolved-indirect report), but runnable
standalone against the committed src/gen without a full ~minutes regen.

Two trap classes are surfaced:

  goto      cpu_trace_unresolved_goto_trap  — a JMP/BRL/BRA whose target was
            not decoded into the enclosing function and is not a known entry
            (cross-fn / cross-bank goto the emitter could not bind).

  dispatch  cpu_trace_dispatch_oob          — an indirect JMP/JML/JSR whose
            jump table the decoder could not resolve (needs an
            `indirect_dispatch` cfg directive, or it is garbage decode).

Each row collapses the four M/X variants of a site into one logical site so
the count reflects distinct control-flow gaps, not variant fan-out.

Exit status: 0 if clean, 1 if any trap site remains. Intended to be cheap
enough to run on every edit.

Usage:
    tools/stub_census.py [--gen-dir src/gen] [-v]
"""
import argparse
import pathlib
import re
import sys
from collections import defaultdict

# site_pc, target_pc, enclosing function name, label name
_GOTO_RE = re.compile(
    r'cpu_trace_unresolved_goto_trap\(cpu,\s*'
    r'0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*'
    r'"([^"]+)",\s*"([^"]+)"\)')

# Tolerant of argument shape; we just need site + any operand-ish hex.
_DISPATCH_RE = re.compile(
    r'cpu_trace_dispatch_oob\(cpu,\s*0x([0-9A-Fa-f]+)')

# strip the trailing _M<m>X<x> variant suffix so the four flag variants of
# one site collapse to a single logical entry.
_VARIANT_RE = re.compile(r'_M[01]X[01]$')


def _strip_variant(name: str) -> str:
    return _VARIANT_RE.sub('', name)


def scan(gen_dir: pathlib.Path):
    gotos = defaultdict(set)      # (site, target, base_fn) -> {variant lines}
    dispatches = defaultdict(set)  # site -> {file:line}
    for p in sorted(gen_dir.glob('bank*_v2.c')):
        with p.open('r', encoding='utf-8', errors='replace') as f:
            for ln, raw in enumerate(f, 1):
                m = _GOTO_RE.search(raw)
                if m:
                    site = int(m.group(1), 16)
                    target = int(m.group(2), 16)
                    fn = _strip_variant(m.group(3))
                    gotos[(site, target, fn)].add(f"{p.name}:{ln}")
                d = _DISPATCH_RE.search(raw)
                if d:
                    site = int(d.group(1), 16)
                    dispatches[site].add(f"{p.name}:{ln}")
    return gotos, dispatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen-dir', default='src/gen', type=pathlib.Path)
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='list every variant occurrence, not just the logical site')
    args = ap.parse_args()

    if not args.gen_dir.is_dir():
        print(f"stub_census: {args.gen_dir} not found "
              f"(run from repo root)", file=sys.stderr)
        return 2

    gotos, dispatches = scan(args.gen_dir)

    print("=== STUB CENSUS ===")
    print(f"  unresolved goto traps : {len(gotos)} logical site(s) "
          f"({sum(len(v) for v in gotos.values())} variant emissions)")
    print(f"  indirect-dispatch oob : {len(dispatches)} logical site(s) "
          f"({sum(len(v) for v in dispatches.values())} variant emissions)")

    if gotos:
        print("\n--- unresolved cross-fn / cross-bank gotos ---")
        for (site, target, fn), where in sorted(gotos.items()):
            print(f"  site=${site:06X} -> target=${target:06X}  in {fn}")
            if args.verbose:
                for w in sorted(where):
                    print(f"      {w}")

    if dispatches:
        print("\n--- unresolved indirect dispatch (needs indirect_dispatch cfg) ---")
        for site, where in sorted(dispatches.items()):
            print(f"  site=${site:06X}  ({len(where)} variant(s))")
            if args.verbose:
                for w in sorted(where):
                    print(f"      {w}")

    total = len(gotos) + len(dispatches)
    if total == 0:
        print("\nCLEAN — no trap stubs in emitted output.")
        return 0
    print(f"\n{total} logical trap site(s) remain. Stubs are forbidden — "
          f"resolve each at the gen path (decode coverage / indirect_dispatch "
          f"cfg), do NOT silence.")
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
