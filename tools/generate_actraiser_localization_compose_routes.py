#!/usr/bin/env python3
"""Generate address-only USA fixed-composer routes from private extraction.

The checked manifest and C include contain semantic IDs, native addresses,
destinations, ownership geometry, and indexed-source identities only. They
never contain retail wording, decoded operations, raw bytes, or record hashes.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from generate_actraiser_localization_routes import (
    checked_json, load_extractor, parse_pc24, pc24, source_for_route,
)


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = (
    ROOT / 'tools/data/localization/us-runtime-compose-routes-v1.json')
DEFAULT_OUTPUT = (
    ROOT / 'src/actraiser/actraiser_localization_compose_route_data.inc')
FORMAT = 'actraiser-us-runtime-compose-routes'
VERSION = 1


SURFACE_KINDS = {
    'menu_heading': {
        'surface_id': 2, 'destination': 0x0512,
        'region': [18, 5, 10, 2], 'native_font_pixels': 7,
    },
    'menu_selection': {
        'surface_id': 3, 'destination': 0x0A12,
        'region': [18, 10, 10, 2], 'native_font_pixels': 7,
    },
    'city_name': {
        'surface_id': 4, 'destination': 0x0106,
        # Row 2 belongs to the live angel-health bar, not the town label.
        'region': [6, 1, 12, 1], 'native_font_pixels': 7,
    },
    'name_entry': {
        'surface_id': 5, 'destination': 0x0703,
        'region': [3, 7, 27, 16], 'native_font_pixels': 8,
    },
    'status_master': {
        'surface_id': 6, 'destination': 0x060A,
        'region': [10, 6, 12, 17], 'native_font_pixels': 7,
    },
    'status_table': {
        'surface_id': 7, 'destination': 0x0603,
        'region': [3, 6, 26, 20], 'native_font_pixels': 8,
    },
    'flow_choice': {
        'surface_id': 8, 'destination': 0x0B17,
        'region': [23, 11, 6, 5], 'native_font_pixels': 7,
    },
    'flow_speed': {
        'surface_id': 9, 'destination': 0x0C12,
        'region': [18, 12, 10, 4], 'native_font_pixels': 7,
    },
}


def kind_for(semantic_id):
    if semantic_id.startswith('sky.menu.magic.') or \
            semantic_id.startswith('sim.menu.possession.'):
        return 'menu_selection'
    if semantic_id.startswith(('sky.menu.', 'sim.menu.')):
        return 'menu_heading'
    if semantic_id.startswith('city.') and semantic_id.endswith('.name'):
        return 'city_name'
    if semantic_id == 'name_entry.prompt_and_alphabet':
        return 'name_entry'
    if semantic_id == 'status.report.master_report':
        return 'status_master'
    if semantic_id in ('status.report.cities_report',
                       'status.report.score_report'):
        return 'status_table'
    if semantic_id == 'system.choice.yes_no':
        return 'flow_choice'
    if semantic_id == 'system.message_speed.scale_labels':
        return 'flow_speed'
    return None


def indexed_identity(semantic_id, source, pointer_tables):
    if semantic_id.startswith('sim.menu.possession.slot_'):
        table = pointer_tables['selected_possession']
        selector = int(semantic_id.rsplit('_', 1)[1])
    elif semantic_id.startswith('sky.menu.magic.'):
        table = pointer_tables['selected_magic']
        targets = [parse_pc24(value) for value in table['target_pc24s']]
        matches = [index for index, target in enumerate(targets)
                   if target == source]
        if len(matches) != 1:
            raise ValueError(
                f'{semantic_id}: selected-magic source is not unique')
        selector = matches[0]
    else:
        return None
    targets = [parse_pc24(value) for value in table['target_pc24s']]
    if selector >= len(targets) or targets[selector] != source:
        raise ValueError(f'{semantic_id}: indexed source identity changed')
    return parse_pc24(table['source_table_pc24']), selector


def build_manifest(extraction_path, rom_path):
    extractor = load_extractor()
    extraction = checked_json(extraction_path)
    rom = rom_path.read_bytes()
    source_identity = extraction.get('source', {})
    if source_identity.get('release_id') != 'us':
        raise ValueError('runtime route generation requires the USA extraction')
    digest = hashlib.sha256(rom).hexdigest()
    if digest != source_identity.get('rom_sha256'):
        raise ValueError('ROM does not match the extraction source hash')
    if not extraction.get('coverage', {}).get('complete') or not \
            extraction.get('semantic_route_catalog', {}).get('complete'):
        raise ValueError('runtime route generation requires complete extraction')

    records = {
        record['id']: record for record in
        extraction['messages'] + extraction['menu_source_catalog']['segments']
    }
    pointer_tables = {
        table['id']: table for table in
        extraction['consumer_census']['fixed_composer_sources'][
            'pointer_tables']
    }
    rows = []
    for semantic_route in extraction['semantic_route_catalog']['routes']:
        semantic_id = semantic_route['id']
        kind = kind_for(semantic_id)
        if kind is None or semantic_route['route_kind'] != 'consumer_route':
            continue
        route_source = source_for_route(
            semantic_route, records, extractor)
        geometry = SURFACE_KINDS[kind]
        row = {
            'semantic_id': semantic_id,
            'source_pc24': pc24(route_source),
            'destination': geometry['destination'],
            'surface_kind': kind,
            'surface_id': geometry['surface_id'],
            'region': geometry['region'],
            'native_font_pixels': geometry['native_font_pixels'],
        }
        indexed = indexed_identity(
            semantic_id, route_source, pointer_tables)
        if indexed is not None:
            row['source_table_pc24'] = pc24(indexed[0])
            row['source_selector'] = indexed[1]
        rows.append(row)

    rows.sort(key=lambda row: (
        parse_pc24(row['source_pc24']), row['destination'],
        parse_pc24(row.get('source_table_pc24', '$00:8000')),
        row.get('source_selector', -1), row['semantic_id']))
    identities = set()
    for row in rows:
        identity = (row['source_pc24'], row['destination'],
                    row.get('source_table_pc24'),
                    row.get('source_selector'))
        if identity in identities:
            raise ValueError(
                f"{row['semantic_id']}: ambiguous compose route identity")
        identities.add(identity)
    if not rows:
        raise ValueError('no runtime fixed-composer routes were generated')
    return {
        'format': FORMAT,
        'version': VERSION,
        'source_profile': 'us',
        'rom_sha256': digest,
        'route_count': len(rows),
        'routes': rows,
    }


def validate_manifest(manifest):
    if set(manifest) != {
            'format', 'version', 'source_profile', 'rom_sha256',
            'route_count', 'routes'}:
        raise ValueError('compose route manifest has unknown or missing keys')
    if manifest.get('format') != FORMAT or manifest.get('version') != VERSION:
        raise ValueError('unsupported compose route manifest')
    if manifest.get('source_profile') != 'us' or not re.fullmatch(
            r'[0-9a-f]{64}', manifest.get('rom_sha256', '')):
        raise ValueError('compose route manifest has invalid source identity')
    routes = manifest.get('routes')
    if not isinstance(routes, list) or manifest.get('route_count') != len(routes):
        raise ValueError('compose route count is inconsistent')
    identities = set()
    previous_source = -1
    for row in routes:
        required = {'semantic_id', 'source_pc24', 'destination',
                    'surface_kind', 'surface_id', 'region',
                    'native_font_pixels'}
        optional = {'source_table_pc24', 'source_selector'}
        if not required <= set(row) or not set(row) <= required | optional:
            raise ValueError('compose route has unknown or missing keys')
        semantic_id = row['semantic_id']
        if not isinstance(semantic_id, str) or not re.fullmatch(
                r'[A-Za-z][A-Za-z0-9_.-]*', semantic_id):
            raise ValueError('compose route has invalid semantic ID')
        source = parse_pc24(row['source_pc24'])
        if source < previous_source:
            raise ValueError('compose routes are not source-address sorted')
        previous_source = source
        if row['surface_kind'] not in SURFACE_KINDS or \
                row['surface_id'] != SURFACE_KINDS[row['surface_kind']][
                    'surface_id'] or \
                row['destination'] != SURFACE_KINDS[row['surface_kind']][
                    'destination'] or \
                row['region'] != SURFACE_KINDS[row['surface_kind']]['region'] or \
                row['native_font_pixels'] != SURFACE_KINDS[
                    row['surface_kind']]['native_font_pixels']:
            raise ValueError(f'{semantic_id}: invalid surface geometry')
        if not isinstance(row['destination'], int) or not \
                0 <= row['destination'] <= 0xffff:
            raise ValueError(f'{semantic_id}: invalid destination')
        table = row.get('source_table_pc24')
        selector = row.get('source_selector')
        if (table is None) != (selector is None):
            raise ValueError(f'{semantic_id}: incomplete indexed identity')
        if table is not None:
            parse_pc24(table)
            if not isinstance(selector, int) or not 0 <= selector <= 0xffff:
                raise ValueError(f'{semantic_id}: invalid source selector')
        identity = (row['source_pc24'], row['destination'], table, selector)
        if identity in identities:
            raise ValueError(f'{semantic_id}: duplicate compose identity')
        identities.add(identity)
    return routes


def c_u32(value):
    return f'UINT32_C(0x{value:06X})'


def generate_c(manifest_path):
    raw = manifest_path.read_bytes()
    routes = validate_manifest(json.loads(raw))
    lines = [
        '/* Generated by',
        ' * tools/generate_actraiser_localization_compose_routes.py.',
        ' * Do not edit by hand. Contains address-only USA adapter metadata;',
        ' * it contains no retail wording, decoded operations, or raw bytes.',
        f' * Manifest SHA-256: {hashlib.sha256(raw).hexdigest()}',
        ' */',
        '',
        'static const ActRaiserLocalizationComposeRoute kComposeRoutes[] = {',
    ]
    for row in routes:
        flags = []
        if 'source_table_pc24' in row:
            flags.append('kActRaiserLocalizationComposeRouteMatch_SourceTable')
            flags.append('kActRaiserLocalizationComposeRouteMatch_Selector')
        lines.extend((
            '  {',
            f'    .source_pc24 = {c_u32(parse_pc24(row["source_pc24"]))},',
            f'    .source_table_pc24 = '
            f'{c_u32(parse_pc24(row["source_table_pc24"]))},'
            if 'source_table_pc24' in row else '',
            f'    .semantic_id = {json.dumps(row["semantic_id"])},',
            f'    .surface_id = UINT32_C({row["surface_id"]}),',
            f'    .destination = UINT16_C(0x{row["destination"]:04X}),',
            f'    .region = {{{", ".join(str(v) for v in row["region"])}}},',
            f'    .source_selector = UINT16_C({row.get("source_selector", 0)}),',
            f'    .native_font_pixels = UINT8_C('
            f'{row["native_font_pixels"]}),',
            f'    .match_flags = {" | ".join(flags) if flags else "0"},',
            '  },',
        ))
    lines.extend(('};', ''))
    return '\n'.join(line for line in lines if line != '') + '\n'


def write_or_check(path, value, check):
    encoded = value.encode('utf-8')
    if check:
        if not path.exists() or path.read_bytes() != encoded:
            raise ValueError(f'{path}: generated output is stale')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded)
    print(f'wrote {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--manifest', type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument('--extraction', type=Path)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--update-manifest', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        if args.update_manifest:
            if args.check or args.extraction is None or args.rom is None:
                raise ValueError(
                    '--update-manifest requires --extraction and --rom, and '
                    'cannot be combined with --check')
            manifest = build_manifest(args.extraction, args.rom)
            write_or_check(
                args.manifest,
                json.dumps(manifest, indent=2, sort_keys=True) + '\n', False)
        elif args.extraction is not None or args.rom is not None:
            raise ValueError(
                '--extraction/--rom are only valid with --update-manifest')
        write_or_check(args.output, generate_c(args.manifest), args.check)
        if args.check:
            print('ActRaiser localization compose route data is current')
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f'generate_actraiser_localization_compose_routes: {error}',
              file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
