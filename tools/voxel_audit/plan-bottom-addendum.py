#!/usr/bin/env python3
"""Add only underside treatment contexts absent from an existing capture plan.

This does not change existing case IDs or claim that a planned/captured family is
visible. The projected coverage reconciliation and visual review remain required.
"""
import argparse
import collections
import importlib.util
import json
import os
from pathlib import Path

spec = importlib.util.spec_from_file_location('visual_plan', Path(__file__).with_name('plan-visual.py'))
plan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plan)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('inventory', 'seed', 'existing', 'equality', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    assert os.getpriority(os.PRIO_PROCESS, 0) == 15, 'Run at nice15'
    seed = {r['id']: r for r in json.loads(args.seed.read_text())['rooms']}
    existing = {r['id']: r for r in json.loads(args.existing.read_text())['rooms']}
    equality = {(r['room'], r['profile']): r['equal_to_profile0'] for r in json.loads(args.equality.read_text())['rows']}
    result, stats = [], []
    for file in sorted((args.inventory / '0').glob('*.json.gz')):
        data0 = plan.load(file)
        room_id = '-'.join((data0['room'][:2], data0['room'][2:])) + ('-completion' if data0['section'] == 'completion' else '')
        base = seed[room_id]
        room = {key: base[key] for key in ('id', 'group', 'map', 'section')}
        room['windows'] = []
        next_id = 5000
        for profile in range(3):
            if equality[(room_id, profile)]:
                stats.append({'room': room_id, 'profile': profile, 'exact_geometry_reuse_profile0': True})
                continue
            data = data0 if profile == 0 else plan.load(args.inventory / str(profile) / file.name)
            surface = data['surface_audit']
            cells = {(c[0], c[1]): c for c in surface['source_cells']}
            families = collections.defaultdict(list)
            for values in surface['edges']:
                edge = dict(zip(surface['edge_fields'], values))
                if edge['side'] != 2 or not (edge.get('physical_span_valid') or edge.get('native_span_valid')):
                    continue
                families[plan.bottom_family(edge, surface, cells)].append(edge)
            planned = [w for w in existing[room_id]['windows'] if profile in w.get('profiles', [0])]
            negative = [w for w in planned if w.get('pitch', .25) < 0]
            def contained(window, edge):
                return plan.contains(window, (edge['x'], edge['y'], min(edge['length'], 16), 1))
            represented = {family for family, edges in families.items() if any(contained(w, e) for w in negative for e in edges)}
            remaining = set(families) - represented
            before = len(remaining)
            candidates = base['windows'] + planned
            while remaining:
                choices = []
                for window in candidates:
                    hits = {family for family in remaining if any(contained(window, edge) for edge in families[family])}
                    if hits:
                        choices.append((len(hits), window, hits))
                if not choices:
                    break
                _, window, hits = max(choices, key=lambda item: item[0])
                room['windows'].append({'id': next_id, 'bounds': window['bounds'], 'pitch': -.3, 'distance': 1.12,
                    'profiles': [profile], 'label': 'full-treatment-underside-addendum', 'freeCamera': True,
                    'treatment_signatures': sorted(hits), 'invariant': 'Previously unrepresented compiled fold/taper/placement treatment; actual visibility and review remain required.'})
                next_id += 1
                remaining -= hits
            stats.append({'room': room_id, 'profile': profile, 'full_treatment_families': len(families),
                          'previously_planned_families': len(represented), 'additional_family_requests': before,
                          'unlocated_families': sorted(remaining)})
        result.append(room)
        print(room_id, len(room['windows']), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'scope': 'Additive complete-treatment underside requests, not acceptance.', 'rooms': result, 'inventory': stats}, indent=2) + '\n')
    print('additional_windows', sum(len(r['windows']) for r in result), flush=True)


if __name__ == '__main__':
    main()
