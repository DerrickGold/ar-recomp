"""Private-in-memory differential vectors for the builder's source census.

This is test orchestration, not another extractor. Expectations are produced
by the current Python implementation; the Go library receives ROM bytes and
structural profiles, never the reference's resolved sources as inputs.
"""
import base64

import language_pack_extract as extract
from generate_go_localization_profiles import source_profile
from go_localization_catalog_vectors import synthetic_catalog_group


SOURCE_KEYS = ('consumers', 'dialogue_forwarding', 'nested_handler_sources',
               'fixed_composer_sources', 'source_reference_seeds')


def reference_sources(profile, rom):
    dictionary = extract.native_dictionary_consumers(profile, rom)
    layout = extract.native_dialogue_layout(profile, rom)
    census = extract.build_consumer_census(profile, rom)
    return {**{key: census[key] for key in SOURCE_KEYS},
            'native_dictionary_consumers': dictionary,
            'native_dialogue_layout': layout,
            # A known-call census must not claim whole-game coverage in Go.
            'whole_game_coverage_complete': False}


def source_mutations(profile, rom, expected):
    """Negative code proofs plus positive pointer/geometry changes.

    A valid mutation is compared in full; it must not be forced into a generic
    'ROM changed' error. Tests bypass retail hash identity only inside Go tests.
    """
    cases = []

    def mutate(label, offset, data, must_fail=False):
        changed = bytearray(rom)
        changed[offset:offset+len(data)] = data
        try:
            result = reference_sources(profile, bytes(changed))
        except (ValueError, IndexError):
            result = None
        if must_fail and result is not None:
            raise AssertionError(f'{label}: reference did not reject broken proof')
        if not must_fail and result is None:
            raise AssertionError(f'{label}: reference rejected positive mutation')
        if not must_fail and result == expected:
            raise AssertionError(f'{label}: positive mutation did not change census')
        cases.append({'id': label, 'offset': offset, 'data': list(data),
                      'reject': result is None, 'expected': result})

    census = extract.CONSUMER_CENSUS_PROFILES[profile['id']]
    for key, signature in (('interactive_entry_pc24', extract.INTERACTIVE_CONSUMER_SIGNATURE),
                           ('composer_entry_pc24', extract.FIXED_COMPOSER_SIGNATURE)):
        offset = extract.pc24_to_offset(census[key])
        for index in range(len(signature)):
            mutate(f'{key}-byte-{index}', offset+index,
                   bytes((rom[offset+index] ^ 1,)), must_fail=True)
    for call, _ in census.get('interactive_branch_join_layout', ()):
        offset = extract.pc24_to_offset(call)
        mutate(f'join-lost-fallthrough-{call:06x}', offset-3, b'\xea', must_fail=True)
        branch = extract.incoming_immediate_y_branches(rom, call)[0]['branch_site']
        mutate(f'join-lost-branch-{call:06x}', extract.pc24_to_offset(branch), b'\xea', must_fail=True)
    for consumer in expected['consumers']:
        first = extract.pc24_from_string(consumer['call_sites'][0]['call_site'])
        mutate('removed-call-' + consumer['id'], extract.pc24_to_offset(first), b'\xea', must_fail=True)
    mutate('unexpected-raw-interpreter-call', len(rom)-16,
           extract.routine_call_pattern(census['interactive_entry_pc24'], 'jsr'), must_fail=True)
    for wrapper in expected['dialogue_forwarding']['wrappers']:
        adjacent = next((row for row in wrapper['call_sites']
                         if row['source_origin'] in ('adjacent_immediate_y', 'branch_join_immediate_y')), None)
        if adjacent:
            call = extract.pc24_to_offset(extract.pc24_from_string(adjacent['call_site']))
            mutate(wrapper['id']+'-lost-ldy', call-3, b'\xea', must_fail=True)
    for consumer, proof in expected['native_dictionary_consumers'].items():
        entry = extract.pc24_to_offset(extract.pc24_from_string(proof['reader_pc24']))
        # Include dispatch, saved cursor, count and per-consumer branch policy.
        # Walk the exact reader until its verified final BRA, rather than copy
        # the Go instruction constructor into the oracle.
        end = entry + 32
        while end < entry + 96 and rom[end:end+3] != b'\x7a\xab\x80':
            end += 1
        if end == entry + 96:
            raise AssertionError('dictionary reader footer not found')
        size = end + 4 - entry
        for index in range(size):
            mutate(f'dictionary-{consumer}-byte-{index}', entry+index,
                   bytes((rom[entry+index] ^ 1,)), must_fail=True)
    clear = extract.pc24_to_offset(extract.pc24_from_string(expected['native_dialogue_layout']['clear_routine_pc24']))
    for index in (0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19):
        mutate(f'clear-signature-{index}', clear+index, bytes((rom[clear+index] ^ 1,)), must_fail=True)
    mutate('clear-odd-origin', clear+2, bytes((rom[clear+2] | 1,)), must_fail=True)
    mutate('clear-zero-columns', clear+12, b'\0', must_fail=True)
    mutate('clear-narrower-columns', clear+12, bytes((rom[clear+12]-1,)))
    matrix = expected['nested_handler_sources']['matrices'][0]
    root = extract.pc24_to_offset(extract.pc24_from_string(matrix['row_table_pc24']))
    mutate('handler-row-table-invalid', root, b'\0\0', must_fail=True)
    first_row = matrix['rows'][0]
    target_table = extract.pc24_to_offset(extract.pc24_from_string(first_row['source_table_pc24']))
    targets = first_row['target_pc24s']
    alternate = next((target for target in targets if target != targets[0]), None)
    if alternate is None:
        # Synthetic all-alias matrix still needs a distinct, valid source.
        alternate = expected['source_reference_seeds']['references'][0]['source_pc24']
    local = extract.pc24_from_string(alternate) & 0xffff
    mutate('handler-valid-pointer-retarget', target_table, local.to_bytes(2, 'little'))
    first_table = expected['fixed_composer_sources']['pointer_tables'][0]
    table = extract.pc24_to_offset(extract.pc24_from_string(first_table['source_table_pc24']))
    mutate('composer-pointer-outside-menu', table, b'\0\x80', must_fail=True)
    for flow in expected['fixed_composer_sources']['flow_sources']:
        if 'descriptor_pc24' in flow:
            descriptor = extract.pc24_to_offset(extract.pc24_from_string(flow['descriptor_pc24']))
            mutate(flow['id']+'-bad-destination', descriptor, b'\xff', must_fail=True)
            mutate(flow['id']+'-moved-destination', descriptor, bytes(((rom[descriptor]+1) % 32,)))
    for indexed in expected['fixed_composer_sources']['indexed_direct_sources']:
        source = extract.pc24_to_offset(extract.pc24_from_string(indexed['source_pc24']))
        mutate('indexed-selector-wrong-opcode', source, b'\x09', must_fail=True)
    for load_site in census.get('composer_flow_sources', {}).get('choice_descriptor_load_sites', ()):
        mutate('choice-descriptor-load-' + str(load_site), extract.pc24_to_offset(load_site), b'\xea', must_fail=True)
    for call, _, continuation in census.get('interactive_yield_continuations', ()):
        if call == census.get('composer_flow_sources', {}).get('choice_yield_call_site'):
            record = extract.Decoder(rom, profile).decode_record(extract.pc24_to_offset(continuation))
            mutate('choice-continuation-end-not-yield', record['end']-1, b'\0', must_fail=True)
    return cases


def synthetic_source_groups():
    """No retail bytes: known instruction shapes around deliberately tiny text."""
    groups = []
    for mode in ('tables', 'yield-flow', 'descriptor-flow'):
        profile = {'id': 'test', 'encoding': 'dictionary-12', 'dictionary': 0x2000,
                   'handler_table': 0xa00, 'menu_start': 0x9000, 'menu_end': 0x9400}
        census = {
            'interactive_entry_pc24': 0x008100, 'interactive_end_pc24': 0x008200,
            'interactive_call_count': 4,
            'interactive_nonadjacent_layout': ('nested_handler_table', 'yield_continuation'),
            'interactive_branch_join_layout': ((0x008023, 2),),
            'interactive_yield_continuations': ((0x008070, 0x009250, 0x009252),),
            'dialogue_source_bank': 0,
            'dialogue_wrappers': ((0x008500, 'jsr', 1, 0),),
            'dialogue_source_relay': (0x008300, 0),
            'composer_source_tables': (('sim_root', 0x018400, 1, 0),),
            'composer_direct_source_table': (0x018410, ('direct',), 2),
            'composer_indexed_direct_sources': (('indexed', 0x019310, 0x1234, 0x019340, ('choice_a', 'choice_b')),),
            'composer_dynamic_sources': (('master_report', 0x019328),),
            'composer_numeric_source': 0x019338,
            'composer_entry_pc24': 0x018100, 'composer_end_pc24': 0x018200,
            'composer_groups': (('sim_and_sky_menu', 1), ('name_entry', 1)),
            'bg3_buffer_write_count': 9,
        }
        rom = bytearray(0x10000)

        def put(offset, data):
            if isinstance(data, str):
                data = bytes.fromhex(data)
            rom[offset:offset+len(data)] = data

        put(0x100, extract.INTERACTIVE_CONSUMER_SIGNATURE)
        put(0x8100, extract.FIXED_COMPOSER_SIGNATURE)
        put(0x10e, 'a2 00 01 20 00 84')
        put(0x20, 'a0 34 92 20 00 81')
        put(0x10, 'a0 05 92 82 0d 00')
        put(0x40, 'a0 05 92 82 0a 00')
        put(0x4d, 'a0 07 92 20 00 85')
        put(0x60, 'a0 50 92 20 00 81')
        put(0x70, '20 00 81')
        put(0x2a, '7d 00 8a ea ea ea ea 20 00 81')
        put(0x8020, '22 00 81 01')
        put(0x8030, '22 00 81 01')
        put(0x8400, '00 93')
        put(0x8410, '00 93')
        put(0x9310, '08 34 12 40 93 00')
        put(0x9340, '20 93 30 93')
        put(0x1234, b'X\0')
        put(0x1250, b'A\x01B\0')
        for row in range(6):
            local = 0x8a0c + row*64
            put(0xa00+row*2, local.to_bytes(2, 'little'))
            for slot in range(32):
                put(0xa0c+row*64+slot*2, '34 92')
        for offset in (0x150, 0x8150, 0x300, 0x500, 0x600, 0x700):
            put(offset, extract.BG3_TEXT_BUFFER_WRITE)
        put(0x400, 'da a2 04 01 a9 06 48 da a9 00 eb a9 1a eb 9f 00 b0 7f e8 e8')
        prefix = bytes.fromhex('b9 00 00 30 02 c8 60 c8 8b 5a c2 20 29 7f 00 48 0a 18 63 01 0a 0a a8 68 e2 20 a9 00 48 ab a9 0c')
        put(0x180, prefix + bytes.fromhex('eb e6 f1 b9 00 a0 9f 00 b0 7f e8 e8 c8 c9 20 f0 09 eb 48 20 1c 90 68 3a d0 e6 7a ab 80 c2'))
        put(0x8180, prefix + bytes.fromhex('eb b9 00 a0 f0 0f 9f 00 b0 7f e8 e8 c8 c9 20 f0 04 eb 3a d0 eb 7a ab 80 c7'))
        put(0x130, '20 80 81')
        put(0x8130, '20 80 81')
        # The flow variants move menu data into the interpreter's bank so the
        # sample dialogue has a real bank-local call, not a fabricated LDY.
        if mode != 'tables':
            profile.update(menu_start=0x1200, menu_end=0x1400)
            put(0xc00, bytes(rom[0x8400:0x8412]))
            put(0x1310, bytes(rom[0x9310:0x9344]))
            census.update(composer_source_tables=(('sim_root', 0x008c00, 1, 0),),
                          composer_direct_source_table=(0x008c10, ('direct',), 2),
                          composer_indexed_direct_sources=(('indexed', 0x009310, 0x1234, 0x009340, ('choice_a', 'choice_b')),),
                          composer_dynamic_sources=(('master_report', 0x009328),),
                          composer_numeric_source=0x009338,
                          composer_groups=(('sim_and_sky_menu', 1), ('name_entry', 2)))
            put(0x80, 'a0 50 93 22 00 81 01')
            put(0x1350, b'ABC\0\x0a\x02FAST<>SLOW\0')
            put(0x60, 'a0 61 93 20 00 81')
            put(0x1250, b'A\x01B\x01\x04\x02Y N\0')
            flow = {'message_speed_selector_call_site': 0x008083, 'choice_yield_call_site': 0x008070}
            if mode == 'descriptor-flow':
                flow = {'message_speed_selector_call_site': 0x008083,
                        'choice_descriptor_pc24': 0x009270,
                        'choice_descriptor_load_sites': (0x008090, 0x008098)}
                put(0x90, 'a2 70 92')
                put(0x98, 'a2 70 92')
                put(0x1270, b'\x04\x02Y N\0')
            census['composer_flow_sources'] = flow

        old_census = extract.CONSUMER_CENSUS_PROFILES.get('test')
        old_dictionary = extract.DICTIONARY_CONSUMER_PROFILES.get('test')
        old_routes = extract.LATIN_INTERACTIVE_ROUTE_IDS
        try:
            extract.CONSUMER_CENSUS_PROFILES['test'] = census
            extract.DICTIONARY_CONSUMER_PROFILES['test'] = (0x008180, 0x018180, 0xf1, False)
            extract.LATIN_INTERACTIVE_ROUTE_IDS = {0x008063: 'system.message_speed.sample'}
            expected = reference_sources(profile, bytes(rom))
            go_source_profile = source_profile(profile, census)
            mutations = source_mutations(profile, bytes(rom), expected)
            catalog_group = synthetic_catalog_group(profile, census, bytes(rom), mode)
        finally:
            extract.LATIN_INTERACTIVE_ROUTE_IDS = old_routes
            for mapping, prior in ((extract.CONSUMER_CENSUS_PROFILES, old_census),
                                   (extract.DICTIONARY_CONSUMER_PROFILES, old_dictionary)):
                if prior is None:
                    mapping.pop('test', None)
                else:
                    mapping['test'] = prior
        groups.append({'id': 'synthetic-sources-' + mode,
                       'rom': base64.b64encode(rom).decode('ascii'),
                       'profile': dict(profile, glyphs=extract.base_glyph_map()),
                       'source_profile': go_source_profile,
                       'source_expected': expected, 'source_mutations': mutations,
                       'cases': []})
        groups.append(catalog_group)
    return groups
