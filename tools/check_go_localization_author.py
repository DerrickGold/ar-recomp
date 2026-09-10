#!/usr/bin/env python3
"""Optional development oracle for the Go author/editor core.

The primary compatibility gate is the direct Go/C test in CTest; it needs no
Python. This additional comparison reads Python-produced scripts (optionally
from all five verified ROMs), compares every parsed operation/source line and
checks them with the same C probe. Retail cases additionally run Go directly
from each original ROM and compare generated script/manifest/progress bytes.
This does NOT certify archive import/export, fonts, GUI installation or rendering.
No generated retail text is retained outside a temporary directory.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from unittest import mock

import language_pack_extract as extract
import language_pack_v1 as pack
from generate_language_contract_data import DEFAULT_CATALOG, DEFAULT_GO_OUTPUT, generate_go

ROOT = Path(__file__).resolve().parents[1]


def resolved_presentations(script):
    messages = {m['id']: m for m in pack.parse_artext_text(script)}
    result = {}
    for key, message in messages.items():
        seen = set()
        while message.get('alias'):
            if message['id'] in seen:
                raise ValueError('cyclic reference alias')
            seen.add(message['id'])
            message = messages[message['alias']]
        result[key] = pack.canonical_messages([message])[0]['operations']
    return result


def cases(roms):
    catalog = json.loads(DEFAULT_CATALOG.read_bytes())
    output = []

    def add(label, profile, coverage, script):
        metadata = {
            'id': 'test.' + profile, 'locale': {'us': 'en-CA', 'eu-en': 'en-GB',
                    'de': 'de-DE', 'fr': 'fr-FR', 'jp': 'ja-JP'}[profile],
            'name': 'Author oracle ' + profile, 'autonym': 'Oracle',
            'author': 'Development fixture', 'license': 'Local test only',
            'direction': 'auto', 'target': 'us-runtime' if profile == 'us' else 'reference-only',
            'source_profile': profile, 'coverage': coverage,
        }
        manifest = pack.manifest_text(metadata)
        case = {'id': label, 'profile': profile, 'coverage': coverage,
                'sources': {'text/source.artext': script}, 'manifest': manifest,
                'manifest_metadata': pack.parse_manifest_text(manifest)}
        try:
            messages = pack.parse_artext_text(script, 'text/source.artext')
            case['stats'] = pack.validate_messages(messages, catalog, profile, coverage)
            case['messages'] = [{key: value for key, value in message.items()
                                 if key != 'ended'} for message in messages]
            case['progress'] = pack.progress_text(
                [m['id'] for m in messages],
                {m['id']: ('not_started', 'wip', 'done')[i % 3]
                 for i, m in enumerate(messages)})
            case['valid'] = True
        except pack.LanguagePackError:
            case['valid'] = False
        output.append(case)

    confirm = (':: sky.action_mode.confirm\n@anchor reset_text_cursor.00\n'
               'Excellent, {master_name}!\n@anchor yield.01\n@end\n')
    add('synthetic/confirm', 'us', 'partial', confirm)
    for newline in ('\n', '\r', '\r\n'):
        add('synthetic/newline-' + repr(newline), 'us', 'partial', confirm.replace('\n', newline))
    for text in ('@@{{braces}}\n\\#comment\n', '\u00a0\n@line\nx\u2028y\n',
                 'e\u0301lève 日本語 👩‍🚀\n', '@empty\n', '@page\n@wait 600\n',
                 '@wait 601\n', '{master_name:02}\n', '@event no.events\n',
                 '@alias action.hud.act_2\n', '@empty\nnot empty\n'):
        add('synthetic/body-' + str(len(output)), 'us', 'partial',
            ':: action.hud.act_1\n' + text)
    add('synthetic/alias', 'us', 'partial',
        ':: action.hud.act_1\n@alias action.hud.act_2\n'
        ':: action.hud.act_2\nMost excellent!\n')
    for broken in (confirm.replace('reset_text_cursor.00', 'wrong.anchor'),
                   confirm.replace('@end', '@page\n@end'),
                   confirm.replace('{master_name}', '{master_name:02}')):
        add('synthetic/rejected-' + str(len(output)), 'us', 'partial', broken)

    if roms:
        extractions = [extract.inspect_rom(path) for path in roms]
        profiles = [profile['id'] for profile, _ in extractions]
        if len(profiles) != 5 or set(profiles) != {'us', 'eu-en', 'de', 'fr', 'jp'}:
            raise ValueError('developer retail comparison requires all five distinct supported ROMs')
        extract.refresh_extraction_coverage(extractions)
        for rom, (profile, ir) in zip(roms, extractions):
            if not ir['coverage']['complete']:
                raise ValueError(f"incomplete source census: {profile['id']}")
            script = pack.emit_artext(pack.build_source_messages(ir))
            # Explicit native-clear padding normalization must preserve the
            # existing parser's presentation for every retail route.
            with mock.patch.object(pack, 'strip_native_padding', lambda ops: ops):
                legacy = pack.emit_artext(pack.build_source_messages(ir))
            if resolved_presentations(script) != resolved_presentations(legacy):
                raise ValueError(f"padding normalization changed presentation: {profile['id']}")
            add('retail/' + profile['id'], profile['id'], 'complete', script)
            if not output[-1]['valid']:
                raise ValueError(f"oracle produced an invalid source pack: {profile['id']}")
            output[-1]['native_rom'] = str(rom.resolve())
            output[-1]['native_progress'] = pack.progress_text(
                [m['id'] for m in output[-1]['messages']])
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--go', default='go')
    parser.add_argument('--probe', type=Path, default=ROOT / 'build/actraiser_language_pack_runtime_test')
    parser.add_argument('--rom', type=Path, action='append', default=[])
    parser.add_argument('--race', action='store_true')
    args = parser.parse_args()
    if DEFAULT_GO_OUTPUT.read_text() != generate_go(DEFAULT_CATALOG):
        raise SystemExit('Go author contracts are stale')
    if not args.probe.is_file():
        raise SystemExit('build actraiser_language_pack_runtime_test or supply --probe')
    vectors = cases(args.rom)
    with tempfile.TemporaryDirectory(prefix='ar-author-oracle-') as temporary:
        path = Path(temporary) / 'cases.json'
        path.write_text(json.dumps(vectors, ensure_ascii=False), encoding='utf-8')
        env = dict(os.environ, AR_AUTHOR_ORACLE_CASES=str(path),
                   AR_AUTHOR_RUNTIME_PROBE=str(args.probe.resolve()))
        command = [args.go, 'test', './internal/localizationkit', '-count=1',
                   '-run', '^TestAuthorOracleParity$']
        if args.race:
            command.append('-race')
        subprocess.run(command, cwd=ROOT / 'snesrecomp-go', env=env, check=True)
    print(f"Author oracle: {len(vectors)} cases; "
          f"{sum(case.get('stats', {}).get('message_count', 0) for case in vectors)} "
          'accepted messages agree with Go and the production C loader')
    if args.rom:
        print('All five Go ROM-to-source packs match manifest/script/progress bytes; '
              'native-clear padding normalization preserves every prior route presentation')


if __name__ == '__main__':
    main()
