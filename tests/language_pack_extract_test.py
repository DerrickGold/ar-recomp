#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'language_pack_extract', ROOT / 'tools' / 'language_pack_extract.py')
EXTRACT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXTRACT)


def main():
    assert EXTRACT.pc24_to_offset(0x01F04F) == 0xF04F
    assert EXTRACT.offset_to_pc24(0xF04F) == 0x01F04F
    assert EXTRACT.routine_call_pattern(0x018E29, 'jsr') == bytes.fromhex(
        '20 29 8e')
    assert EXTRACT.routine_call_pattern(0x02BF60, 'jsl') == bytes.fromhex(
        '22 60 bf 02')

    conditional = bytearray(64)
    conditional[11:14] = bytes.fromhex('a0 11 91')
    conditional[19:22] = bytes.fromhex('f0 0a a0')
    conditional[22:24] = bytes.fromhex('22 92')
    conditional[24:27] = bytes.fromhex('a3 01 f0')
    conditional[27:30] = bytes.fromhex('03 a0 33')
    conditional[30:33] = bytes.fromhex('93 68 20')
    assert EXTRACT.conditional_immediate_y_sources(
        bytes(conditional), 0x008020) == [0x9111, 0x9222, 0x9333]

    hud_rom = bytearray(0x4000)
    hud_rom[0x100:0x111] = bytes.fromhex(
        'bf 00 90 00 09 00 20 9f 40 b0 7f e8 e8 e0 80 00 d0')
    hud_rom[0x111] = 0xEE
    hud_rom[0x1000:0x1080] = bytes.fromhex('01 00') * 64
    hud_template = EXTRACT.classify_bg3_hud_template_copy(
        bytes(hud_rom), 0x008107, 'action_hud_template', {
            'bg3_hud_template_sources': (
                ('action_hud_template', 0x009000),),
        })
    assert hud_template['classification'] == \
        'classified_graphical_text_template'
    assert hud_template['graphical_text_region_count'] == 4
    assert hud_template['graphical_text_word_count'] == 21
    assert hud_template['other_template_word_count'] == 43

    asset_rom = bytearray(0x30000)
    asset_rom[
        EXTRACT.ASSET_SCRIPT_BASE + EXTRACT.ASSET_SCRIPT_HEADER_BYTES:
        EXTRACT.ASSET_SCRIPT_BASE + EXTRACT.ASSET_SCRIPT_HEADER_BYTES + 8
    ] = bytes.fromhex('00 00 08 01 00 07 08 00')
    title_surface = EXTRACT.build_title_graphical_text_surface(
        bytes(asset_rom))
    assert title_surface['id'] == 'title.logo_and_publisher'
    assert title_surface['asset_scene']['command_count'] == 1
    assert title_surface['settled_capture_rect_xyxy'] == [11, 27, 248, 122]

    ending_rom = bytearray(0x8000)
    signature_offset = 0x400
    routine_offset = signature_offset - 20
    ending_rom[routine_offset:routine_offset + 5] = bytes.fromhex(
        '48 b0 0e a9 0f')
    ending_rom[
        signature_offset:signature_offset +
        len(EXTRACT.ENDING_PAGE_COPY_SIGNATURE)
    ] = EXTRACT.ENDING_PAGE_COPY_SIGNATURE
    routine_local = EXTRACT.offset_to_pc24(routine_offset) & 0xFFFF
    ending_call = bytes((0x20, routine_local & 0xFF, routine_local >> 8))
    ending_rom[0xFD:0x103] = bytes.fromhex('a9 00 38') + ending_call
    ending_rom[0x108:0x10B] = ending_call
    ending_rom[0x10B:0x10F] = bytes.fromhex('1a c9 11 90')
    for call_offset, page, carry in (
            (0x120, 17, 0x18), (0x130, 18, 0x18), (0x140, 19, 0x38)):
        ending_rom[call_offset - 3:call_offset + 3] = bytes(
            (0xA9, page, carry)) + ending_call
    ending_surface = EXTRACT.build_ending_graphical_text_surface(
        bytes(ending_rom))
    assert ending_surface['copy_routine']['caller_count'] == 5
    assert ending_surface['page_abi']['addressable_page_count'] == 20
    assert ending_surface['page_abi']['active_page_indices'] == list(range(20))
    assert not ending_surface['page_abi']['dormant_page_indices']
    ending_rom[0x10D] = 0x10
    ending_rom[0x11E] = 16
    japanese_ending_surface = EXTRACT.build_ending_graphical_text_surface(
        bytes(ending_rom))
    assert japanese_ending_surface['page_abi'][
        'sequential_page_stop_exclusive'] == 16
    assert japanese_ending_surface['page_abi']['dormant_page_indices'] == [17]

    vram_rom = bytearray(0x10000)
    vram_sites = tuple(0x008100 + index * 8 for index in range(9))
    vram_patterns = (
        '8d1821', '8d1921', '8d1821', '8d1921',
        '8d1821', '9c1821', '8d1821', '8d1821', '8f182100')
    for site, pattern in zip(vram_sites, vram_patterns):
        offset = EXTRACT.pc24_to_offset(site)
        raw = bytes.fromhex(pattern)
        vram_rom[offset:offset + len(raw)] = raw
    vram_profile = {
        'direct_vram_port_paths': (
            ('asset_script_character_upload', vram_sites[:4]),
            ('developed_world_map_upload', vram_sites[4:5]),
            ('bg3_tilemap_clear', vram_sites[5:6]),
            ('action_obj_graphics_upload', vram_sites[6:8]),
            ('simulation_tilemap_upload', vram_sites[8:])),
    }
    vram_census = EXTRACT.classify_direct_vram_port_paths(
        bytes(vram_rom), vram_profile)
    assert vram_census['decoded_write_site_count'] == 9
    assert len(vram_census['paths']) == 5
    assert not vram_census['unclassified_path_count']

    vram_rom[0x300:0x303] = bytes.fromhex('7e00b2')
    range_evidence = EXTRACT.classify_decoded_bg3_range_evidence(
        bytes(vram_rom), {
            'decoded_bg3_range_reference_count': 10,
            'decoded_bg3_range_direct_long_count': 9,
            'decoded_bg3_range_rejected_sites': ((0x008300, '7e00b2'),),
        })
    assert range_evidence['decoded_nonlong_bg3_write_count'] == 0
    assert range_evidence['rejected_nonlong_candidate_count'] == 1

    dma_sites = tuple(0x008500 + index * 8 for index in range(12))
    for role, site in zip(EXTRACT.DMA_LAUNCH_ROLES, dma_sites):
        raw = bytes.fromhex(
            '9c0b42' if role == 'dma_disable' else
            '8e0b42' if role in ('tilemap_record', 'cgram_flicker') else
            '8d0b42')
        offset = EXTRACT.pc24_to_offset(site)
        vram_rom[offset:offset + len(raw)] = raw
    dma_census = EXTRACT.classify_dma_launches(bytes(vram_rom), {
        'dma_launch_sites': tuple(zip(EXTRACT.DMA_LAUNCH_ROLES, dma_sites)),
    })
    assert dma_census['decoded_write_site_count'] == 12
    assert dma_census['bg3_known_text_transfer_count'] == 2
    assert dma_census['generic_vram_descriptor_transfer_count'] == 1
    assert not dma_census['unclassified_count']

    descriptor_rom = bytearray(0x10000)
    cursor = 0x100
    for pattern_hex in EXTRACT.VRAM_DESCRIPTOR_PATTERN_HEX[
            'native'].values():
        pattern = bytes.fromhex(pattern_hex)
        descriptor_rom[cursor:cursor + len(pattern)] = pattern
        cursor += len(pattern) + 16
    descriptor_census = EXTRACT.build_vram_descriptor_census(
        bytes(descriptor_rom), {'vram_descriptor_abi': 'native'})
    assert descriptor_census['abi'] == 'native'
    assert descriptor_census['producer_or_dependency_family_count'] == 7
    assert descriptor_census['slots']['slot_0']['source_address'] == '$D0'
    assert not descriptor_census['unclassified_family_count']

    indirect_rom = bytearray(0x10000)
    indirect_sites = (0x008100, 0x008108)
    rejected_site = 0x008110
    for site in indirect_sites:
        offset = EXTRACT.pc24_to_offset(site)
        indirect_rom[offset:offset + 2] = bytes.fromhex('97a8')
    rejected_offset = EXTRACT.pc24_to_offset(rejected_site)
    indirect_rom[rejected_offset:rejected_offset + 2] = bytes.fromhex('93f0')
    indirect_census = EXTRACT.classify_indirect_write_paths(
        bytes(indirect_rom), {
            'indirect_write_sites': (
                ('asset_workspace', '97a8', indirect_sites),),
            'indirect_write_rejected_sites': (
                (rejected_site, '93f0'),),
            'indirect_write_decoded_reference_count': 3,
        })
    assert indirect_census['live_write_site_count'] == 2
    assert indirect_census['rejected_decode_count'] == 1
    assert not indirect_census['unclassified_count']
    assert indirect_census['families'][0][
        'classification'] == 'verified_non_language_asset_workspace'

    asset_rom = bytearray(EXTRACT.ASSET_SCRIPT_BASE + 64)
    script = bytes((
        0x07, 0x08,
        0x80, 0x00, 0x08, 0x50, 0x34, 0x12, 0x00,
        0x00,
    ))
    start = EXTRACT.ASSET_SCRIPT_BASE + EXTRACT.ASSET_SCRIPT_HEADER_BYTES
    asset_rom[start:start + len(script)] = script
    entries = list(EXTRACT.iter_asset_script(bytes(asset_rom)))
    assert len(entries) == 1
    assert entries[0][0:2] == (0x07, 0x08)
    assert entries[0][2][0][1] == bytes.fromhex('000850341200')

    rom = bytearray(4096)
    source = bytes((
        ord('A'),
        0x08, 0x02, 0x00, 0x43, 0x80,
        0x09, 0x03, 0x06, 0x00,
        0x04, 0x05, 0x00,
    ))
    rom[:len(source)] = source
    profile = {
        'encoding': 'dictionary-12',
        'dictionary': 2048,
        'indexed_text_semantics': {'02 00 43 80': 'enemy_name'},
        'number_semantics': {'$0006': 'lair_count'},
    }
    decoder = EXTRACT.Decoder(bytes(rom), profile)
    operations = decoder.decode_record(0, len(source))['operations']
    assert operations == [
        {'op': 'text', 'value': 'A'},
        {'op': 'insert_indexed_text', 'code': '08',
         'args_hex': '02 00 43 80', 'index_address': '$0002',
         'pointer_table': '$8043', 'value': 'enemy_name',
         'confidence': 'mapped_semantic'},
        {'op': 'format_number', 'width': 3, 'native_address': '$0006',
         'value': 'lair_count', 'confidence': 'mapped_semantic'},
        {'op': 'toggle_text_state', 'native_control': '04',
         'confidence': 'mapped_presentation_only'},
        {'op': 'reset_text_cursor', 'confidence': 'mapped'},
        {'op': 'end', 'native_cursor_after': '$00:800D'},
    ]

    yielded_source = b'A\x01\x08\x01\x02\x03\x04\x00'
    yielded_profile = {'encoding': 'direct-glyph'}
    yielded = EXTRACT.Decoder(
        yielded_source, yielded_profile).decode_record(
            0, len(yielded_source))
    assert yielded['terminated']
    assert yielded['end'] == 2
    assert yielded['operations'] == [
        {'op': 'text', 'value': 'A'},
        {'op': 'yield', 'native_cursor_after': '$00:8002'},
    ]
    # A caller-proven cursor continuation can still be decoded explicitly.
    continued = EXTRACT.Decoder(
        yielded_source, yielded_profile).decode_record(
            2, len(yielded_source))
    assert continued['operations'][0]['op'] == 'insert_indexed_text'

    unknown = bytearray(4096)
    unknown[:6] = bytes((0x08, 1, 2, 3, 4, 0))
    unresolved = EXTRACT.Decoder(bytes(unknown), profile).decode_record(
        0, 6)['operations'][0]
    assert unresolved['op'] == 'insert_indexed_text'
    assert unresolved['value'] is None
    assert unresolved['confidence'] == 'unresolved_semantic'

    japanese = {
        'encoding': 'direct-glyph',
        'glyph_overrides': EXTRACT.JAPANESE_GLYPH_MAP,
    }
    # Small kana, hiragana/katakana, punctuation, long vowel, and following
    # dakuten/handakuten all become ordinary NFC Unicode text.
    direct = bytes((
        0x3E, 0x87, 0x91, 0x96, 0xDE, 0xA1, 0xA4, 0xA5,
        0xA7, 0xB1, 0xB0, 0xCA, 0xDF, 0xEA, 0xDE, 0x00,
    ))
    japanese_operations = EXTRACT.Decoder(direct, japanese).decode_record(
        0, len(direct))['operations']
    assert japanese_operations == [
        {'op': 'text', 'value': '!ぁあが。、・ァアーパば'},
        {'op': 'end', 'native_cursor_after': '$00:8010'},
    ]

    # Unassigned UI/icon tiles and an unattached mark stay lossless instead of
    # being guessed into the author-facing script.
    unresolved_direct = bytes((0x80, 0xDE, 0x00))
    unresolved_operations = EXTRACT.Decoder(
        unresolved_direct, japanese).decode_record(
            0, len(unresolved_direct))['operations']
    assert unresolved_operations[0] == {
        'op': 'native_glyphs', 'codes': '80', 'unicode': None}
    assert unresolved_operations[1]['op'] == 'native_diacritic'

    semantic_glyph_profile = {
        'encoding': 'direct-glyph',
        'glyph_overrides': {0x7D: '×'},
        'icon_glyphs': {
            0x3A: EXTRACT.icon_part('ui.selection_pointer', 0, 2),
            0x3B: EXTRACT.icon_part('ui.selection_pointer', 1, 2),
        },
    }
    semantic_glyphs = EXTRACT.Decoder(
        bytes((0x3A, 0x3B, 0x7D, 0x00)),
        semantic_glyph_profile).decode_record(0, 4)['operations']
    assert semantic_glyphs == [
        {'op': 'insert_icon', 'value': 'ui.selection_pointer',
         'native_code': '3A', 'part_index': 0, 'part_count': 2,
         'confidence': 'mapped_semantic'},
        {'op': 'insert_icon', 'value': 'ui.selection_pointer',
         'native_code': '3B', 'part_index': 1, 'part_count': 2,
         'confidence': 'mapped_semantic'},
        {'op': 'text', 'value': '×'},
        {'op': 'end', 'native_cursor_after': '$00:8004'},
    ]
    assert EXTRACT.visible_units(semantic_glyphs) == 3

    japanese_spacing_marks = EXTRACT.Decoder(
        bytes((0xFE, 0xFF, 0x00)), japanese).decode_record(
            0, 3)['operations']
    assert japanese_spacing_marks[0] == {
        'op': 'text', 'value': '゛゜'}

    composer_source = bytes((
        ord('A'), 0x02,
        0x08, 0x34, 0x12, 0x78, 0x56,
        0x09, 0x02, 0x06, 0x00,
        0x0B, 0x02, 0x0D, 0x00,
    ))
    composer_profile = {
        'encoding': 'direct-glyph',
        'indexed_text_semantics': {},
        'number_semantics': {'$0006': 'lair_count'},
    }
    composer_operations = EXTRACT.FixedComposerDecoder(EXTRACT.Decoder(
        composer_source, composer_profile)).decode_record(
            0, len(composer_source))['operations']
    assert composer_operations == [
        {'op': 'text', 'value': 'A'},
        {'op': 'composer_noop_control', 'code': '02',
         'confidence': 'mapped_reserved'},
        {'op': 'insert_indexed_text', 'code': '08',
         'args_hex': '34 12 78 56', 'index_address': '$1234',
         'pointer_table': '$5678', 'value': None,
         'confidence': 'unresolved_semantic'},
        {'op': 'format_number', 'width': 2, 'native_address': '$0006',
         'value': 'lair_count', 'confidence': 'mapped_semantic'},
        {'op': 'text', 'value': '  '},
        {'op': 'line_break'},
        {'op': 'end', 'native_cursor_after': '$00:800F'},
    ]

    dynamic_messages = [{
        'id': 'dynamic.values',
        'operations': [
            {'op': 'insert_master_name'},
            {'op': 'format_number', 'value': 'lair_count'},
            {'op': 'insert_indexed_text', 'value': 'growth_state'},
        ],
    }, {
        'id': 'dynamic.lookup.target',
        'verified_semantic_id': 'simulation.growth_state.none',
        'operations': [{'op': 'text', 'value': 'None'}],
    }]
    dynamic_inventory = {'dynamic_lookup_tables': [{
        'id': 'growth_state',
        'target_count': 1,
        'semantic_ids': ['simulation.growth_state.none'],
    }]}
    dynamic_census = EXTRACT.build_dynamic_text_census(
        {'number_semantics': {'$0006': 'lair_count'},
         'indexed_text_semantics': {'00 00 00 00': 'growth_state'}},
        dynamic_messages, {'segments': []}, dynamic_inventory)
    assert dynamic_census['complete']
    assert dynamic_census['typed_value_count'] == 3
    unresolved_dynamic = EXTRACT.build_dynamic_text_census(
        {}, [{'id': 'missing', 'operations': [
            {'op': 'format_number', 'value': None}]}],
        {'segments': []}, {'dynamic_lookup_tables': []})
    assert not unresolved_dynamic['complete']
    assert unresolved_dynamic['unresolved_operation_count'] == 1

    assert EXTRACT.merge_source_intervals([
        (10, 20), (20, 25), (5, 8), (7, 9)]) == [(5, 9), (10, 25)]
    assert EXTRACT.interval_byte_count([(10, 20), (15, 25)]) == 15

    unterminated_profile = {'id': 'test', 'encoding': 'direct-glyph'}
    unterminated_decoder = EXTRACT.Decoder(b'AB', unterminated_profile)
    try:
        EXTRACT.add_sequential_block(
            [], {}, unterminated_profile, b'AB', unterminated_decoder,
            0, 2, 'test_block', 'test.message')
    except ValueError as error:
        assert 'without an end/yield control' in str(error)
    else:
        raise AssertionError('unterminated sequential record was accepted')

    source_metadata = {'rom_sha256': '0' * 64}
    messages = [{
        'alignment_status': 'callsite_verified',
        'operations': [{'op': 'text', 'value': 'Synthetic text'},
                       {'op': 'end'}],
    }, {
        'alignment_status': 'positional_unverified',
        'operations': [{'op': 'native_control', 'code': '0A',
                        'confidence': 'unresolved'}],
    }]
    menu = {'segments': [{
        'alignment_status': 'unassigned',
        'classification': 'text_candidate',
        'operations': [{'op': 'native_glyphs', 'codes': '7E'}],
    }]}
    coverage = EXTRACT.build_coverage_report(
        {'id': 'test', 'locale': 'xx-Test'}, source_metadata,
        messages, [{'id': 'synthetic.pointer_set'}], menu)
    assert not coverage['complete']
    assert coverage['counts'] == {
        'structured_messages': 2,
        'logically_aligned_structured_messages': 1,
        'unaligned_structured_messages': 1,
        'pointer_sets': 1,
        'menu_segments': 1,
        'semantically_assigned_menu_segments': 0,
        'unassigned_menu_segments': 1,
        'unresolved_operation_instances': 2,
    }
    blocker_ids = {blocker['id'] for blocker in coverage['blockers']}
    assert blocker_ids == {
        'whole_rom_language_candidate_scan',
        'text_consumer_reference_census',
        'dynamic_text_census',
        'graphical_text_census',
        'cross_release_semantic_alignment',
        'unresolved_operations_or_glyphs',
    }
    # The standalone coverage artifact is safe to inspect: it reports counts
    # and provenance, not extracted retail wording.
    assert 'Synthetic text' not in json.dumps(coverage)

    # A ROM-free synthetic layout exercises the exact consumer census without
    # distributing or requiring any retail image in the test suite.
    synthetic_rom = bytearray(0x10000)
    synthetic_profile = {
        'interactive_entry_pc24': 0x008100,
        'interactive_end_pc24': 0x008200,
        'interactive_call_count': 4,
        'interactive_nonadjacent_layout': (
            'nested_handler_table', 'yield_continuation'),
        'interactive_branch_join_layout': ((0x008023, 2),),
        'interactive_yield_continuations': (
            (0x008070, 0x009250, 0x009252),),
        'dialogue_source_bank': 0x00,
        'dialogue_wrappers': ((0x008500, 'jsr', 1, 0x00),),
        'dialogue_source_relay': (0x008300, 0),
        'composer_source_tables': (('sim_root', 0x018400, 1, 0),),
        'composer_direct_source_table': (0x018410, ('direct',), 2),
        'composer_indexed_direct_sources': (
            ('indexed', 0x019310, 0x1234, 0x019340,
             ('choice_a', 'choice_b')),),
        'composer_dynamic_sources': (('master_report', 0x019328),),
        'composer_numeric_source': 0x019338,
        'composer_entry_pc24': 0x018100,
        'composer_end_pc24': 0x018200,
        'composer_groups': (('sim_and_sky_menu', 1), ('name_entry', 1)),
        'bg3_buffer_write_count': 7,
    }
    EXTRACT.CONSUMER_CENSUS_PROFILES['test'] = synthetic_profile
    interactive_offset = EXTRACT.pc24_to_offset(0x008100)
    composer_offset = EXTRACT.pc24_to_offset(0x018100)
    synthetic_rom[
        interactive_offset:interactive_offset +
        len(EXTRACT.INTERACTIVE_CONSUMER_SIGNATURE)
    ] = EXTRACT.INTERACTIVE_CONSUMER_SIGNATURE
    synthetic_rom[
        composer_offset:composer_offset +
        len(EXTRACT.FIXED_COMPOSER_SIGNATURE)
    ] = EXTRACT.FIXED_COMPOSER_SIGNATURE
    synthetic_rom[0x20:0x26] = bytes.fromhex('a0 34 92 20 00 81')
    synthetic_rom[0x10:0x16] = bytes.fromhex('a0 05 92 82 0d 00')
    synthetic_rom[0x40:0x46] = bytes.fromhex('a0 05 92 82 0a 00')
    synthetic_rom[0x4D:0x53] = bytes.fromhex('a0 07 92 20 00 85')
    synthetic_rom[0x60:0x66] = bytes.fromhex(
        'a0 50 92 20 00 81')
    synthetic_rom[0x70:0x73] = bytes.fromhex('20 00 81')
    synthetic_rom[0x2A:0x34] = bytes.fromhex(
        '7d 00 8a ea ea ea ea 20 00 81')
    synthetic_rom[0x8020:0x8024] = bytes.fromhex('22 00 81 01')
    synthetic_rom[0x8030:0x8034] = bytes.fromhex('22 00 81 01')
    synthetic_rom[0x8400:0x8402] = bytes.fromhex('00 93')
    synthetic_rom[0x8410:0x8412] = bytes.fromhex('00 93')
    synthetic_rom[0x9310:0x9316] = bytes.fromhex('08 34 12 40 93 00')
    synthetic_rom[0x9340:0x9344] = bytes.fromhex('20 93 30 93')
    synthetic_rom[0x1234:0x1236] = b'X\0'
    synthetic_rom[0x1250:0x1254] = b'A\x01B\0'
    for row in range(EXTRACT.HANDLER_CITY_COUNT):
        row_local = (0x8A00 + EXTRACT.HANDLER_CITY_COUNT * 2 +
                     row * EXTRACT.HANDLER_SLOTS_PER_CITY * 2)
        synthetic_rom[0xA00 + row * 2:0xA02 + row * 2] = bytes((
            row_local & 0xFF, row_local >> 8))
        row_offset = 0xA0C + row * EXTRACT.HANDLER_SLOTS_PER_CITY * 2
        for slot in range(EXTRACT.HANDLER_SLOTS_PER_CITY):
            target_offset = row_offset + slot * 2
            synthetic_rom[target_offset:target_offset + 2] = bytes.fromhex(
                '34 92')
    for offset in (0x150, 0x8150, 0x300, 0x400, 0x500, 0x600, 0x700):
        synthetic_rom[offset:offset + 4] = EXTRACT.BG3_TEXT_BUFFER_WRITE
    consumer_census = EXTRACT.build_consumer_census(
        {'id': 'test', 'encoding': 'dictionary-12',
         'dictionary': 0x2000,
         'handler_table': 0xA00,
         'menu_start': 0x9000, 'menu_end': 0x9400}, bytes(synthetic_rom))
    del EXTRACT.CONSUMER_CENSUS_PROFILES['test']
    interactive, composer = consumer_census['consumers']
    assert interactive['call_site_count'] == 4
    assert interactive['adjacent_immediate_y_call_count'] == 2
    assert interactive['branch_join_immediate_y_call_count'] == 1
    assert interactive['unique_immediate_y_sources'] == 3
    assert interactive['nonadjacent_source_origin_call_count'] == 2
    assert interactive['unresolved_source_origin_call_count'] == 0
    continuation = next(
        site for site in interactive['call_sites']
        if site['source_origin'] == 'yield_continuation')
    assert continuation['parent_source_pc24'] == '$00:9250'
    assert continuation['source_pc24'] == '$00:9252'
    assert continuation['target_resolution'] == 'verified_returned_y_cursor'
    assert consumer_census['nested_handler_sources'][
        'pointer_slot_count'] == 192
    assert consumer_census['nested_handler_sources'][
        'unique_target_count'] == 1
    assert consumer_census['dialogue_forwarding']['wrappers'][0][
        'call_sites'][0]['source_origin'] == 'branch_join_immediate_y'
    assert composer['surface_group_counts'] == {
        'name_entry': 1, 'sim_and_sky_menu': 1}
    assert consumer_census['bg3_buffer_writes'][
        'outside_known_text_consumer_count'] == 5
    assert consumer_census['bg3_buffer_writes'][
        'outside_write_classification_status'] == 'not_profiled'
    assert consumer_census['bg3_direct_long_writes'][
        'status'] == 'not_profiled'
    seed_messages = [{
            'id': 'native.test.bank00.9200',
            'category': 'dialogue_consumer_seed',
            'source': {
                'file_offset': '0x001200',
                'end_file_offset_exclusive': '0x001210',
            },
        }, {
            'id': 'native.test.bank01.9300',
            'category': 'dialogue_consumer_seed',
            'source': {
                'file_offset': '0x009300',
                'end_file_offset_exclusive': '0x009340',
            },
        }]
    seed_profile = {
        'id': 'test',
        'encoding': 'dictionary-12',
        'dictionary': 0x2000,
    }
    seed_resolution = EXTRACT.expand_unmapped_dialogue_seeds(
        consumer_census, seed_messages, {'segments': []}, seed_profile,
        bytes(synthetic_rom), EXTRACT.Decoder(
            bytes(synthetic_rom), seed_profile))
    assert seed_resolution['all_current_seeds_mapped']
    assert seed_resolution['mapped_unique_source_count'] == 11
    assert consumer_census['source_seed_expansion']['added_record_count'] == 3
    assert seed_messages[-3]['operations'][0] == {
        'op': 'text', 'value': 'X'}
    assert seed_messages[-2]['operations'][0] == {
        'op': 'text', 'value': 'A'}
    assert seed_messages[-1]['operations'][0] == {
        'op': 'text', 'value': 'B'}
    fixed_sources = consumer_census['fixed_composer_sources']
    assert fixed_sources['status'] == 'known_tables_direct_dynamic_censused'
    assert len(fixed_sources['direct_sources']) == 1
    assert len(fixed_sources['indexed_direct_sources']) == 1
    assert fixed_sources['indexed_direct_sources'][0]['pointer_count'] == 2
    assert not fixed_sources['direct_source_candidates']
    assert not fixed_sources['flow_sources']
    assert len(fixed_sources['dynamic_reports']) == 1
    assert not fixed_sources['score_report_present']
    assert not fixed_sources['numeric_only_source'][
        'included_in_language_source_seeds']

    consumer_census['whole_game_consumer_discovery_complete'] = True
    source_ownership = EXTRACT.build_language_source_ownership(
        seed_profile, bytes(synthetic_rom), seed_messages, [],
        {'segments': []}, consumer_census)
    consumer_census['whole_game_consumer_discovery_complete'] = False
    assert source_ownership['complete']
    assert source_ownership['raw_printable_scan_rejected']
    assert not source_ownership['unclassified_record_count']
    assert source_ownership['record_counts_by_ownership'] == {
        'live_consumer_text': 5,
    }
    assert source_ownership['unique_consumer_source_count'] == 11
    assert source_ownership['mapped_unique_consumer_source_count'] == 11

    coverage_with_census = EXTRACT.build_coverage_report(
        {'id': 'test', 'locale': 'xx-Test'}, source_metadata,
        messages, [{'id': 'synthetic.pointer_set'}], menu, consumer_census,
        source_ownership=source_ownership)
    assert coverage_with_census['counts'][
        'known_text_consumer_call_sites'] == 6
    assert coverage_with_census['counts'][
        'nested_handler_pointer_slots'] == 192
    assert coverage_with_census['counts'][
        'nested_handler_unique_targets'] == 1
    assert coverage_with_census['counts'][
        'consumer_seed_expansion_records'] == 3
    assert coverage_with_census['counts'][
        'branch_join_interactive_source_origins'] == 1
    assert coverage_with_census['counts'][
        'branch_join_dialogue_wrapper_calls'] == 1
    assert coverage_with_census['counts'][
        'fixed_composer_verified_direct_sources'] == 2
    assert coverage_with_census['counts'][
        'fixed_composer_indexed_direct_targets'] == 2
    assert coverage_with_census['counts'][
        'fixed_composer_direct_source_candidates'] == 0
    assert coverage_with_census['counts'][
        'fixed_composer_dynamic_reports'] == 1
    assert not coverage_with_census['counts'][
        'fixed_composer_score_report_present']
    assert coverage_with_census['counts'][
        'fixed_composer_numeric_only_sources'] == 1
    assert coverage_with_census['counts'][
        'fixed_composer_stateful_flow_sources'] == 0
    assert coverage_with_census['counts'][
        'fixed_composer_stateful_language_sources'] == 0
    assert EXTRACT.fixed_composer_route_id(
        'fixed_composer_flow_choice_labels', 0) == 'system.choice.yes_no'
    assert EXTRACT.fixed_composer_route_id(
        'fixed_composer_flow_message_speed_scale_labels', 0) == \
        'system.message_speed.scale_labels'
    consumer_blocker = next(
        blocker for blocker in coverage_with_census['blockers']
        if blocker['id'] == 'text_consumer_reference_census')
    assert consumer_blocker['status'] == 'in_progress'
    assert 'whole_rom_language_candidate_scan' not in {
        blocker['id'] for blocker in coverage_with_census['blockers']}
    assert coverage_with_census['counts'][
        'unclassified_language_source_records'] == 0
    assert '$00:' not in json.dumps(coverage_with_census)

    route_catalogs = []
    for release_id in ('us', 'eu-en', 'de', 'fr', 'jp'):
        route_catalogs.append((
            {'id': release_id},
            {'semantic_route_catalog': {
                'complete': True,
                'routes': [{
                    'id': 'shared.route',
                    'source_record_id': f'native.{release_id}',
                }] + ([{
                    'id': 'variant.jp.route',
                    'source_record_id': 'native.jp.variant',
                }] if release_id == 'jp' else []),
            }}))
    alignment = EXTRACT.apply_cross_release_semantic_alignment(route_catalogs)
    assert alignment['complete']
    assert alignment['semantic_route_count'] == 2
    assert alignment['all_release_route_count'] == 1
    assert alignment['regional_or_release_variant_route_count'] == 1
    assert alignment['routes'][1]['available_releases'] == ['jp']

    first = ({'id': 'jp', 'label': 'Japan', 'locale': 'ja-JP'}, {
        'source': {'rom_sha256': '1' * 64},
        'coverage': coverage,
    })
    second = ({'id': 'us', 'label': 'USA', 'locale': 'en-US'}, {
        'source': {'rom_sha256': '2' * 64},
        'coverage': coverage,
    })
    index = EXTRACT.build_extraction_index([first, second])
    assert [release['release_id'] for release in index['releases']] == [
        'us', 'jp']
    assert not index['complete']
    print('language pack extraction control checks passed')


if __name__ == '__main__':
    main()
