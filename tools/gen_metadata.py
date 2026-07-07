#!/usr/bin/env python3
"""Build a metadata sidecar from the emitted gen C + recomp cfgs.

One fast scrape after each regen produces `saves/gen_meta.json`, so trace
diagnosis (trace_slice.py --diagnose) becomes a join between runtime facts
(dispatch-misses, leaks) and static decode facts — no more hand-grepping 41MB
of generated C per bug.

Captures:
  functions   pc24 -> [variant suffixes]           (registered/callable entries)
  labels      pc24 -> [containing function names]  (decoder-created local blocks)
  tailcalls   [{src, tgt_fn, tgt_pc24}]            (tail-call-past-end sites)
  cfg         per-directive lists from recomp/bank*.cfg

Usage:
  python3 tools/gen_metadata.py            # writes saves/gen_meta.json
  python3 tools/gen_metadata.py --out X    # custom path
"""
import argparse, collections, glob, json, os, re, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
GEN = os.path.join(ROOT, 'src', 'gen')
CFG = os.path.join(ROOT, 'recomp')

FUNC_RE = re.compile(r'^RecompReturn (bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]+))(_M(\d)X(\d))\(CpuState \*cpu\) \{')
LABEL_RE = re.compile(r'^  L_([0-9A-Fa-f]{4})_M(\d)X(\d):')
TAIL_RE = re.compile(r'tail-call past end: into (bank_[0-9A-Fa-f]{2}_[0-9A-Fa-f]+_M\dX\d) at \$([0-9A-Fa-f]+)')

def scrape_gen():
    functions = collections.defaultdict(list)   # "BBPPPP" -> [suffix]
    labels = collections.defaultdict(set)       # "BBPPPP" -> {containing fn}
    tailcalls = []
    for path in sorted(glob.glob(os.path.join(GEN, 'bank*_v2.c'))):
        cur = None
        cur_bank = None
        for line in open(path, errors='replace'):
            m = FUNC_RE.match(line)
            if m:
                cur = m.group(1) + m.group(4)
                cur_bank = m.group(2).upper()
                key = f"{m.group(2).upper()}{m.group(3).upper()}"
                functions[key].append(m.group(4))
                continue
            if cur is None:
                continue
            lm = LABEL_RE.match(line)
            if lm:
                labels[f"{cur_bank}{lm.group(1).upper()}"].add(cur)
                continue
            tm = TAIL_RE.search(line)
            if tm:
                tailcalls.append({
                    'src': cur,
                    'tgt_fn': tm.group(1),
                    'tgt_pc24': f"{cur_bank}{tm.group(2).upper()}",
                })
    return functions, labels, tailcalls

CFG_DIRECTIVES = ('func', 'rts_dispatch', 'indirect_dispatch', 'exit_mx_at')

def scrape_cfg():
    cfg = {d: [] for d in CFG_DIRECTIVES}
    for path in sorted(glob.glob(os.path.join(CFG, 'bank*.cfg'))):
        bank = re.search(r'bank([0-9A-Fa-f]+)\.cfg', path).group(1).upper().zfill(2)
        for ln, line in enumerate(open(path), 1):
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            word = s.split()[0]
            if word in CFG_DIRECTIVES:
                cfg[word].append({'bank': bank, 'line': ln, 'text': s})
    return cfg

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'saves', 'gen_meta.json'))
    args = ap.parse_args()
    t0 = time.time()
    functions, labels, tailcalls = scrape_gen()
    cfg = scrape_cfg()
    meta = {
        'generated_at': time.strftime('%Y-%m-%d %H:%M:%S'),
        'functions': {k: sorted(v) for k, v in functions.items()},
        'labels': {k: sorted(v) for k, v in labels.items()},
        'tailcalls': tailcalls,
        'cfg': cfg,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w') as f:
        json.dump(meta, f)
    print(f"gen_meta: {len(functions)} func entries, {len(labels)} label pcs, "
          f"{len(tailcalls)} tailcall sites, "
          f"cfg: {', '.join(f'{d}={len(v)}' for d, v in cfg.items())} "
          f"-> {args.out} ({time.time()-t0:.1f}s)")

if __name__ == '__main__':
    main()
