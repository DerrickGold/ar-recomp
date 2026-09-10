#!/usr/bin/env python3
"""Differential gate for Go native readers and whole-game source coverage.

Without ROMs, compare independently decoded synthetic data. With --rom for
all five releases, require the verified Python whole-ROM census to pass and
compare interactive/fixed records, known-call census, bounded inventory,
reference expansion, ownership, destination/graphical audits and logical route
union, including positive/negative mutations. Require exact complete coverage
reports for all five ROMs. This does NOT certify author-format export, visual
font/graphics output, builder integration or runtime rendering support.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

import language_pack_extract as extract
from generate_go_localization_profiles import generated, generated_sources, generated_catalogs, generated_destinations
from go_localization_source_vectors import reference_sources, source_mutations, synthetic_source_groups
from go_localization_catalog_vectors import reference_catalog, catalog_mutations
from go_localization_destination_vectors import synthetic_destination_groups, destination_mutations

ROOT = Path(__file__).resolve().parents[1]


def reference_case(decoder, consumer, start, limit, stop=True, label=''):
    reader = decoder if consumer == 'interactive' else extract.FixedComposerDecoder(decoder)
    expected = reader.decode_record(start, limit, stop) if consumer == 'interactive' \
        else reader.decode_record(start, limit)
    return {'id': label, 'consumer': consumer, 'start': start, 'limit': limit,
            'stop_on_yield': stop, 'expected': expected}


def synthetic_groups():
    facts = {p['id']: p for p in json.loads(generated())['profiles']}
    rng = random.Random(0x41525458)
    groups = []
    for original in extract.ROM_PROFILES.values():
        profile = dict(original, dictionary=120000, dialogue_name_prefix=119000,
                       dialogue_name_separator=' ')
        go_profile = dict(facts[profile['id']], dictionary=120000,
                          name_prefix=119000, name_separator=' ')
        rom = bytearray(0x20000)
        rom[119000:119003] = b'AZ\0'
        if profile['encoding'] == 'dictionary-12':
            for index in range(128):
                word = bytearray(b'ABCDEFGHIJKL')
                word[index % 12] = (0x00, 0x20, 0x40)[index % 3]
                rom[120000+index*12:120012+index*12] = word
        call = go_profile['speed_source_call']
        speed = None
        if call is not None:
            site = extract.pc24_to_offset(call)
            rom[site-3:site] = bytes((0xa0, 0x00, 0xfc))
            speed = extract.pc24_to_offset((call & 0xff0000) | 0xfc06)
        population = go_profile['population_source']
        # Same atlas slots must remain punctuation outside these consumers.
        if population is not None:
            codes = (0x5b, 0x5c) if profile['id'] == 'fr' else (0x3a, 0x3b)
            rom[population:population+3] = bytes((*codes, 0))
        if speed is not None:
            codes = (0x1d, 0x1c) if profile['id'] == 'jp' else (0x3d, 0x3c)
            rom[speed:speed+3] = bytes((*codes, 0))

        spans = []
        position = 0

        def add(data, label):
            nonlocal position
            start = position
            position += len(data)
            assert position < 0x8000, 'synthetic records overlap contextual sources'
            rom[start:position] = data
            spans.append((start, position, label))

        for code in range(256):
            add(bytes((ord('A'), code, ord('Z'), 0x40, 3, 0x80, 0x40, 0x40, 0)),
                f'byte-{code:02x}')
        for code, count in extract.CONTROL_ARGUMENTS.items():
            for available in range(count):
                add(bytes((code,)) + b'A' * available, f'truncated-{code:02x}-{available}')
        for code in (0x20, 0x40, 0x00, 0x01, 0xde, 0xdf):
            add(bytes((0x0b, 3, 0x80, code, 0xff, 0)), f'boundary-{code:02x}')
        for address in profile.get('number_semantics', {}):
            native = int(address.lstrip('$'), 16)
            for width in (0, 1, 2, 5, 255):
                add(bytes((9, width, native & 255, native >> 8, 0)),
                    f'number-{address}-{width}')
        for arguments in profile.get('indexed_text_semantics', {}):
            add(bytes((8,)) + bytes.fromhex(arguments) + bytes((0,)),
                f'indexed-{arguments}')
        if profile['encoding'] == 'direct-glyph':
            for code in profile['glyph_overrides']:
                for sequence in ((code, 0xde, 0), (code, 0xdf, 0),
                                 (code, 0xde, 0xde, 0), (code, 0xde, 0xdf, 0)):
                    add(bytes(sequence), 'kana-' + bytes(sequence).hex())
        for index in range(200):
            add(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 48))),
                f'generated-{index}')
        for source, label in ((population, 'population'), (speed, 'speed')):
            if source is not None:
                spans.append((source, source+3, label))
        decoder = extract.Decoder(bytes(rom), profile)
        cases = [reference_case(decoder, consumer, start, limit, stop, label)
                 for start, limit, label in spans
                 for consumer in ('interactive', 'fixed') for stop in (False, True)]
        groups.append({'id': 'synthetic-' + profile['id'],
                       'rom': base64.b64encode(rom).decode('ascii'),
                       'profile': go_profile, 'cases': cases})
    return groups


def official_groups(paths):
    extractions, groups = [], []
    for path in paths:
        profile, ir = extract.inspect_rom(path)
        extractions.append((profile, ir))
        decoder = extract.Decoder(path.read_bytes(), profile)
        cases = []
        for consumer, records in (('interactive', ir['messages']),
                                  ('fixed', ir['menu_source_catalog']['segments'])):
            for record in records:
                source = record['source']
                start = int(source['file_offset'], 16)
                limit = int(source['end_file_offset_exclusive'], 16)
                stop = record['operations'][-1]['op'] == 'yield'
                cases.append(reference_case(decoder, consumer, start, limit,
                                            stop, record['id']))
        source_expected = reference_sources(profile, path.read_bytes())
        catalog_expected = reference_catalog(profile, path.read_bytes())
        groups.append({'id': profile['id'], 'rom_path': str(path.resolve()), 'cases': cases,
                       'source_expected': source_expected,
                       'catalog_expected': catalog_expected,
                       'catalog_mutations': catalog_mutations(profile, path.read_bytes(), catalog_expected),
                       'destination_mutations': destination_mutations(profile, path.read_bytes(), catalog_expected),
                       'source_mutations': source_mutations(profile, path.read_bytes(), source_expected)})
        print(f"Censused {profile['id']}: {len(cases)} native reader records", flush=True)
    if paths:
        ids = [profile['id'] for profile, _ in extractions]
        if len(ids) != 5 or set(ids) != {'us', 'eu-en', 'de', 'fr', 'jp'}:
            raise ValueError('ROM parity requires all five distinct supported releases')
        extract.refresh_extraction_coverage(extractions)
        for profile, ir in extractions:
            if not ir['coverage']['complete']:
                raise ValueError(f"{profile['id']}: authoritative extraction census is incomplete")
    if paths:
        groups[-1]['alignment_expected'] = extract.apply_cross_release_semantic_alignment(
            [(profile, group['catalog_expected']) for (profile, _), group in zip(extractions, groups)])
        # The union is an independent result, not an extra native-catalog field.
        for group in groups:
            del group['catalog_expected']['cross_release_semantic_alignment']
        groups[-1]['coverage_expected'] = [ir['coverage'] for _, ir in extractions]
    return groups, [{'id': profile['id'], 'sha256': ir['source']['rom_sha256'],
                     'reference_census_complete': ir['coverage']['complete']}
                    for profile, ir in extractions]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--go', default='go')
    parser.add_argument('--rom', type=Path, action='append', default=[])
    parser.add_argument('--report', type=Path)
    parser.add_argument('--race', action='store_true', help='Run the Go race detector on the full comparison')
    args = parser.parse_args()
    subprocess.run([sys.executable, str(ROOT / 'tools/generate_go_localization_profiles.py'), '--check'], check=True)
    groups = synthetic_groups() + synthetic_source_groups()
    official, census = official_groups(args.rom)
    groups.extend(official)
    destinations = synthetic_destination_groups()
    with tempfile.TemporaryDirectory(prefix='ar-go-decoder-parity-') as temporary:
        vectors = Path(temporary) / 'vectors.json'
        vectors.write_text(json.dumps(groups, ensure_ascii=False), encoding='utf-8')
        destination_vectors = Path(temporary) / 'destinations.json'
        destination_vectors.write_text(json.dumps(destinations, ensure_ascii=False), encoding='utf-8')
        env = dict(os.environ, AR_LOCALIZATION_DECODER_VECTORS=str(vectors),
                   AR_LOCALIZATION_DESTINATION_VECTORS=str(destination_vectors))
        command = [args.go, 'test', './internal/localizationkit', '-count=1', '-v']
        if args.race:
            command.append('-race')
        subprocess.run(command,
                       cwd=ROOT / 'snesrecomp-go', env=env, check=True,
                       timeout=300 if args.rom else 120)
    summary = {'gate': 'native_readers_catalogues_and_whole_game_source_coverage', 'passed': True, 'race_detector': args.race,
               'groups': [{'id': group['id'], 'record_count': len(group['cases']),
                           'source_reference_count': group.get('source_expected', {}).get('source_reference_seeds', {}).get('reference_count', 0),
                           'source_mutation_cases': len(group.get('source_mutations', [])),
                           'positive_source_mutations': sum(not case['reject'] for case in group.get('source_mutations', [])),
                           'catalogue_record_count': len(group.get('catalog_expected', {}).get('messages', [])),
                           'composer_record_count': len(group.get('catalog_expected', {}).get('menu_source_catalog', {}).get('segments', [])),
                           'semantic_route_count': group.get('catalog_expected', {}).get('semantic_route_catalog', {}).get('route_count', 0),
                           'catalogue_mutation_cases': len(group.get('catalog_mutations', [])),
                           'positive_catalogue_mutations': sum(not case['reject'] for case in group.get('catalog_mutations', [])),
                           'destination_mutation_cases': len(group.get('destination_mutations', [])),
                           'positive_destination_mutations': sum(not case['reject'] for case in group.get('destination_mutations', [])),
                           'graphical_resource_count': group.get('catalog_expected', {}).get('graphical_text_census', {}).get('resource_count', 0)}
                          for group in groups],
               'destination_synthetic_cases': sum(len(group['cases']) for group in destinations),
               'destination_synthetic_rejections': sum(case.get('reject', False) for group in destinations for case in group['cases']),
               'whole_game_coverage': next((group['coverage_expected'] for group in groups if 'coverage_expected' in group), None),
               'cross_release_alignment': next(({key: value for key, value in group['alignment_expected'].items() if key != 'routes'}
                                                for group in groups if 'alignment_expected' in group), None),
               'official_reference_census': census,
               'reference_sha256': hashlib.sha256((ROOT / 'tools/language_pack_extract.py').read_bytes()).hexdigest(),
               'decoder_sha256': hashlib.sha256((ROOT / 'snesrecomp-go/internal/localizationkit/decoder.go').read_bytes()).hexdigest(),
               'profile_sha256': hashlib.sha256(generated().encode('utf-8')).hexdigest(),
               'source_profile_sha256': hashlib.sha256(generated_sources().encode('utf-8')).hexdigest(),
               'catalogue_profile_sha256': hashlib.sha256(generated_catalogs().encode('utf-8')).hexdigest(),
               'destination_profile_sha256': hashlib.sha256(generated_destinations().encode('utf-8')).hexdigest(),
               'source_code_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                      for path in sorted((ROOT / 'snesrecomp-go/internal/localizationkit').glob('source*.go'))},
               'catalogue_code_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                        for path in sorted((ROOT / 'snesrecomp-go/internal/localizationkit').glob('catalog*.go'))},
               'destination_code_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                          for path in sorted((ROOT / 'snesrecomp-go/internal/localizationkit').glob('*.go'))
                                          if path.stem.startswith(('destination', 'asset_', 'graphical_', 'coverage'))},
               'remaining_gates': ['author export including intentional wrapping and aliases',
                                   'font/graphical output and mapping validation', 'builder installation and UI']}
    if args.report:
        args.report.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print('Native reader/catalogue/destination parity passed; ' +
          ('five-ROM whole-game coverage agrees. ' if args.rom else 'run with all five ROMs for whole-game coverage. ') +
          'Author export, asset output and builder integration remain open.', flush=True)


if __name__ == '__main__':
    main()
