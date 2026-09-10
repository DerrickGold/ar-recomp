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


def main():
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
