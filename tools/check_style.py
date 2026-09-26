#!/usr/bin/env python3
"""Hold authored C to the house layout rules, one file at a time.

The rules are what most of the code already does (measured 2026-09-25), so
they describe the majority rather than impose a new style:

  tab-indent       indentation uses spaces, never tabs
  trailing-space   no whitespace at the end of a line
  long-line        at most 100 columns
  packed           one statement per line; `a; b;` on one line is packed.
                   `case X: a; break;` and one-line braced blocks such as
                   `if (x) { a; b; }` are established idioms and allowed.
  static-g-prefix  file-private variables use `s_`; a `static` named `g_`
                   reads as a global
  header-guard     a header's include guard is AR_<FILE NAME>_H; file names
                   are unique across src/, so guards cannot collide
  header-comment   the first line of a header that is not blank or a
                   preprocessor directive is a comment saying what the module
                   owns

This is a ratchet, not a formatter. tools/style_baseline.json records how many
violations each file had when it was adopted. A file may never gain
violations, and a new file must have none. When a cleanup removes some, run
with --update-baseline to lock the improvement in; that refuses any increase
once a baseline exists.
Generated code is not checked: src/gen, src/generated, src/shaders/*.h,
*_data.inc, and any file whose first two lines say it is generated ("Generated
by", "Generated from", "GENERATED FILE" or "DO NOT EDIT"). Fix a generated file
in its generator, never by hand.
"""

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / 'tools/style_baseline.json'
RULES = ('tab-indent', 'trailing-space', 'long-line', 'packed', 'static-g-prefix',
         'header-guard', 'header-comment')
MAX_COLUMNS = 100
STATIC_G = re.compile(
    r'^static\s+(?!const\b)(?!inline\b)[^;(){}=]*?\bg_\w+\s*(\[[^\]]*\])*\s*(=[^;]*)?;')
CASE_LABEL = re.compile(r'\s*(case\b.*?:|default\s*:)')
GENERATED_MARK = re.compile(
    r'generated (by|from)|generated file|do not edit|^/\* generated\b', re.I | re.M)


def is_generated(path):
    with open(path, errors='replace') as source:
        head = source.readline() + source.readline()
    return bool(GENERATED_MARK.search(head))


def checked_files(root):
    try:
        listed = subprocess.run(
            ['git', 'ls-files', '--cached', '--others', '--exclude-standard',
             'src', 'tests'],
            cwd=root, capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        # Not a git checkout: every file on disk counts.
        listed = [path.relative_to(root).as_posix()
                  for top in ('src', 'tests') for path in (root / top).rglob('*')]
    for name in sorted(set(listed)):
        if not name.endswith(('.c', '.h', '.inc')):
            continue
        if name.startswith(('src/gen/', 'src/generated/', 'src/shaders/')):
            continue
        if name.endswith('_data.inc') or not (root / name).is_file():
            continue
        if is_generated(root / name):
            continue
        yield name


def strip_literals(line):
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
    line = re.sub(r'/\*.*?\*/', '', line)
    return re.sub(r'//.*', '', line)


def is_packed(code):
    depth = 0
    semicolons = []
    for index, char in enumerate(code):
        if char in '([':
            depth += 1
        elif char in ')]':
            depth -= 1
        elif char == ';' and depth == 0:
            semicolons.append(index)
    if len(semicolons) < 2 or CASE_LABEL.match(code):
        return False
    first = semicolons[0]
    rest = code[first + 1:].strip()
    return bool(rest) and not rest.startswith('}') and '{' not in code[:first]


def header_counts(path, text):
    counts = Counter()
    guard = re.search(r'^#ifndef\s+(\w+)\s*\n#define\s+\1\b', text, re.M)
    expected = 'AR_' + re.sub(r'\W', '_', path.stem).upper() + '_H'
    if not guard or guard.group(1) != expected:
        counts['header-guard'] += 1
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        if not stripped.startswith(('/*', '//')):
            counts['header-comment'] += 1
        break
    return counts


def count(path):
    counts = Counter()
    if path.suffix == '.h':
        counts.update(header_counts(path, path.read_text(errors='replace')))
    in_comment = False
    in_directive = False
    for raw in path.read_text(errors='replace').splitlines():
        if raw[:len(raw) - len(raw.lstrip())].count('\t'):
            counts['tab-indent'] += 1
        if raw != raw.rstrip():
            counts['trailing-space'] += 1
        if len(raw.expandtabs(8)) > MAX_COLUMNS:
            counts['long-line'] += 1
        stripped = raw.strip()
        if in_directive:
            in_directive = stripped.endswith('\\')
            continue
        if in_comment:
            in_comment = '*/' not in stripped
            continue
        if stripped.startswith('#'):
            in_directive = stripped.endswith('\\')
            continue
        if stripped.startswith('/*') and '*/' not in stripped:
            in_comment = True
            continue
        if stripped.startswith(('*', '//')):
            continue
        if STATIC_G.match(raw):
            counts['static-g-prefix'] += 1
        if is_packed(strip_literals(raw)):
            counts['packed'] += 1
    return counts


def measure(root):
    return {name: counts for name in checked_files(root)
            if (counts := count(root / name))}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--update-baseline', action='store_true',
                        help='record current counts; refuses any increase')
    parser.add_argument('--summary', action='store_true',
                        help='print totals per rule and top-level folder')
    parser.add_argument('--adopt-rules', default='',
                        help='comma-separated rules whose current counts become the baseline '
                             '(for introducing a rule); other rules stay ratcheted')
    args = parser.parse_args()
    adopted = {rule for rule in args.adopt_rules.split(',') if rule}
    unknown = adopted - set(RULES)
    if unknown:
        sys.exit(f'check_style: unknown rules: {", ".join(sorted(unknown))}')

    current = measure(ROOT)
    baseline = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}

    increases = []
    improved = 0
    for name in sorted(set(current) | set(baseline)):
        now, before = current.get(name, {}), baseline.get(name, {})
        for rule in RULES:
            if rule in adopted:
                continue
            if now.get(rule, 0) > before.get(rule, 0):
                increases.append(f'  {name}: {rule} {before.get(rule, 0)} -> '
                                 f'{now.get(rule, 0)}')
            elif now.get(rule, 0) < before.get(rule, 0):
                improved += 1

    if args.summary:
        totals = Counter()
        for name, counts in current.items():
            parts = name.split('/')
            folder = '/'.join(parts[:2]) if len(parts) > 2 else parts[0]
            for rule, value in counts.items():
                totals[(folder, rule)] += value
        for folder in sorted({f for f, _ in totals}):
            row = ', '.join(f'{r} {totals[(folder, r)]}' for r in RULES
                            if totals[(folder, r)])
            print(f'{folder}: {row}')

    # The first --update-baseline adopts the current counts; later ones may
    # only lower them.
    adopting = args.update_baseline and not BASELINE.exists()
    if adopted and not args.update_baseline:
        sys.exit('check_style: --adopt-rules only applies with --update-baseline')
    if increases and not adopting:
        print('Style check failed; these files gained violations:', file=sys.stderr)
        print('\n'.join(increases), file=sys.stderr)
        print('Rules and exceptions are described in tools/check_style.py.',
              file=sys.stderr)
        sys.exit(1)

    if args.update_baseline:
        BASELINE.write_text(json.dumps(
            {name: {rule: counts[rule] for rule in RULES if counts.get(rule)}
             for name, counts in sorted(current.items())},
            indent=1, sort_keys=True) + '\n')
        print(f'Style baseline updated: {len(current)} files with violations')
        return
    total = sum(sum(c.values()) for c in current.values())
    message = (f'Style check: no file gained violations; {total} remain in '
               f'{len(current)} files')
    if improved:
        message += (f'. {improved} per-file count(s) dropped; run '
                    'tools/check_style.py --update-baseline to lock that in')
    print(message)


if __name__ == '__main__':
    main()
