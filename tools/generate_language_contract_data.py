#!/usr/bin/env python3
"""Generate the portable C registry for the v1 semantic language catalog."""

import argparse
import fnmatch
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG = (
    ROOT / 'tools' / 'data' / 'localization' / 'semantic-catalog-v1.json')
DEFAULT_OUTPUT = ROOT / 'src' / 'localization' / 'language_contract_data.inc'
DEFAULT_GO_OUTPUT = (ROOT / 'installer' / 'internal' / 'localization' /
                     'data' / 'author-contracts.json')
DEFAULT_SHAPES = ROOT / 'tools/data/localization/presentation-shapes-v1.json'
DEFAULT_SHAPE_OUTPUT = ROOT / 'src/localization/language_row_shape_data.inc'
DEFAULT_KEYBOARD_OUTPUT = ROOT / 'src/localization/language_keyboard_shape.h'
DEFAULT_UNICODE_OUTPUT = ROOT / 'installer/internal/localization/unicode_grapheme_data.go'
UNICODE_SOURCE = ROOT / 'src/localization/unicode_grapheme_data.inc'
ROW_SHAPES = {
    'cities': 'kArLanguageRowShape_Cities',
    'score': 'kArLanguageRowShape_Score',
    'master': 'kArLanguageRowShape_Master',
    'fixed_rows': 'kArLanguageRowShape_FixedRows',
    'sound_test': 'kArLanguageRowShape_SoundTest',
    'message_speed': 'kArLanguageRowShape_MessageSpeed',
    'message_speed_jp': 'kArLanguageRowShape_MessageSpeedJP',
}
PROFILES = ('us', 'eu-en', 'de', 'fr', 'jp')
PRESENTATION_SHAPES = {
    'flow': 'kArLanguagePresentation_Flow',
    'fixed': 'kArLanguagePresentation_Fixed',
    'keyboard': 'kArLanguagePresentation_Keyboard',
    'inline': 'kArLanguagePresentation_Inline',
}
PLACEHOLDER_KINDS = {
    'localized_text': 'kArLanguagePlaceholder_LocalizedText',
    'localized_term': 'kArLanguagePlaceholder_LocalizedTerm',
    'number': 'kArLanguagePlaceholder_Number',
    'icon': 'kArLanguagePlaceholder_Icon',
}


def c_string(value):
    encoded = value.encode('utf-8')
    result = ['"']
    for byte in encoded:
        if byte == 0x22:
            result.append(r'\"')
        elif byte == 0x5c:
            result.append(r'\\')
        elif 0x20 <= byte <= 0x7e:
            result.append(chr(byte))
        else:
            result.append(f'\\x{byte:02x}')
    result.append('"')
    return ''.join(result)


def generate(catalog_path):
    raw = catalog_path.read_bytes()
    catalog = json.loads(raw)
    if catalog.get('format') != 'actraiser-language-semantic-catalog' or \
            catalog.get('version') != 1:
        raise ValueError('unsupported semantic catalog')
    if tuple(catalog.get('release_order', ())) != PROFILES:
        raise ValueError('semantic catalog profile order changed')

    placeholders = sorted(catalog['placeholders'])
    placeholder_index = {name: index for index, name in enumerate(placeholders)}
    routes = catalog['routes']
    if [route['id'] for route in routes] != sorted(
            route['id'] for route in routes):
        raise ValueError('semantic routes must be sorted by id')
    routes = sorted(routes + catalog.get('optional_routes', []), key=lambda r: r['id'])
    if len({route['id'] for route in routes}) != len(routes):
        raise ValueError('duplicate semantic route')
    coverage = catalog['us_runtime_coverage']
    live, dormant = set(coverage['live_optional']), set(coverage['dormant'])
    native = {r['id'] for r in routes if 'us' in r['contracts']}
    optional = {r['id'] for r in routes if r.get('optional') and 'us' in r['contracts']}
    if live & dormant or not live <= optional or not dormant <= native or optional != live | (dormant & optional):
        raise ValueError('classify every optional US route as live or dormant')

    for route in routes:
        shape = route.get('presentation', {}).get('shape')
        if shape not in PRESENTATION_SHAPES:
            raise ValueError(
                f'{route["id"]}: unknown presentation shape {shape!r}')

    route_placeholder_indices = []
    anchor_names = []
    contracts = []
    generated_routes = []
    for route in routes:
        placeholder_first = len(route_placeholder_indices)
        route_placeholder_indices.extend(
            placeholder_index[name] for name in route['allowed_placeholders'])
        contract_first = len(contracts)
        profile_mask = 0
        for profile_index, profile in enumerate(PROFILES):
            contract = route['contracts'].get(profile)
            if contract is None:
                continue
            profile_mask |= 1 << profile_index
            anchor_first = len(anchor_names)
            anchor_names.extend(
                anchor['id'] for anchor in contract['required_anchors'])
            contracts.append((profile_index, anchor_first,
                              len(contract['required_anchors'])))
        presentation = route['presentation']
        generated_routes.append((
            route['id'], placeholder_first,
            len(route['allowed_placeholders']), contract_first,
            len(contracts) - contract_first, profile_mask,
            PROFILES.index(route['canonical_contract_profile']),
            int(route.get('optional', False)),
            PRESENTATION_SHAPES[presentation['shape']],
            presentation.get('maximum_pages', 0),
            presentation.get('maximum_lines', 0),
            presentation.get('required_nonempty_lines', 0)))

    if max((len(placeholders), len(contracts), len(anchor_names),
            len(route_placeholder_indices))) > 0xffff:
        raise ValueError('generated semantic registry exceeds uint16 indexes')

    lines = [
        '/* Generated by tools/generate_language_contract_data.py.',
        ' * Do not edit by hand. Contains semantic IDs and contracts only;',
        ' * it contains no retail wording, ROM addresses, or extracted bytes.',
        f' * Catalog SHA-256: {hashlib.sha256(raw).hexdigest()}',
        ' */',
        '',
        'static const ArGeneratedPlaceholder kGeneratedPlaceholders[] = {',
    ]
    for name in placeholders:
        kind = PLACEHOLDER_KINDS.get(catalog['placeholders'][name])
        if kind is None:
            raise ValueError(
                f'unknown placeholder kind {catalog["placeholders"][name]!r}')
        lines.append(f'  {{{c_string(name)}, {kind}}},')
    lines.extend(['};', '',
                  'static const uint16_t kGeneratedRoutePlaceholders[] = {'])
    for index in route_placeholder_indices:
        lines.append(f'  UINT16_C({index}),')
    lines.extend(['};', '',
                  'static const char *const kGeneratedAnchorNames[] = {'])
    for name in anchor_names:
        lines.append(f'  {c_string(name)},')
    lines.extend(['};', '',
                  'static const ArGeneratedContract kGeneratedContracts[] = {'])
    for profile, first, count in contracts:
        lines.append(
            f'  {{{profile}, UINT16_C({first}), UINT16_C({count})}},')
    lines.extend(['};', '',
                  'static const ArGeneratedRoute kGeneratedRoutes[] = {'])
    for (route_id, placeholder_first, placeholder_count, contract_first,
         contract_count, profile_mask, canonical_profile, optional, shape,
         maximum_pages, maximum_lines,
         required_nonempty_lines) in generated_routes:
        lines.append(
            f'  {{{c_string(route_id)}, UINT16_C({placeholder_first}), '
            f'UINT16_C({placeholder_count}), UINT16_C({contract_first}), '
            f'UINT16_C({contract_count}), UINT16_C({maximum_lines}), '
            f'UINT8_C({profile_mask}), '
            f'{canonical_profile}, {optional}, {shape}, '
            f'UINT8_C({maximum_pages}), '
            f'UINT8_C({required_nonempty_lines})}},')
    lines.extend(['};', ''])
    return '\n'.join(lines)


def load_row_shapes(catalog_path, shapes_path):
    data = json.loads(shapes_path.read_bytes())
    if data.get('format') != 'actraiser-language-presentation-shapes' or \
            data.get('version') != 1 or set(data['tables']) != set(ROW_SHAPES):
        raise ValueError('unsupported row-shape registry')
    for shape, rules in data['tables'].items():
        if not rules:
            raise ValueError(f'{shape}: empty row rules')
        for rule in rules:
            if not 0 <= rule['first_line'] <= rule['last_line'] <= 255:
                raise ValueError(f'{shape}: invalid line range')
            fields = rule.get('fields', [])
            reserved = rule.get('native_reserved', False)
            if (not reserved and not fields) or (reserved and fields) or \
                    fields != sorted(set(fields)) or \
                    any(type(n) is not int or not 1 <= n <= 10 for n in fields):
                raise ValueError(f'{shape}: invalid field counts')
    catalog = json.loads(catalog_path.read_bytes())
    routes = catalog['routes'] + catalog.get('optional_routes', [])
    bindings = {}
    for pattern, shape in data['bindings'].items():
        if shape not in ROW_SHAPES:
            raise ValueError(f'{pattern}: unknown row shape')
        matched = [r for r in routes if fnmatch.fnmatchcase(r['id'], pattern)]
        if not matched:
            raise ValueError(f'{pattern}: unused row binding')
        for route in matched:
            if route['id'] in bindings or route['presentation']['shape'] != 'fixed':
                raise ValueError(f'{pattern}: conflicting/non-fixed row binding')
            bindings[route['id']] = shape
    profile_bindings = data.get('profile_bindings', {})
    for profile, overrides in profile_bindings.items():
        if profile not in PROFILES:
            raise ValueError(f'{profile}: unknown row-shape profile')
        for route_id, shape in overrides.items():
            if route_id not in bindings or shape not in ROW_SHAPES:
                raise ValueError(f'{route_id}: invalid profile row binding')
    return data['tables'], bindings, profile_bindings


def generate_row_shapes(catalog_path, shapes_path):
    tables, bindings, profiles = load_row_shapes(catalog_path, shapes_path)
    lines = ['/* Generated by tools/generate_language_contract_data.py.',
             ' * ROM-free content shapes; no pixel geometry or retail text. */',
             'static const RowRule kRowRules[] = {']
    for shape, rules in tables.items():
        for rule in rules:
            mask = sum(1 << count for count in rule.get('fields', []))
            lines.append(f'  {{{ROW_SHAPES[shape]}, {rule["first_line"]}, '
                         f'{rule["last_line"]}, {mask}, '
                         f'{str(rule.get("native_reserved", False)).lower()}}},')
    lines.extend(['};', '', 'static const RowBinding kRowBindings[] = {'])
    for route_id, shape in sorted(bindings.items()):
        lines.append(f'  {{{c_string(route_id)}, {ROW_SHAPES[shape]}}},')
    lines.extend(['};', '', 'static const ProfileRowBinding kProfileRowBindings[] = {'])
    for profile, overrides in sorted(profiles.items()):
        for route_id, shape in sorted(overrides.items()):
            lines.append(f'  {{{c_string(route_id)}, {PROFILES.index(profile)}, '
                         f'{ROW_SHAPES[shape]}}},')
    return '\n'.join(lines + ['};', ''])


def generate_go(catalog_path, shapes_path=DEFAULT_SHAPES):
    """Only portable author contracts; no native layouts/addresses or prose."""
    catalog = json.loads(catalog_path.read_bytes())
    # Run the existing schema/order checks before either consumer is emitted.
    generate(catalog_path)
    tables, bindings, profiles = load_row_shapes(catalog_path, shapes_path)
    keyboard = load_keyboard(catalog_path, shapes_path)
    def presentation(route, overrides):
        shape = overrides.get(route['id'], bindings.get(route['id']))
        return {**route['presentation'], **({'keyboard': {
            k: v for k, v in keyboard.items() if k != 'id'}}
            if route['id'] == keyboard['id'] else {}), **({'table': {
            'kind': shape, 'rules': tables[shape]}} if shape else {})}
    return json.dumps({
        'placeholders': catalog['placeholders'],
        'routes': [{
            'id': route['id'],
            'allowed_placeholders': route['allowed_placeholders'],
            'canonical_profile': route['canonical_contract_profile'],
            'us_runtime_usage': ('live_optional' if route['id'] in catalog['us_runtime_coverage']['live_optional']
                                 else 'dormant' if route['id'] in catalog['us_runtime_coverage']['dormant']
                                 else 'contract' if 'us' in route['contracts'] else 'regional_reference'),
            **({'optional': True} if route.get('optional') else {}),
            'presentation': presentation(route, {}),
            **({'presentation_by_profile': {
                profile: presentation(route, overrides)
                for profile, overrides in profiles.items() if route['id'] in overrides
            }} if any(route['id'] in overrides for overrides in profiles.values()) else {}),
            'anchors': {profile: [anchor['id'] for anchor in contract['required_anchors']]
                        for profile, contract in route['contracts'].items()},
        } for route in sorted(catalog['routes'] + catalog.get('optional_routes', []), key=lambda r: r['id'])],
    }, ensure_ascii=False, sort_keys=True, indent=2) + '\n'


def load_keyboard(catalog_path, shapes_path):
    keyboard = json.loads(shapes_path.read_bytes())['keyboard']
    catalog = json.loads(catalog_path.read_bytes())
    route = next(r for r in catalog['routes'] if r['id'] == keyboard['id'])
    if route['presentation']['shape'] != 'keyboard' or \
            not 1 <= keyboard['rows'] < keyboard['maximum_lines'] <= 64 or \
            not 1 <= keyboard['columns'] <= 255 or \
            not 1 <= keyboard['maximum_page_bytes'] <= 3072:
        raise ValueError('unsupported playable keyboard shape')
    return keyboard


def generate_keyboard(catalog_path, shapes_path):
    keyboard = load_keyboard(catalog_path, shapes_path)
    return '\n'.join([
        '/* Generated by tools/generate_language_contract_data.py. */',
        '#ifndef AR_LANGUAGE_KEYBOARD_SHAPE_H',
        '#define AR_LANGUAGE_KEYBOARD_SHAPE_H',
        f'#define AR_LANGUAGE_KEYBOARD_ROUTE {c_string(keyboard["id"])}',
        'enum {',
        f'  kArLanguageKeyboardRows = {keyboard["rows"]},',
        f'  kArLanguageKeyboardColumns = {keyboard["columns"]},',
        f'  kArLanguageKeyboardMaximumLines = {keyboard["maximum_lines"]},',
        f'  kArLanguageKeyboardMaximumPageBytes = {keyboard["maximum_page_bytes"]},',
        '};', '#endif', ''])


def generate_go_unicode():
    # Reuse the game's exact Unicode version/properties; this is a projection
    # of its embedded data, not another dependency or a shipped Python oracle.
    source = UNICODE_SOURCE.read_text()
    rows = re.findall(r'\{(0x[0-9A-F]+)u, (0x[0-9A-F]+)u, (0x[0-9A-F]+)u\}', source)
    if len(rows) != source.count('  {') or not rows:
        raise ValueError('unsupported game grapheme property data')
    lines = ['// Code generated by tools/generate_language_contract_data.py; DO NOT EDIT.',
             '// Properties from src/localization/unicode_grapheme_data.inc.',
             '// Unicode data license: third_party/unicode/NOTICE.',
             '', 'package localization', '',
             'var graphemeRanges = [...]graphemeRange{']
    lines.extend('\t{' + ', '.join(row) + '},' for row in rows)
    return '\n'.join(lines + ['}', ''])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--catalog', type=Path, default=DEFAULT_CATALOG)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument('--go-out', type=Path, default=DEFAULT_GO_OUTPUT)
    parser.add_argument('--shapes', type=Path, default=DEFAULT_SHAPES)
    parser.add_argument('--shape-out', type=Path, default=DEFAULT_SHAPE_OUTPUT)
    parser.add_argument('--keyboard-out', type=Path, default=DEFAULT_KEYBOARD_OUTPUT)
    parser.add_argument('--unicode-out', type=Path, default=DEFAULT_UNICODE_OUTPUT)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    outputs = {args.out: generate(args.catalog),
               args.go_out: generate_go(args.catalog, args.shapes),
               args.shape_out: generate_row_shapes(args.catalog, args.shapes),
               args.keyboard_out: generate_keyboard(args.catalog, args.shapes),
               args.unicode_out: generate_go_unicode()}
    if args.check:
        for path, output in outputs.items():
            if not path.is_file() or path.read_text(encoding='utf-8') != output:
                raise SystemExit(
                    f'{path}: generated language contract data is stale')
        print('language contract data is current')
        return
    for path, output in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(output, encoding='utf-8')
        print(f'wrote {path}')


if __name__ == '__main__':
    main()
