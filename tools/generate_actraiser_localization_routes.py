#!/usr/bin/env python3
"""Generate address-only USA runtime dialogue routes from private extraction.

The checked manifest and C include contain semantic IDs, native addresses,
control-flow selectors, and page-unit totals only. They never contain retail
wording, decoded operations, raw bytes, or source-record hashes.
"""

import argparse
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = (
    ROOT / 'tools/data/localization/us-runtime-dialogue-routes-v1.json')
DEFAULT_OUTPUT = (
    ROOT / 'src/actraiser/actraiser_localization_route_data.inc')
FORMAT = 'actraiser-us-runtime-dialogue-routes'
VERSION = 1
MAX_NATIVE_PAGES = 8
RUNTIME_PREFIXES = (
    'dialogue.event.', 'dialogue.offering.', 'sim.', 'simulation.event.',
    'sky.', 'system.', 'variant.western.sim.',
)


def load_extractor():
    spec = importlib.util.spec_from_file_location(
        'language_pack_extract', ROOT / 'tools/language_pack_extract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_pc24(value):
    if not isinstance(value, str) or not re.fullmatch(
            r'\$[0-7][0-9A-F]:[89A-F][0-9A-F]{3}', value):
        raise ValueError(f'invalid PC24 address {value!r}')
    return int(value[1:3], 16) << 16 | int(value[4:], 16)


def pc24(value):
    return f'${value >> 16:02X}:{value & 0xffff:04X}'


def checked_json(path):
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError(f'{path}: expected a JSON object')
    return value


def native_page_units(rom, extractor, source):
    position = extractor.pc24_to_offset(source)
    pages = []
    units = 0
    for _ in range(65536):
        if position >= len(rom):
            raise ValueError(f'{pc24(source)}: native stream leaves ROM')
        code = rom[position]
        position += 1
        units += 1
        if code in extractor.CONTROL_ARGUMENTS:
            position += extractor.CONTROL_ARGUMENTS[code]
            if position > len(rom):
                raise ValueError(f'{pc24(source)}: truncated native control')
        if code == 0x02:
            pages.append(units)
            units = 0
        elif code in (0x00, 0x01):
            pages.append(units)
            break
    else:
        raise ValueError(f'{pc24(source)}: unterminated native stream')
    if not pages or len(pages) > MAX_NATIVE_PAGES or any(
            not count or count > 0xffff for count in pages):
        raise ValueError(f'{pc24(source)}: invalid native page-unit totals')
    return pages


def source_for_route(route, records, extractor):
    record = records.get(route['source_record_id'])
    if record is None:
        raise ValueError(f"{route['id']}: source record is missing")
    source = parse_pc24(record['source']['snes'])
    source_offset = route.get('source_offset_within_record', 0)
    if not isinstance(source_offset, int) or source_offset < 0:
        raise ValueError(f"{route['id']}: invalid source offset")
    file_offset = extractor.pc24_to_offset(source) + source_offset
    return extractor.offset_to_pc24(file_offset)


def provenance_call(route, prefixes):
    for provenance in route.get('provenance', ()):
        kind, separator, address = provenance.partition(':')
        if separator and kind.startswith(prefixes):
            return kind, parse_pc24(address)
    raise ValueError(f"{route['id']}: no matching consumer provenance")


def build_manifest(extraction_path, rom_path):
    extractor = load_extractor()
    extraction = checked_json(extraction_path)
    rom = rom_path.read_bytes()
    source = extraction.get('source', {})
    if source.get('release_id') != 'us':
        raise ValueError('runtime route generation requires the USA extraction')
    digest = hashlib.sha256(rom).hexdigest()
    if digest != source.get('rom_sha256'):
        raise ValueError('ROM does not match the extraction source hash')
    if not extraction.get('coverage', {}).get('complete') or not \
            extraction.get('semantic_route_catalog', {}).get('complete'):
        raise ValueError('runtime route generation requires complete extraction')

    records = {
        record['id']: record for record in
        extraction['messages'] + extraction['menu_source_catalog']['segments']
    }
    census = extraction['consumer_census']
    wrappers = census['dialogue_forwarding']['wrappers']
    wrapper_text_calls = [
        parse_pc24(site['call_site']) for site in
        census['consumers'][0]['call_sites']
        if site['source_origin'] == 'dialogue_wrapper'
    ]
    if len(wrapper_text_calls) != len(wrappers):
        raise ValueError('dialogue wrapper/text-entry relationship changed')
    nested_calls = [
        parse_pc24(site['call_site']) for site in
        census['consumers'][0]['call_sites']
        if site['source_origin'] == 'nested_handler_table'
    ]
    if len(nested_calls) != 1:
        raise ValueError('simulation event dispatcher relationship changed')
    nested_call = nested_calls[0]

    nested_selectors = {}
    city_names = ('fillmore', 'bloodpool', 'kasandora',
                  'aitos', 'marahna', 'northwall')
    matrices = census['nested_handler_sources']['matrices']
    if len(matrices) != 1:
        raise ValueError('expected one simulation event pointer matrix')
    for row in matrices[0]['rows']:
        city = city_names[row['city_slot']]
        table = parse_pc24(row['source_table_pc24'])
        for slot in range(row['pointer_count']):
            nested_selectors[f'simulation.event.{city}.slot_{slot:02d}'] = (
                table & 0xffff) + slot * 2

    forwarded_wrapper = None
    for index, wrapper in enumerate(wrappers):
        forwarded = [site for site in wrapper['call_sites']
                     if site['source_origin'] ==
                     'forwarded_y_from_source_relay']
        if forwarded:
            if len(forwarded) != 1 or forwarded_wrapper is not None:
                raise ValueError('source relay wrapper relationship changed')
            forwarded_wrapper = (index, forwarded[0])
    if forwarded_wrapper is None:
        raise ValueError('source relay wrapper is missing')

    route_rows = []
    semantic_routes = extraction['semantic_route_catalog']['routes']
    for route in semantic_routes:
        route_id = route['id']
        if not route_id.startswith(RUNTIME_PREFIXES):
            continue
        if route['route_kind'] not in ('consumer_route', 'pointer_table_slot'):
            continue
        if route_id.startswith('dialogue.ending.'):
            continue
        if not any(
                provenance.startswith(('interpreter_', 'dialogue_wrapper_'))
                for provenance in route.get('provenance', ())):
            continue

        row = {
            'semantic_id': route_id,
            'source_pc24': pc24(source_for_route(route, records, extractor)),
        }
        if route_id.startswith('simulation.event.'):
            row['caller_pc24'] = pc24(nested_call + 3)
            row['selector_x'] = nested_selectors[route_id]
        elif route_id.startswith('dialogue.offering.'):
            _, call = provenance_call(route, ('interpreter_offering_',))
            row['caller_pc24'] = pc24(call + 3)
            slot = int(route_id.rsplit('_', 1)[1])
            row['selector_x'] = slot * 2
        elif route_id.startswith('dialogue.event.relay.'):
            wrapper_index, forwarded = forwarded_wrapper
            wrapper = wrappers[wrapper_index]
            row['caller_pc24'] = pc24(wrapper_text_calls[wrapper_index] + 3)
            outer_call = parse_pc24(forwarded['call_site'])
            row['context_pc24'] = pc24(
                outer_call + (4 if wrapper['call_kind'] == 'jsl' else 3))
            city = route_id.rsplit('.', 1)[1]
            row['map_number'] = city_names.index(city) + 1
        elif route_id.startswith('dialogue.event.wrapper_'):
            match = re.match(r'dialogue\.event\.wrapper_(\d{2})\.', route_id)
            if not match:
                raise ValueError(f'{route_id}: malformed wrapper identity')
            wrapper_index = int(match.group(1))
            wrapper = wrappers[wrapper_index]
            _, outer_call = provenance_call(route, ('dialogue_wrapper_',))
            row['caller_pc24'] = pc24(wrapper_text_calls[wrapper_index] + 3)
            row['context_pc24'] = pc24(
                outer_call + (4 if wrapper['call_kind'] == 'jsl' else 3))
        else:
            _, call = provenance_call(route, ('interpreter_',))
            row['caller_pc24'] = pc24(call + 3)

        route_source = parse_pc24(row['source_pc24'])
        row['native_page_units'] = native_page_units(
            rom, extractor, route_source)
        source_operations = route.get('source_operations', ())
        expected_pages = 1 + sum(
            operation.get('op') == 'page_break'
            for operation in source_operations)
        if expected_pages != len(row['native_page_units']):
            raise ValueError(
                f'{route_id}: source operations/native pages disagree')
        route_rows.append(row)

    route_rows.sort(key=lambda row: (
        parse_pc24(row['source_pc24']), parse_pc24(row['caller_pc24']),
        parse_pc24(row.get('context_pc24', '$00:8000')),
        row.get('selector_x', -1), row.get('map_number', -1),
        row['semantic_id']))
    identities = set()
    for row in route_rows:
        identity = (
            row['source_pc24'], row['caller_pc24'], row.get('context_pc24'),
            row.get('selector_x'), row.get('map_number'))
        if identity in identities:
            raise ValueError(
                f"{row['semantic_id']}: ambiguous runtime route identity")
        identities.add(identity)
    if not route_rows:
        raise ValueError('no runtime dialogue routes were generated')

    return {
        'format': FORMAT,
        'version': VERSION,
        'source_profile': 'us',
        'rom_sha256': digest,
        'route_count': len(route_rows),
        'routes': route_rows,
    }


def validate_manifest(manifest):
    if set(manifest) != {
            'format', 'version', 'source_profile', 'rom_sha256',
            'route_count', 'routes'}:
        raise ValueError('runtime route manifest has unknown or missing keys')
    if manifest.get('format') != FORMAT or manifest.get('version') != VERSION:
        raise ValueError('unsupported runtime route manifest')
    if manifest.get('source_profile') != 'us' or not re.fullmatch(
            r'[0-9a-f]{64}', manifest.get('rom_sha256', '')):
        raise ValueError('runtime route manifest has invalid source identity')
    routes = manifest.get('routes')
    if not isinstance(routes, list) or manifest.get('route_count') != len(routes):
        raise ValueError('runtime route count is inconsistent')
    previous_source = -1
    identities = set()
    for row in routes:
        allowed_keys = {
            'semantic_id', 'source_pc24', 'caller_pc24', 'context_pc24',
            'selector_x', 'map_number', 'native_page_units'}
        if not set(row) <= allowed_keys or not {
                'semantic_id', 'source_pc24', 'caller_pc24',
                'native_page_units'} <= set(row):
            raise ValueError('runtime route has unknown or missing keys')
        semantic_id = row.get('semantic_id')
        if not isinstance(semantic_id, str) or not re.fullmatch(
                r'[A-Za-z][A-Za-z0-9_.-]*', semantic_id):
            raise ValueError('runtime route has invalid semantic ID')
        source = parse_pc24(row.get('source_pc24'))
        parse_pc24(row.get('caller_pc24'))
        if source < previous_source:
            raise ValueError('runtime routes are not source-address sorted')
        previous_source = source
        context = row.get('context_pc24')
        if context is not None:
            parse_pc24(context)
        selector = row.get('selector_x')
        if selector is not None and (not isinstance(selector, int) or
                                     not 0 <= selector <= 0xffff):
            raise ValueError(f'{semantic_id}: invalid selector X')
        map_number = row.get('map_number')
        if map_number is not None and (not isinstance(map_number, int) or
                                       not 0 <= map_number <= 0xff):
            raise ValueError(f'{semantic_id}: invalid map number')
        pages = row.get('native_page_units')
        if not isinstance(pages, list) or not 1 <= len(pages) <= \
                MAX_NATIVE_PAGES or any(
                    not isinstance(value, int) or not 1 <= value <= 0xffff
                    for value in pages):
            raise ValueError(f'{semantic_id}: invalid page-unit totals')
        identity = (row['source_pc24'], row['caller_pc24'], context,
                    selector, map_number)
        if identity in identities:
            raise ValueError(f'{semantic_id}: duplicate runtime identity')
        identities.add(identity)
    return routes


def c_u32(value):
    return f'UINT32_C(0x{value:06X})'


def generate_c(manifest_path):
    raw = manifest_path.read_bytes()
    manifest = json.loads(raw)
    routes = validate_manifest(manifest)
    lines = [
        '/* Generated by tools/generate_actraiser_localization_routes.py.',
        ' * Do not edit by hand. Contains address-only USA adapter metadata;',
        ' * it contains no retail wording, decoded operations, or raw bytes.',
        f' * Manifest SHA-256: {hashlib.sha256(raw).hexdigest()}',
        ' */',
        '',
        'static const ActRaiserLocalizationRoute kDialogueRoutes[] = {',
    ]
    for row in routes:
        flags = ['kActRaiserLocalizationRouteMatch_Caller']
        if 'context_pc24' in row:
            flags.append('kActRaiserLocalizationRouteMatch_Context')
        if 'selector_x' in row:
            flags.append('kActRaiserLocalizationRouteMatch_SelectorX')
        if 'map_number' in row:
            flags.append('kActRaiserLocalizationRouteMatch_MapNumber')
        units = ', '.join(str(value) for value in row['native_page_units'])
        lines.extend((
            '  {',
            f'    .source_pc24 = {c_u32(parse_pc24(row["source_pc24"]))},',
            f'    .caller_pc24 = {c_u32(parse_pc24(row["caller_pc24"]))},',
            f'    .context_pc24 = {c_u32(parse_pc24(row["context_pc24"]))},'
            if 'context_pc24' in row else '',
            f'    .selector_x = UINT16_C(0x{row.get("selector_x", 0):04X}),',
            f'    .semantic_id = {json.dumps(row["semantic_id"])},',
            '    .surface_id = kDialogueSurface,',
            '    .region = {5, 19, 23, 7},',
            f'    .native_page_count = {len(row["native_page_units"])},',
            f'    .native_page_units = {{{units}}},',
            '    .native_font_pixels = 7,',
            f'    .match_flags = {" | ".join(flags)},',
            f'    .map_number = UINT8_C({row.get("map_number", 0)}),',
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
            print('ActRaiser localization route data is current')
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f'generate_actraiser_localization_routes: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
