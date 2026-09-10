"""Independent Python expectations for the Go native catalogue (not export)."""
import base64

import language_pack_extract as extract
from generate_go_localization_profiles import catalog_profile, source_profile

DESTINATION_KEYS = ('whole_game_consumer_discovery_complete', 'bg3_buffer_writes',
                    'bg3_direct_long_writes', 'bg3_decoded_address_range_audit',
                    'direct_vram_port_writes', 'dma_launches', 'generic_vram_descriptor',
                    'indirect_write_paths', 'dialog_font_asset')


def reference_catalog(profile, rom):
    decoder = extract.Decoder(rom, profile)
    messages, pointers, inventory = (extract.extract_japanese(rom, profile, decoder)
                                    if profile['encoding'] == 'direct-glyph'
                                    else extract.extract_european(rom, profile, decoder))
    census = extract.build_consumer_census(profile, rom)
    menu = extract.fixed_composer_catalog(profile, rom, decoder, census)
    extract.expand_unmapped_dialogue_seeds(census, messages, menu, profile, rom, decoder)
    dynamic = extract.build_dynamic_text_census(profile, messages, menu, inventory)
    # The Go catalogue now revalidates the complete destination/graphics
    # evidence itself. The original small source-only fixtures are explicitly
    # unprofiled for those audits and must retain incomplete ownership.
    closure = {}
    if census['whole_game_consumer_discovery_complete']:
        closure = {'destination_census': {key: census[key] for key in DESTINATION_KEYS},
                   'graphical_text_census': extract.build_graphical_text_census(profile, rom, census)}
    ownership = extract.build_language_source_ownership(profile, rom, messages, pointers, menu, census)
    routes = extract.build_semantic_route_catalog(profile, messages, pointers, menu, census, decoder)
    return {
        'whole_game_coverage_complete': False,
        'inventory': inventory, 'messages': sorted(messages, key=lambda m: int(m['source']['file_offset'], 16)),
        'pointer_sets': pointers, 'menu_source_catalog': menu,
        'source_reference_seeds': census['source_reference_seeds'],
        'source_reference_resolution': census['source_reference_resolution'],
        'source_seed_expansion': census['source_seed_expansion'],
        'dynamic_text_census': dynamic, 'language_source_ownership': ownership,
        'semantic_route_catalog': routes, **closure,
    }


def synthetic_catalog_group(profile, census, source_rom, mode):
    """Add invented bounded tables to the ROM-free instruction graph.

    The caller temporarily registers its test consumer contracts. Text bytes
    are deliberately tiny and duplicated; semantic identity must still follow
    each pointer/caller, not string equality or native-record ordinals.
    """
    profile = dict(profile, angel_start=0x1700, angel_end=0x1705,
                   town_start=0x1800, offering_table=0x1805, ending_table=0x1850,
                   action_stage_name_table=0x1500,
                   title_text_start=0x1900, title_text_end=0x1902,
                   action_label_start=0x1910, action_label_end=0x191d,
                   sound_test_start=0x1950, sound_test_end=0x1952,
                   lookup_source_tables=(('test_a', 0x1540, ('test.a', 'test.b')),
                                         ('test_b', 0x1550, ('test.c', 'test.d'))))
    rom = bytearray(0x30000)
    rom[:len(source_rom)] = source_rom

    def put(offset, data):
        rom[offset:offset+len(data)] = data

    def pointers(offset, targets):
        for index, target in enumerate(targets):
            assert offset // 0x8000 == target // 0x8000
            put(offset+index*2, (0x8000+target % 0x8000).to_bytes(2, 'little'))

    towns = [0x20100+index*8 for index in range(6)]
    enemies = [0x20140+index*8 for index in range(4)]
    stages = [0x1600+index*8 for index in range(7)]
    lookups = [0x1660+index*8 for index in range(4)]
    pointers(0x20000, [towns[0]] + towns)
    pointers(0x20043, enemies)
    pointers(0x1500, stages)
    pointers(0x1540, lookups[:2])
    pointers(0x1550, lookups[2:])
    for target in towns + enemies + stages + lookups:
        put(target, b'AB\0')
    put(0x1700, b'A1\0B\0')
    put(0x1800, b'C1\0D\0')
    pointers(0x1805, [0x1850] + [0x1834]*20)
    put(0x1834, b'E\0')
    pointers(0x1850, [0x1870]*8)
    put(0x1870, b'F\0')
    put(0x1900, b'T\0')
    put(0x1910, b'A\0B\0\0C\0D\0E\0F\0')
    put(0x1950, b'S\0')
    expected = reference_catalog(profile, bytes(rom))
    return {'id': 'synthetic-catalog-' + mode,
            'rom': base64.b64encode(rom).decode('ascii'),
            'profile': dict(profile, glyphs=extract.base_glyph_map(),
                            speed_codes=[0x3d, 0x3c],
                            speed_source_call=census.get('composer_flow_sources', {}).get(
                                'message_speed_selector_call_site')),
            'source_profile': source_profile(profile, census),
            'catalog_profile': catalog_profile(profile),
            'catalog_expected': expected,
            'catalog_mutations': catalog_mutations(profile, bytes(rom), expected), 'cases': []}


def catalog_mutations(profile, rom, expected):
    """Positive text/alias edits and negative catalogue-semantic conflicts."""
    cases = []

    def mutate(label, offset, data, reject=False):
        changed = bytearray(rom)
        changed[offset:offset+len(data)] = data
        try:
            result = reference_catalog(profile, bytes(changed))
        except (ValueError, IndexError):
            result = None
        if (result is None) != reject:
            raise AssertionError(f'{label}: unexpected reference acceptance/rejection')
        if not reject and result == expected:
            raise AssertionError(f'{label}: positive mutation had no effect')
        cases.append({'id': label, 'offset': offset, 'data': list(data),
                      'reject': reject, 'expected': result})

    # Alter only a known glyph byte, never an instruction or control argument.
    record = next(message for message in expected['messages']
                  if message['category'] == 'action_stage_name')
    start = int(record['source']['file_offset'], 16)
    assert rom[start] >= 0x20
    mutate('changed-stage-text', start, bytes((0x41 if rom[start] != 0x41 else 0x42,)))
    town_table = profile.get('town_name_table', 0x20000)
    mutate('retargeted-town-sentinel-alias', town_table, rom[town_table+4:town_table+6])
    stage_table = profile['action_stage_name_table']
    mutate('conflicting-verified-stage-identities', stage_table,
           rom[stage_table+2:stage_table+4], reject=True)
    for bound in ('title_text_end', 'action_label_end'):
        assert rom[profile[bound]-1] in (0, 1)
        mutate('unterminated-' + bound, profile[bound]-1, b'A', reject=True)
    # Several independently verified lookup semantics cannot silently collapse.
    for name, table, ids in profile.get('lookup_source_tables', ()):
        if len(ids) > 1:
            mutate('conflicting-lookup-' + name, table, rom[table+2:table+4], reject=True)
    return cases
