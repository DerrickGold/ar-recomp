"""ROM-free executable-reference tests for destination and asset ownership.

All bytes here are invented instruction fixtures or generated compressed data.
Published catalogue comparisons still run independently against every ROM.
"""
import base64
import random

import language_pack_extract as extract
from generate_go_localization_profiles import destination_profile
from go_localization_catalog_vectors import reference_catalog


def destination_mutations(profile, rom, catalog):
    """Exercise complete catalogue failure/publication, not just leaf helpers."""
    p = extract.CONSUMER_CENSUS_PROFILES[profile['id']]
    cases = []

    def mutate(label, offset, data, reject=True):
        changed = bytearray(rom)
        changed[offset:offset+len(data)] = data
        try:
            result = reference_catalog(profile, bytes(changed))
        except (ValueError, IndexError):
            result = None
        if (result is None) != reject or (not reject and result == catalog):
            raise AssertionError(f'{profile["id"]}/{label}: unexpected reference mutation result')
        cases.append(dict(id=label, offset=offset, data=list(data), reject=reject, expected=result))

    census = catalog['destination_census']
    for row in census['bg3_buffer_writes']['outside_classifications']:
        site = extract.pc24_to_offset(extract.pc24_from_string(row['write_site']))
        mutate('bg3-loop-'+row['role'], site+4, b'\xea')
    for role, site in p['bg3_direct_long_auxiliary']:
        if role in extract.BG3_HUD_GRAPHICAL_TEXT_REGIONS:
            mutate('hud-copy-'+role, extract.pc24_to_offset(site)-7, b'\xea')
            source = extract.pc24_to_offset(dict(p['bg3_hud_template_sources'])[role])
            mutate('hud-tile-'+role, source, bytes((rom[source]^1,)), reject=False)
    mutate('unexpected-bg3-store', len(rom)-16, bytes.fromhex('9f00b07f'))
    for role, sites in p['direct_vram_port_paths']:
        for index, site in enumerate(sites):
            mutate(f'vram-{role}-{index}', extract.pc24_to_offset(site), b'\xea')
    for role, site in p['dma_launch_sites']:
        mutate('dma-'+role, extract.pc24_to_offset(site), b'\xea')
    for role, pattern in extract.VRAM_DESCRIPTOR_PATTERN_HEX[p['vram_descriptor_abi']].items():
        site = extract.find_unique_pattern_pc24(rom, bytes.fromhex(pattern), role)
        mutate('descriptor-'+role, extract.pc24_to_offset(site), b'\xea')
    for role, _, sites in p['indirect_write_sites']:
        mutate('indirect-'+role, extract.pc24_to_offset(sites[0]), b'\xea')
    for key in ('decoded_bg3_range_rejected_sites', 'indirect_write_rejected_sites'):
        for index, (site, _) in enumerate(p.get(key, ())):
            mutate(f'{key}-{index}', extract.pc24_to_offset(site), b'\xea')
    font = int(census['dialog_font_asset']['source_file_offset'], 16)
    mutate('font-decoded-size', font, bytes((rom[font]^1,)))
    mutate('missing-title-scene', extract.ASSET_SCRIPT_BASE+3, b'\x01')
    ending = next(row for row in catalog['graphical_text_census']['resources'] if row['id'] == 'ending.credits.graphical_pages')
    entry = extract.pc24_to_offset(extract.pc24_from_string(ending['copy_routine']['entry_pc24']))
    mutate('ending-entry', entry, b'\xea')
    mutate('ending-signature', entry+20, b'\xea')
    return cases


def packed_blob(size, tokens):
    stream = ''.join('1' + f'{value:08b}' if kind == 'literal'
                     else '0' + f'{value:08b}{length-2:04b}'
                     for kind, value, length in tokens)
    stream += '0' * (-len(stream) % 8)
    return size.to_bytes(2, 'little') + bytes(int(stream[i:i+8], 2) for i in range(0, len(stream), 8))


def synthetic_destination_groups():
    groups = []
    for abi in ('native', 'shifted_western'):
        rom = bytearray(0x30000)
        contracts = dict(extract.CONSUMER_CENSUS_PROFILES['us'])

        def put(offset, data):
            if isinstance(data, str):
                data = bytes.fromhex(data)
            rom[offset:offset+len(data)] = data

        sites = [0x100+index*0x40 for index in range(5)]
        for index, offset in enumerate(sites):
            put(offset, extract.BG3_TEXT_BUFFER_WRITE)
            if index == 0:
                put(offset-12, 'a90000')
                put(offset-3, 'a2c000')
                put(offset+4, 'e8e8e00001d0f5')
            elif index == 1:
                put(offset-6, 'a2c000a92301')
                put(offset+4, '1ae8e8e0cc00d0f4')
            elif index == 2:
                put(offset-6, 'a90020a20001')
                put(offset+4, 'e8e8e00008d0f5')
            else:
                put(offset-6, 'a90020a20000')
                put(offset+4, 'e8e8e00007d0f5')
        hud_roles = tuple(extract.BG3_HUD_GRAPHICAL_TEXT_REGIONS)
        for index, role in enumerate(hud_roles):
            offset, source = 0x400+index*0x40, 0x2000+index*0x80
            put(offset-7, b'\xbf'+extract.offset_to_pc24(source).to_bytes(3, 'little')+bytes.fromhex('090020'))
            put(offset, '9f40b07fe8e8e08000d0ee')
            for cell in range(64):
                put(source+cell*2, (cell+1).to_bytes(2, 'little'))
        contracts.update(bg3_hud_template_sources=tuple((role, extract.offset_to_pc24(0x2000+index*0x80)) for index, role in enumerate(hud_roles)),
                         decoded_bg3_range_reference_count=10,
                         decoded_bg3_range_direct_long_count=9,
                         decoded_bg3_range_rejected_sites=((0x008600, '7e00b2'),),
                         vram_descriptor_abi=abi)
        put(0x600, '7e00b2')
        vram = [0x008700+index*8 for index in range(9)]
        instructions = ('8f182100', '8d1821', '8d1921', '9c1821')
        for index, site in enumerate(vram):
            put(extract.pc24_to_offset(site), instructions[index % len(instructions)])
        contracts['direct_vram_port_paths'] = (
            ('asset_script_character_upload', vram[:4]), ('developed_world_map_upload', vram[4:5]),
            ('bg3_tilemap_clear', vram[5:6]), ('action_obj_graphics_upload', vram[6:8]),
            ('simulation_tilemap_upload', vram[8:]))
        roles = extract.DMA_LAUNCH_ROLES if abi == 'native' else extract.DMA_LAUNCH_ROLES_JP
        contracts['dma_launch_sites'] = tuple((role, 0x008800+index*8) for index, role in enumerate(roles))
        for role, site in contracts['dma_launch_sites']:
            put(extract.pc24_to_offset(site), '9c0b42' if role == 'dma_disable' else '8e0b42' if role in ('tilemap_record', 'cgram_flicker') else '8d0b42')
        for index, (_, pattern) in enumerate(extract.VRAM_DESCRIPTOR_PATTERN_HEX[abi].items()):
            put(0x1000+index*0x40, pattern)
        contracts['indirect_write_sites'] = tuple((role, raw, (0x008c00+index*8,))
                                                for index, (role, raw, _) in enumerate(contracts['indirect_write_sites']))
        for role, raw, (site,) in contracts['indirect_write_sites']:
            put(extract.pc24_to_offset(site), raw)
        contracts['indirect_write_rejected_sites'] = ((0x008e00, '93f0'),)
        contracts['indirect_write_decoded_reference_count'] = len(contracts['indirect_write_sites'])+1
        put(0xe00, '93f0')
        put(0x1800, '48b00ea90f')
        put(0x1814, extract.ENDING_PAGE_COPY_SIGNATURE)
        for index in range(5):
            call = 0x1900+index*16
            put(call, '200098')
            if index != 1:
                put(call-3, bytes((0xa9, (0, 0, 17, 18, 19)[index], 0x18 if index == 0 else 0x38)))
        put(0x1913, '1ac91090')
        font_source = 0x2a000
        font_command = bytes((0x80, 0, 8, 0x50))+font_source.to_bytes(3, 'little')
        all_commands = b''.join(bytes(((1 << selector) | (1 if selector else 0),)) + bytes((0x11+selector,))*count
                                for selector, count in enumerate(extract.ASSET_COMMAND_OPERAND_BYTES))
        put(extract.ASSET_SCRIPT_BASE+3, b'\0\0'+all_commands+font_command+b'\0\x07\x08'+font_command+b'\0')
        font = packed_blob(4096, [('literal', index % 256, 0) for index in range(4096)])
        put(font_source, font)
        cases = []

        def add(method, oracle, site=0, role='', mutations=()):
            def evaluate(data):
                try:
                    return oracle(data), False
                except (ValueError, IndexError):
                    return None, True
            expected, reject = evaluate(bytes(rom))
            assert not reject, (method, role)
            cases.append(dict(id=method+'-'+role, method=method, site=site, role=role, expected=expected, reject=False))
            for label, offset, data, reject in mutations:
                changed = bytearray(rom)
                changed[offset:offset+len(data)] = data
                result, failed = evaluate(bytes(changed))
                assert failed == reject, (method, label)
                assert failed or result != expected, (method, label, 'no change')
                cases.append(dict(id=method+'-'+role+'-'+label, method=method, site=site, role=role,
                                  expected=result, reject=reject, offset=offset, data=list(data)))

        for offset, role in zip(sites, extract.BG3_OUTSIDE_WRITE_ROLES):
            site = extract.offset_to_pc24(offset)
            add('outside', lambda data, site=site, role=role: extract.classify_outside_bg3_write(data, site, role), site, role,
                [('broken-loop', offset+4, b'\xea', True)] +
                ([('changed-tile', offset-2, b'\x24', False)] if role == 'status_strip_sequential_tiles' else []))
        for index, role in enumerate(hud_roles):
            site, source = 0x008400+index*0x40, 0x2000+index*0x80
            add('hud', lambda data, site=site, role=role: extract.classify_bg3_hud_template_copy(data, site, role, contracts), site, role,
                [('bad-copy', extract.pc24_to_offset(site)+4, b'\xea', True), ('changed-tile', source, b'\x02', False),
                 ('empty-region', source, bytes(12), True)])
        add('range', lambda data: extract.classify_decoded_bg3_range_evidence(data, contracts), mutations=[('rejected-evidence-changed', 0x600, b'\xea', True)])
        add('vram', lambda data: extract.classify_direct_vram_port_paths(data, contracts), mutations=[('bad-port', 0x701, b'\x17', True), ('valid-store-kind', 0x708, b'\x9c', False)])
        add('dma', lambda data: extract.classify_dma_launches(data, contracts), mutations=[('bad-store-kind', 0x800, b'\x9c', True)])
        add('descriptor', lambda data: extract.build_vram_descriptor_census(data, contracts), mutations=[('lost-producer', 0x1000, b'\xea', True), ('duplicate-producer', 0x1500, bytes.fromhex(next(iter(extract.VRAM_DESCRIPTOR_PATTERN_HEX[abi].values()))), True)])
        add('indirect', lambda data: extract.classify_indirect_write_paths(data, contracts), mutations=[('bad-store', 0xc00, b'\xea', True), ('changed-rejected', 0xe00, b'\xea', True)])
        add('font', extract.build_dialog_font_census, mutations=[('wrong-size', font_source, b'\x01', True), ('changed-font-glyph', font_source+2, bytes((rom[font_source+2]^0x40,)), False)])
        add('asset_script', lambda data: [dict(mode=mode, submode=submode,
                                               commands=[dict(command=command, operands_hex=operands.hex()) for command, operands in commands])
                                         for mode, submode, commands in extract.iter_asset_script(data)])
        add('title', extract.build_title_graphical_text_surface, mutations=[('missing-title', extract.ASSET_SCRIPT_BASE+3, b'\x01', True), ('changed-command', extract.ASSET_SCRIPT_BASE+5, b'\x81', False)])
        add('ending', extract.build_ending_graphical_text_surface, mutations=[('bad-signature', 0x1814, b'\xea', True), ('bad-caller', 0x1940, b'\xea', True), ('bad-entry', 0x1800, b'\xea', True), ('longer-loop', 0x1915, b'\x11', False)])
        groups.append({'id': 'destinations-'+abi, 'rom': base64.b64encode(rom).decode('ascii'),
                       'profile': destination_profile(contracts), 'cases': cases})

    rng = random.Random(0x41524453)
    for phase in range(8):
        for length in range(2, 18):
            tokens = [('literal', 65+index, 0) for index in range(phase)] + [('match', 0xef, length)]
            blob = packed_blob(phase+length, tokens)
            decoded, consumed = extract.quintet_decompress(blob, 0)
            groups.append({'id': f'lzss-phase-{phase}-length-{length}', 'rom': base64.b64encode(blob).decode('ascii'),
                           'cases': [dict(id='overlap', method='decompress', expected={'decoded_hex': decoded.hex(), 'stream_cursor_bytes': consumed})]})
    for index in range(100):
        size = rng.randrange(0, 2048)
        tokens, remaining = [], size
        while remaining > 0:
            if rng.randrange(2):
                tokens.append(('literal', rng.randrange(256), 0))
                remaining -= 1
            else:
                length = rng.randrange(2, 18)
                tokens.append(('match', rng.randrange(256), length))
                remaining -= length
        blob = packed_blob(size, tokens)
        decoded, consumed = extract.quintet_decompress(blob, 0)
        groups.append({'id': f'lzss-generated-{index}', 'rom': base64.b64encode(blob).decode('ascii'),
                       'cases': [dict(id='mixed', method='decompress', expected={'decoded_hex': decoded.hex(), 'stream_cursor_bytes': consumed})]})
    return groups
