#!/usr/bin/env python3
"""Find review candidates in compiled Voxel depths without starting the game.

Depths and material roles come exclusively from the production C compiler.
This module detects patterns in those immutable results; it never fixes maps
or attempts to reproduce the renderer's material-classification policy.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EPSILON = 1e-4
SIDES = ((0, -1, 0, 2), (1, 0, 1, 3), (0, 1, 2, 0), (-1, 0, 3, 1))


def _position(cell):
    return cell['x'], cell['y']


def _signature(cell):
    return round(cell['front'][0], 5), round(cell['back'][0], 5)


def _reasons(cell):
    reasons = set(cell.get('flags', ()))
    if cell.get('authored_fields', 0):
        reasons.add('authored_override')
    if cell.get('opaque', 256) == 0:
        reasons.add('empty_art')
    if any(max(cell[key]) - min(cell[key]) > EPSILON for key in ('front', 'back')):
        reasons.add('mixed_quadrant_depth')
    if cell.get('priority', 0) not in (0, 15):
        reasons.add('mixed_quadrant_priority')
    return sorted(reasons)


def _connected(a, b, side, opposite):
    return (a.get('priority', 0) == b.get('priority', 0)
            and abs(a['back'][0] - b['back'][0]) <= EPSILON
            and (a.get('borders', [0] * 4)[side]
                 & b.get('borders', [0] * 4)[opposite]).bit_count() >= 4)


def analyze_room(room):
    """Return deterministic candidates and exclusions for one exporter record.

    High confidence requires two independent witnesses: at least two connected
    A/B/A depth returns, plus identical native artwork at A's depth within the
    same connected body. Collision deliberately does not define visual identity.
    Priority, rear attachment, source contact and explicit/complex ownership do.
    """
    result = {key: room.get(key) for key in ('source', 'room', 'terrain', 'section', 'status', 'profile', 'frame')}
    result.update(findings=[], exclusions={}, counts={})
    if room.get('status', 'ok') != 'ok':
        result['exclusions'] = {room['status']: 1}
        result['counts'] = {'cells': 0, 'eligible_cells': 0, 'excluded_cells': 0, 'high': 0, 'review': 0}
        return result
    cells = {}
    for cell in room['cells']:
        key = _position(cell)
        if key in cells:
            raise ValueError(f"Duplicate cell {key} in {room['room']}")
        for field in ('front', 'back'):
            if len(cell[field]) != 4 or not all(math.isfinite(v) for v in cell[field]):
                raise ValueError(f"Invalid {field} at {key}")
        cells[key] = cell
    eligible, exclusions = {}, Counter()
    for key, cell in sorted(cells.items()):
        reasons = _reasons(cell)
        exclusions.update(reasons)
        if not reasons:
            eligible[key] = cell

    # Connect through the actual opaque native borders. A tile-coordinate
    # adjacency alone cannot join floating scenery or transparent silhouettes.
    component, groups = {}, []
    for seed in eligible:
        if seed in component:
            continue
        index = len(groups)
        members, queue = [], deque([seed])
        component[seed] = index
        while queue:
            key = queue.popleft()
            members.append(key)
            for dx, dy, side, opposite in SIDES:
                other = key[0] + dx, key[1] + dy
                if (other in eligible and other not in component
                        and _connected(eligible[key], eligible[other], side, opposite)):
                    component[other] = index
                    queue.append(other)
        groups.append(members)

    # Index exact art and depth once. Neither metatile IDs nor collision bits
    # participate; stamped words/palette/masks were already resolved by C.
    art_groups, local_art = defaultdict(list), defaultdict(list)
    for key, cell in eligible.items():
        identity = cell['art'], cell.get('priority', 0)
        art_groups[identity].append(key)
        local_art[(component[key], *identity)].append(key)
    threshold = max(EPSILON, abs(room.get('depth', .2)) * .10)
    motifs = defaultdict(list)
    for key, cell in eligible.items():
        for axis, (dx, dy, before_side, after_side) in enumerate(((1, 0, 3, 1), (0, 1, 0, 2))):
            before, after = (key[0] - dx, key[1] - dy), (key[0] + dx, key[1] + dy)
            if before not in eligible or after not in eligible:
                continue
            a, z = eligible[before], eligible[after]
            if not (_connected(cell, a, before_side, after_side)
                    and _connected(cell, z, after_side, before_side)):
                continue
            if abs(a['front'][0] - z['front'][0]) > EPSILON:
                continue
            if abs(a['front'][0] - cell['front'][0]) < threshold:
                continue
            signature = (component[key], axis, cell['art'], cell.get('priority', 0),
                         *_signature(cell), round(a['front'][0], 5))
            motifs[signature].append((key, before, after))

    findings, covered_art = [], set()
    for signature, entries in sorted(motifs.items()):
        group, axis, art, priority, front, back, surrounding = signature
        by_key = {entry[0]: entry for entry in entries}
        # Separate distant objects, even if the same large floor connects them.
        while by_key:
            seed = min(by_key)
            queue, cluster = [seed], []
            while queue:
                key = queue.pop()
                if key not in by_key:
                    continue
                cluster.append(by_key.pop(key))
                for sign in (-1, 1):
                    queue.append((key[0] + (2 * sign if axis == 0 else 0),
                                  key[1] + (2 * sign if axis == 1 else 0)))
            centers = sorted(entry[0] for entry in cluster)
            aliases = [key for key in local_art[(group, art, priority)]
                       if abs(eligible[key]['front'][0] - surrounding) <= EPSILON
                       and any(abs(key[0] - center[0]) + abs(key[1] - center[1]) <= 8
                               for center in centers)]
            high = len(centers) >= 2 and bool(aliases)
            witnesses = sorted({key for entry in cluster for key in entry[1:]} | set(aliases))
            findings.append({'confidence': 'high' if high else 'review',
                             'rule': 'repeated_depth_return_with_art_alias' if high else 'depth_return',
                             'axis': 'x' if axis == 0 else 'y', 'art': art,
                             'depth_delta': round(abs(front - surrounding), 6),
                             'cells': [eligible[key] for key in centers],
                             'witness_cells': [eligible[key] for key in witnesses],
                             'reason': ('Repeated connected depth returns disagree with identical artwork in the same body.'
                                        if high else 'Depth returns to its earlier plane; repetition and local artwork evidence are insufficient to infer an error.')})
            covered_art.add((art, priority))

    for identity, keys in sorted(art_groups.items()):
        if identity in covered_art:
            continue
        signatures = {_signature(eligible[key]) for key in keys}
        if len(signatures) < 2 or max(s[0] for s in signatures) - min(s[0] for s in signatures) < threshold:
            continue
        # Same-art reuse in separate structures is a review clue, never enough
        # to assert a policy bug. Preserve all locations in machine output.
        findings.append({'confidence': 'review', 'rule': 'same_art_different_depth',
                         'art': identity[0], 'cells': [eligible[key] for key in sorted(keys)],
                         'witness_cells': [],
                         'reason': 'Identical displayed artwork occupies different planes; deliberate reuse, context and native priority must be reviewed.'})
    findings.sort(key=lambda f: (f['confidence'] != 'high', f['rule'],
                                 tuple(_position(c) for c in f['cells'])))
    result['findings'] = findings
    result['exclusions'] = dict(sorted(exclusions.items()))
    result['counts'] = {'cells': len(cells), 'eligible_cells': len(eligible),
                        'excluded_cells': len(cells) - len(eligible),
                        'connected_bodies': len(groups),
                        'high': sum(f['confidence'] == 'high' for f in findings),
                        'review': sum(f['confidence'] == 'review' for f in findings)}
    return result


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _same_file(a, b):
    return a == b or (a.exists() and b.exists() and a.samefile(b))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenes', nargs='*', type=Path, help='Portable .arscene files or directories (searched recursively).')
    parser.add_argument('--native-bin', type=Path, default=ROOT / 'build-tests/actraiser_voxel_depth_audit')
    parser.add_argument('--ini', type=Path, default=ROOT / 'diorama-layers.ini', help='Shipped or editor-exported scenery INI; replaces the default file.')
    parser.add_argument('--no-ini', action='store_true', help='Audit native artwork without any scenery overrides.')
    parser.add_argument('--from-export', type=Path, help='Analyze previously saved diagnostic JSONL instead of launching the native exporter.')
    parser.add_argument('--raw-output', type=Path, help='Optionally keep complete native per-cell diagnostic JSONL (contains no tile pixels).')
    parser.add_argument('--output', type=Path, required=True, help='Deterministic JSON report. Candidates do not change the exit status.')
    parser.add_argument('--summary-limit', type=int, default=12, help='Maximum candidate lines printed; the JSON always keeps all findings.')
    args = parser.parse_args(argv)
    process = None
    raw = None
    try:
        paths = sorted({p.resolve() for entry in args.scenes for p in (entry.rglob('*.arscene') if entry.is_dir() else [entry])})
        protected = set(paths)
        if args.from_export:
            protected.add(args.from_export.resolve())
        else:
            protected.add(args.native_bin.resolve())
            if not args.no_ini:
                protected.add(args.ini.resolve())
        outputs = [p.resolve() for p in (args.output, args.raw_output) if p]
        if (any(_same_file(a, b) for i, a in enumerate(outputs) for b in outputs[i + 1:])
                or any(_same_file(p, source) for p in outputs for source in protected)):
            raise ValueError('Output paths must be distinct from each other and all audit inputs.')
        receipt = {}
        if args.from_export:
            stream = args.from_export.open()
            receipt['diagnostic_export'] = {'path': str(args.from_export), 'sha256': _sha(args.from_export)}
        else:
            if not paths:
                parser.error('Supply .arscene snapshots/directories or --from-export.')
            if not args.native_bin.is_file():
                raise ValueError('Build the headless target first: cmake --build build-tests --target actraiser_voxel_depth_audit --parallel 1')
            command = [str(args.native_bin.resolve())]
            if not args.no_ini:
                command += ['--ini', str(args.ini.resolve())]
                receipt['ini'] = {'path': str(args.ini.resolve()), 'sha256': _sha(args.ini)}
            else:
                receipt['ini'] = None
            receipt['compiler'] = {'path': str(args.native_bin.resolve()), 'sha256': _sha(args.native_bin)}
            receipt['scenes'] = [{'path': str(p), 'sha256': _sha(p)} for p in paths]
            process = subprocess.Popen(command + [str(p) for p in paths], stdout=subprocess.PIPE, text=True)
            stream = process.stdout
        if args.raw_output:
            args.raw_output.parent.mkdir(parents=True, exist_ok=True)
            raw = args.raw_output.open('w')
        rooms = []
        with stream:
            for line in stream:
                if not line.strip():
                    continue
                if raw:
                    raw.write(line)
                rooms.append(analyze_room(json.loads(line)))
        if process and process.wait():
            raise ValueError('Native depth export failed; no successful audit report was written.')
        if not rooms:
            raise ValueError('No room records were exported.')
        rooms.sort(key=lambda r: (r['room'], r.get('terrain', 0), r.get('section', ''), r.get('source') or ''))
        counts = Counter()
        for room in rooms:
            counts.update(room['counts'])
        report = {'schema': 1, 'tool': 'voxel_depth_audit', 'receipt': receipt,
                  'scope': {'scene_records': len(rooms), 'compiled_records': sum(r['status'] == 'ok' for r in rooms)},
                  'counts': dict(counts), 'rooms': rooms,
                  'limits': ['Candidates are review clues, never automatic map changes or acceptance.',
                             'The audit examines static BG1 cell depth and native contact. It does not validate sprites, gameplay, camera artifacts, transparency, all animation phases or performance.',
                             'Specialized curved interiors/completion routing and intentional/complex authored ownership are reported as exclusions, not as clean results.',
                             'Uniformly wrong depth and material errors without a contrasting repeated pattern may not be detected.']}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
        print(f"Audited {len(rooms)} scene records: {counts['high']} high-confidence candidates, {counts['review']} review clues; {counts['excluded_cells']} excluded cells.")
        shown = 0
        for room in rooms:
            for finding in room['findings']:
                if shown >= max(0, args.summary_limit):
                    break
                coords = ', '.join(f"{c['x']},{c['y']}" for c in finding['cells'][:8])
                if len(finding['cells']) > 8:
                    coords += f" (+{len(finding['cells']) - 8} more)"
                print(f"  {finding['confidence']}: {room['room']} terrain{room['terrain']} {finding['rule']} [{coords}]")
                shown += 1
        print(f"Full report: {args.output}")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Depth audit failed: {error}", file=sys.stderr)
        return 1
    finally:
        if raw:
            raw.close()
        if process and process.poll() is None:
            process.terminate()
            process.wait()


if __name__ == '__main__':
    raise SystemExit(main())
