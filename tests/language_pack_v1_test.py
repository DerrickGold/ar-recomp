#!/usr/bin/env python3
import importlib.util
import json
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'language_pack_v1', ROOT / 'tools' / 'language_pack_v1.py')
PACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACK)


def expect_error(function, text):
    try:
        function()
    except PACK.LanguagePackError as error:
        assert text in str(error), str(error)
    else:
        raise AssertionError(f'expected LanguagePackError containing {text!r}')


def route(route_id, operations, category='town_dialogue'):
    encoded = json.dumps(
        operations, sort_keys=True, separators=(',', ':')).encode()
    return {
        'id': route_id,
        'source_record_id': 'synthetic.' + route_id,
        'source_offset_within_record': 0,
        'source_category': category,
        'source_operations': operations,
        'source_operation_count': len(operations),
        'source_operations_sha256': __import__('hashlib').sha256(
            encoded).hexdigest(),
    }


def extraction(release_id, routes):
    locales = {
        'us': 'en-US', 'eu-en': 'en-GB', 'de': 'de-DE',
        'fr': 'fr-FR', 'jp': 'ja-JP',
    }
    return {
        'format': 'actraiser-language-extraction',
        'locale': locales[release_id],
        'source': {'release_id': release_id},
        'coverage': {'complete': True},
        'semantic_route_catalog': {'routes': routes},
    }


def native_break_tests():
    layout = {'columns': 12, 'space_delimited_words': True}

    def converted(left, right, geometry=layout, category='town_dialogue'):
        left_ops = [{'op': 'text', 'value': left}] if isinstance(left, str) else left
        right_ops = [{'op': 'text', 'value': right}] if isinstance(right, str) else right
        return PACK.source_operations_to_author(route('test.wrap', [
            *left_ops, {'op': 'line_break'}, *right_ops, {'op': 'end'},
        ], category), geometry)

    def hard(operations):
        return any(op['op'] == 'line' for op in operations)

    assert hard(converted('Hello', 'world again'))  # Short intentional line.
    assert hard(converted('Hello!', 'world again'))  # Exact 12-cell fit.
    soft = converted('Hello!!', 'world again')  # Whole word exceeds by one.
    assert not hard(soft)
    assert soft[0]['value'] == 'Hello!! world again'
    assert hard(converted('Hello!!', 'a longer phrase'))  # Only the first word.
    assert hard(converted('  Hello!    ', '    world again'))  # Dictionary padding.
    for accent in ('é', 'e\u0301'):
        assert hard(converted(accent * 6, 'world again'))  # Not UTF-8 byte count.
        assert not hard(converted(accent * 7, 'world again'))
    for dynamic in ({'op': 'insert_master_name'},
                    {'op': 'format_number', 'value': 'total_score', 'width': 0x86},
                    {'op': 'insert_indexed_text', 'value': 'town_name'}):
        assert hard(converted([{'op': 'text', 'value': 'Title '}, dynamic], 'welcome'))
    assert hard(converted('Hello!', [
        {'op': 'format_number', 'value': 'total_population', 'width': 5}]))
    assert not hard(converted('Hello!!', [
        {'op': 'format_number', 'value': 'total_population', 'width': 5}]))
    assert hard(converted('', 'world'))
    assert hard(converted('Hello!!', ''))
    assert hard(converted('Hello!!', 'world', None))  # Old extraction: no geometry.
    assert hard(converted('Hello!!', 'world', {'columns': 12}))
    assert hard(converted('Hello!!', 'world',
                          {'columns': 12, 'space_delimited_words': False}))
    for category in ('ending_text', 'post_offering_or_ending_native', 'fixed_composer'):
        assert hard(converted('Hello!!', 'world', category=category))
    for boundary in ('page_break', 'reset_text_cursor', 'yield', 'end'):
        source = route('test.boundary', [
            {'op': 'text', 'value': 'Previous long text'}, {'op': boundary},
            {'op': 'text', 'value': 'Hi'}, {'op': 'line_break'},
            {'op': 'text', 'value': 'there'}, {'op': boundary},
            {'op': 'line_break'}, {'op': 'line_break'},
        ])
        assert PACK.native_line_breaks(source, layout) == {3, 6, 7}
    source = route('test.round_trip', [
        {'op': 'text', 'value': 'Title '}, {'op': 'insert_master_name'},
        {'op': 'line_break'}, {'op': 'text', 'value': 'Hello!!'},
        {'op': 'line_break'}, {'op': 'text', 'value': 'world again'},
        {'op': 'yield'}, {'op': 'end'},
    ])
    operations = PACK.source_operations_to_author(source, layout)
    emitted = PACK.emit_artext([{'id': source['id'], 'operations': operations}])
    assert 'Title {master_name}\n@line\nHello!! world again' in emitted
    assert PACK.canonical_messages(PACK.parse_artext_text(emitted)) == \
        PACK.canonical_messages([{'id': source['id'], 'operations': operations}])
    # Equal source bytes cannot alias routes with different line/layout policies.
    other = route('test.fixed', source['source_operations'], 'fixed_composer')
    ir = extraction('us', [source, other])
    ir['native_dialogue_layout'] = layout
    messages = PACK.build_source_messages(ir)
    assert all('alias' not in message for message in messages)


def main():
    native_break_tests()
    numbers = PACK.source_operations_to_author(route('test.numbers', [
        {'op': 'format_number', 'value': 'total_population', 'width': 3},
        {'op': 'format_number', 'value': 'score_fillmore_act_1', 'width': 0x84},
        {'op': 'format_number', 'value': 'total_score', 'width': 0x86},
        {'op': 'end'},
    ], 'fixed_composer'))
    assert numbers[0]['minimum_digits'] == 3
    assert 'minimum_digits' not in numbers[1]
    assert 'minimum_digits' not in numbers[2]

    # Source conversion produces explicit cells and collapses multipart art;
    # the renderer must not guess columns from words in a translated label.
    speed = PACK.source_operations_to_author(route(
        'system.message_speed.scale_labels', [
            {'op': 'text', 'value': '0123456789  '},
            {'op': 'line_break'}, {'op': 'line_break'},
            {'op': 'text', 'value': 'Quick'},
            {'op': 'insert_icon', 'value': 'ui.speed_direction', 'part_index': 0},
            {'op': 'insert_icon', 'value': 'ui.speed_direction', 'part_index': 1},
            {'op': 'text', 'value': 'Gradual'}, {'op': 'end'},
        ], 'fixed_composer'))
    assert speed[0]['value'] == '0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9'
    assert sum(op['op'] == 'placeholder' for op in speed) == 1
    title = PACK.fixed_table_operations([
        {'op': 'text', 'value': 'TOTAL PEOPLE'}, {'op': 'line'},
        {'op': 'text', 'value': 'IN THIS CITY   '},
        {'op': 'placeholder', 'name': 'total_population'}, {'op': 'end'},
    ], 'status.report.cities_report')
    assert title[0]['value'] == 'TOTAL PEOPLE'
    assert title[2]['value'] == 'IN THIS CITY'
    assert title[3]['value'] == ' | '

    formatted = PACK.parse_artext_text(
        ':: status.report.master_report\n{master_level:03}\n@end\n')
    assert formatted[0]['operations'][0]['minimum_digits'] == 3
    assert PACK.canonical_messages(PACK.parse_artext_text(
        PACK.emit_artext(formatted))) == PACK.canonical_messages(formatted)
    for bad_format in ('00', '010', '3', '0', ''):
        expect_error(lambda: PACK.parse_artext_text(
            ':: test.format\n{master_level:' + bad_format + '}\n'),
            'number format')

    for category in ('town_dialogue', 'system_dialogue'):
        addressed = PACK.source_operations_to_author(route('test.address', [
            {'op': 'text', 'value': 'Title '},
            {'op': 'insert_master_name'},
            {'op': 'text', 'value': ' what next?'},
            {'op': 'end'},
        ], category))
        assert PACK.emit_artext([{'id': 'test.address',
                                 'operations': addressed}]).find(
                                     'Title {master_name} what next?') >= 0
    for suffix in (' next', ', welcome'):
        addressed = PACK.source_operations_to_author(route('test.name', [
            {'op': 'insert_master_name'},
            {'op': 'text', 'value': suffix}, {'op': 'end'},
        ]))
        assert addressed[1]['value'] == suffix

    manifest = PACK.parse_manifest_text('''
[pack]
format = actraiser-language-pack
version = 1
id = example.fr-ca
locale = fr-CA
name = Canadian French
autonym = Français canadien
author = Example Author
license = CC-BY-4.0
direction = auto
target = us-runtime
source_profile = us
fallback = native-us
coverage = partial

[fonts]
primary = builtin:actraiser-sans
fallback = fonts/Example.ttf

[scripts]
source = text/sky.artext
source = text/simulation.artext
''')
    assert manifest['pack']['locale'] == 'fr-CA'
    assert manifest['fonts']['fallback'] == ['fonts/Example.ttf']
    assert manifest['scripts'] == [
        'text/sky.artext', 'text/simulation.artext']
    expect_error(lambda: PACK.parse_manifest_text('''
[pack]
format = actraiser-language-pack
version = 1
id = bad
locale = en_CA
name = Bad
autonym = Bad
author = Bad
license = Bad
direction = ltr
target = us-runtime
source_profile = us
fallback = native-us
coverage = partial
[fonts]
primary = builtin:actraiser-sans
[scripts]
source = ../bad.artext
'''), 'invalid BCP-47 locale')

    script = '''
:: sky.demo
@anchor reset_text_cursor.00
Bienvenue, {master_name}.
Cette ligne physique continue le même paragraphe.

Un nouveau paragraphe avec {{accolades}}.
@page
Une page ajoutée.
@wait 30
@end

:: sky.empty
@empty

:: sky.alias
@alias sky.demo

:: sky.escaped
@@visible command
\\#visible comment
'''
    messages = PACK.parse_artext_text(script)
    assert len(messages) == 4
    demo = messages[0]
    assert [operation['op'] for operation in demo['operations']] == [
        'anchor', 'text', 'placeholder', 'text', 'text', 'paragraph',
        'text', 'page', 'text', 'wait', 'end']
    assert demo['operations'][4]['value'].startswith(' ')
    assert demo['operations'][6]['value'].endswith('{accolades}.')
    emitted = PACK.emit_artext(messages)
    reparsed = PACK.parse_artext_text(emitted)
    assert PACK.canonical_messages(reparsed) == PACK.canonical_messages(messages)
    assert reparsed[1]['operations'][0]['op'] == 'empty'
    assert reparsed[2]['alias'] == 'sky.demo'
    assert reparsed[3]['operations'][0]['value'] == \
        '@visible command #visible comment'

    shared_by_release = {}
    releases = ('us', 'eu-en', 'de', 'fr', 'jp')
    for release in releases:
        placeholder = ('town_name' if release == 'jp' else 'master_name')
        operations = [
            {'op': 'reset_text_cursor', 'confidence': 'mapped'},
            {'op': 'text', 'value': 'Synthetic'},
            ({'op': 'insert_master_name'} if placeholder == 'master_name'
             else {'op': 'insert_indexed_text', 'value': 'town_name'}),
            {'op': 'end'},
        ]
        shared_by_release[release] = route('sky.demo', operations)
    extra_jp = route('variant.jp.sky.extra', [
        {'op': 'text', 'value': 'Synthetic regional text'}, {'op': 'end'}])
    extractions = [
        extraction(release, [shared_by_release[release]] +
                   ([extra_jp] if release == 'jp' else []))
        for release in releases
    ]
    catalog = PACK.build_semantic_catalog(extractions)
    assert catalog['route_count'] == 2
    assert catalog['all_release_route_count'] == 1
    assert catalog['regional_or_release_variant_route_count'] == 1
    assert catalog['placeholders'] == {
        'master_name': 'localized_text', 'town_name': 'localized_text'}
    shared_contract = catalog['routes'][0]
    assert shared_contract['allowed_placeholders'] == [
        'master_name', 'town_name']

    # A translation may add or remove presentation pages and can use a typed
    # value observed in another official language, but it cannot alter the
    # U.S. route's machine-owned control anchor.
    translated = PACK.parse_artext_text('''
:: sky.demo
@anchor reset_text_cursor.00
Page courte pour {town_name}.
@page
Page supplémentaire.
''')
    stats = PACK.validate_messages(
        translated, catalog, 'us', coverage='complete')
    assert stats['message_count'] == 1
    no_anchor = PACK.parse_artext_text('''
:: sky.demo
Le contrôle manque.
''')
    expect_error(
        lambda: PACK.validate_messages(no_anchor, catalog, 'us'),
        'locked anchors changed')
    bad_placeholder = PACK.parse_artext_text('''
:: sky.demo
@anchor reset_text_cursor.00
{unavailable_value}
''')
    expect_error(
        lambda: PACK.validate_messages(bad_placeholder, catalog, 'us'),
        'is unavailable on this route')
    long_wait = PACK.parse_artext_text('''
:: sky.demo
@anchor reset_text_cursor.00
Texte.
@wait 601
''')
    expect_error(
        lambda: PACK.validate_messages(long_wait, catalog, 'us'),
        '@wait must be')
    alias_cycle = PACK.parse_artext_text('''
:: sky.demo
@alias variant.jp.sky.extra
:: variant.jp.sky.extra
@alias sky.demo
''')
    expect_error(
        lambda: PACK.validate_messages(alias_cycle, catalog, 'us'),
        'alias cycle')

    statuses = PACK.parse_progress_text(
        'sky.demo\twip\nvariant.jp.sky.extra\tdone\n',
        {'sky.demo', 'variant.jp.sky.extra'})
    assert statuses == {'sky.demo': 'wip', 'variant.jp.sky.extra': 'done'}
    expect_error(
        lambda: PACK.parse_progress_text('sky.demo\tlater\n'),
        'invalid progress status')

    # Every official source profile can be represented and validated using
    # its own extracted control contract. Generated files remain temporary.
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        catalog_path = root / 'catalog.json'
        PACK.write_json(catalog_path, catalog)
        for source in extractions:
            release = source['source']['release_id']
            pack_root = root / release
            metadata = {
                'id': f'local.{release}',
                'locale': source['locale'],
                'name': f'{release} source',
                'autonym': release,
                'author': 'Local test',
                'license': 'ROM-derived-local-only',
                'direction': 'auto',
                'target': ('us-runtime' if release == 'us'
                           else 'reference-only'),
                'source_profile': release,
                'coverage': 'complete',
            }
            PACK.write_source_pack(source, pack_root, metadata)
            result = PACK.validate_pack(pack_root, catalog)
            assert result['valid']
            assert result['message_count'] == len(
                source['semantic_route_catalog']['routes'])

    print('language pack v1 authoring checks passed')


if __name__ == '__main__':
    main()
