#!/usr/bin/env python3
"""Inventory and review compiled Voxel surfaces without policy-membership gates.

The production C exporter owns geometry/material classification. This module
compares immutable surface results, records review candidates, and keeps every
unsupported or unreviewed route pending. A successful export is not acceptance.
Native artwork is private input/output; synthetic regression data is public.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
import math
from pathlib import Path
import sys


SIDES = ('top', 'right', 'bottom', 'left')


def _art(surface, key):
    pixels = surface.get(key + '_art')
    size = surface.get(key + '_size')
    if pixels is None:
        return None
    if (not isinstance(size, (list, tuple)) or len(size) != 2 or
            any(not isinstance(v, int) or v <= 0 for v in size) or
            len(pixels) != size[0] * size[1]):
        raise ValueError(f"Invalid {key} artwork dimensions for {surface['id']}")
    if any(not isinstance(v, int) or not 0 <= v <= 65535 for v in pixels):
        raise ValueError(f"Invalid {key} artwork pixels for {surface['id']}")
    return pixels, size


def _detail(art):
    if art is None:
        return {'available': False, 'rows': 0, 'columns': 0, 'colors': 0}
    pixels, (w, h) = art
    return {'available': True,
            'rows': len({tuple(pixels[y*w:(y+1)*w]) for y in range(h)}),
            'columns': len({tuple(pixels[x::w]) for x in range(w)}),
            'colors': len({v for v in pixels if v & 0x8000})}


def _finding(rule, ids, **kw):
    return {'rule': rule, 'surface_ids': ids, 'confidence': 'review', **kw}


def analyze_scene(data):
    """Analyze normalized surfaces. Candidates require review; no auto-fixes.

    Every input surface survives with a pending disposition. External visual
    evidence may later supply a disposition keyed by content signature; this
    function deliberately cannot accept a surface merely from texture flags.
    """
    out = {k: data.get(k) for k in ('scene_id', 'room', 'terrain', 'section', 'scope')}
    out.update(surfaces=[], findings=[], routes=[], counts={}, complete=False)
    seen = set()
    adjacency = defaultdict(list)
    for source in data.get('surfaces', []):
        s = dict(source)
        sid = s.get('id')
        if not isinstance(sid, str) or not sid or sid in seen:
            raise ValueError(f'Duplicate or missing surface id: {sid}')
        seen.add(sid)
        bounds, depth = s.get('bounds'), s.get('depth', [0, 0])
        if (not isinstance(bounds, (tuple, list)) or len(bounds) != 4 or
                any(not isinstance(v, (float, int)) or not math.isfinite(v) for v in bounds) or
                bounds[2] < 0 or bounds[3] < 0):
            raise ValueError(f'Invalid surface bounds: {sid}')
        if (not isinstance(depth, (tuple, list)) or len(depth) != 2 or
                any(not isinstance(v, (float, int)) or not math.isfinite(v) for v in depth)):
            raise ValueError(f'Invalid surface depth: {sid}')
        source_art, rendered_art = _art(s, 'source'), _art(s, 'rendered')
        native, output = _detail(source_art), _detail(rendered_art)
        result = {k: v for k, v in s.items() if k not in ('source_art', 'rendered_art')}
        result.update(disposition='pending', source_detail=native, rendered_detail=output)
        if s.get('mapping') == 'missing':
            out['findings'].append(_finding('missing_surface_mapping', [sid]))
        if not source_art:
            result['pending_reason'] = 'source_art_unavailable'
        elif s.get('mapping') == 'missing':
            result['pending_reason'] = 'missing_mapping_or_geometry'
        else:
            result['pending_reason'] = 'visual_review_required'
        detail_axis = ('rows' if s.get('texture_mode') == 3 else
                       'columns' if s.get('orientation') in ('left', 'right') else 'rows')
        visible = s.get('authored_face_visible', True)
        if not visible:
            result['pending_reason'] = 'authored_face_intentionally_hidden'
        if visible and s.get('mapping') == 'native_extrusion' and native[detail_axis] > 1 and native['colors'] > 1:
            out['findings'].append(_finding('untextured_source_detail', [sid],
                reason='Exposed face repeats contour ink while native source has two-dimensional detail. May be intentional; inspect full object.'))
        if (visible and s.get('mapping') == 'assigned' and native[detail_axis] > 1 and
                output['available'] and output[detail_axis] <= 1 and native['colors'] > 1):
            out['findings'].append(_finding('source_detail_flattened', [sid],
                reason='Assigned face has no variation across extrusion despite detailed native source. Verify intended donor.'))
        if s.get('mapping') == 'assigned' and not output['available']:
            result['pending_reason'] = 'assigned_sampler_requires_render_review'
        # No policy/family/palette/priority whitelist: adjoining tops may cross
        # all of those boundaries. It is a candidate, never an automatic defect.
        if visible and s.get('orientation') in SIDES:
            axis = 1 if s['orientation'] in ('left', 'right') else 0
            if bounds[axis+2] > 0:
                adjacency[(s['orientation'], round(bounds[1-axis], 4), axis)].append(s)
        out['surfaces'].append(result)
    for (_, _, axis), values in adjacency.items():
        ends = defaultdict(list)
        for s in values:
            ends[round(s['bounds'][axis] + s['bounds'][axis+2], 4)].append(s)
        for b in values:
            for a in ends.get(round(b['bounds'][axis], 4), []):
                if a['id'] == b['id']:
                    continue
                if max(min(a['depth']), min(b['depth'])) > min(max(a['depth']), max(b['depth'])) + 1e-5:
                    continue
                assigned_a = a.get('mapping') == 'assigned'
                assigned_b = b.get('mapping') == 'assigned'
                if assigned_a != assigned_b:
                    out['findings'].append(_finding('mixed_adjacent_mapping', [a['id'], b['id']],
                        reason='A continuous edge changes between assigned artwork and contour extrusion; compare the native transition.'))
    expected = data.get('expected_surface_count')
    if expected is not None and (not isinstance(expected, int) or expected < 0):
        raise ValueError('Invalid expected surface count')
    if expected is not None and expected > len(seen):
        out['findings'].append(_finding('missing_expected_surfaces', [],
            expected=expected, inventoried=len(seen)))
    for source in data.get('expected_routes', []):
        route = dict(source)
        if route.get('status') == 'not_applicable' and route.get('count') == 0:
            route['disposition'] = 'not_applicable'
            out['routes'].append(route)
            continue
        route['disposition'] = 'pending'
        if route.get('count') is None or route.get('status') != 'inventoried':
            route['pending_reason'] = route.get('reason', 'route_not_fully_inventoried')
        else:
            route['pending_reason'] = 'visual_review_required'
        out['routes'].append(route)
    out['counts'] = {'inventoried_surfaces': len(seen), 'expected_surfaces': expected,
                     'pending_surfaces': len(seen), 'candidate_events': len(out['findings']),
                     'pending_routes': sum(r['disposition']=='pending' for r in out['routes'])}
    return out


def summarize(rows):
    rows = list(rows)
    counts = Counter()
    for r in rows:
        for k, v in r['counts'].items():
            if isinstance(v, int):
                counts[k] += v
    return {'scene_snapshots': len(rows), 'counts': dict(counts),
            'complete': bool(rows) and all(r.get('complete', False) for r in rows),
            'status': 'review_complete' if rows and all(r.get('complete', False) for r in rows) else 'review_pending'}


def _hex_art(value):
    return [int(value[i:i+4], 16) for i in range(0, len(value), 4)] if value else None


def _signature(surface):
    # Coordinates are excluded: repeated identical treatment may share a visual
    # review. Context joins are audited separately and never inherit this vote.
    fields = {k: surface.get(k) for k in ('route', 'orientation', 'source_art', 'source_size',
        'rendered_art', 'rendered_size', 'mapping', 'material_key', 'depth', 'semantic',
        'sample_offset', 'sample_size', 'native_x', 'curve', 'authored_fields')}
    return hashlib.sha256(json.dumps(fields, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def main():
    tool_dir = str(Path(__file__).resolve().parent)
    if tool_dir not in sys.path:
        sys.path.insert(0, tool_dir)
    from voxel_audit.normalize import normalize_export, normalize_legacy, read_json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--details-dir', type=Path,
                        help='Write per-scene inventories as gzip files instead of holding every surface in RAM')
    args = parser.parse_args()
    files = sorted([*args.input.rglob('*.json'), *args.input.rglob('*.json.gz')]) if args.input.is_dir() else [args.input]
    destination = args.output.resolve()
    if destination in {file.resolve() for file in files} or (
            args.input.is_dir() and destination.is_relative_to(args.input.resolve())):
        parser.error('Output must be separate from the input inventory tree')
    if args.details_dir and (args.details_dir.resolve() == args.input.resolve() or
            args.input.is_dir() and args.details_dir.resolve().is_relative_to(args.input.resolve())):
        parser.error('Details must be separate from the input inventory tree')
    if args.details_dir:
        args.details_dir.mkdir(parents=True, exist_ok=True)
    tool_hash = hashlib.sha256(Path(__file__).read_bytes() +
        (Path(__file__).parent/'voxel_audit/normalize.py').read_bytes()).hexdigest()
    rows = []
    for number,file in enumerate(files,1):
        data = read_json(file)
        if 'surface_audit' in data:
            scope = next((p for p in file.parts if p in ('raw','shipped')), 'unknown')
            data = normalize_export(data, scope)
        elif 'edges' in data and 'layers' in data:
            data = normalize_legacy(data)
        if 'surfaces' not in data:
            raise ValueError(f'{file}: expected normalized surface inventory')
        row = analyze_scene(data)
        row['source'] = str(file.resolve())
        row['compiled_counts'] = data.get('compiled_counts')
        row['source_inventory_hash'] = data.get('source_inventory_hash')
        row['budgets'] = data.get('budgets')
        row['audit_tool_sha256'] = tool_hash
        if args.details_dir:
            identity = str(file.resolve()) + tool_hash + (row.get('source_inventory_hash') or '')
            name = hashlib.sha256(identity.encode()).hexdigest()[:20] + '.json.gz'
            detail = args.details_dir / name
            with gzip.open(detail, 'wt') as f:
                json.dump(row, f, separators=(',', ':'))
            row = {k:v for k,v in row.items() if k not in ('surfaces','findings')}
            row['detail'] = str(detail.resolve())
        rows.append(row)
        print(f"{number}/{len(files)} {file.name}: {row['counts']['inventoried_surfaces']} surfaces", file=sys.stderr, flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'summary': summarize(rows), 'scenes': rows}, indent=2) + '\n')
    print(json.dumps(summarize(rows), indent=2))


if __name__ == '__main__':
    main()
