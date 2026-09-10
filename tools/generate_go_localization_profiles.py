#!/usr/bin/env python3
"""Generate ROM-free Go decoder facts from the verified extraction profiles.

No ROM or extracted prose is read. Keep executable behavior independently
tested against language_pack_extract.py; this file shares only declarative
encoding/consumer facts, not precomputed decoding results.
"""
import argparse
import json
from pathlib import Path
import unicodedata

import language_pack_extract as extract

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'snesrecomp-go/internal/localizationkit/data/decoder-profiles.json'
DISCOVERY_OUTPUT = OUTPUT.with_name('source-profiles.json')
CATALOG_OUTPUT = OUTPUT.with_name('catalog-profiles.json')
DESTINATION_OUTPUT = OUTPUT.with_name('destination-profiles.json')


def destination_profile(census):
    return {
        'base_write_count': census['bg3_buffer_write_count'],
        'outside_roles': census['bg3_outside_write_roles'],
        'long_write_count': census['bg3_direct_long_write_count'],
        'auxiliary': [dict(id=role, address=address) for role, address in census['bg3_direct_long_auxiliary']],
        'hud_sources': dict(census['bg3_hud_template_sources']),
        'context_words': census['sim_sky_context_label_word_count'],
        'noncode': census['bg3_direct_long_noncode'],
        'range_count': census['decoded_bg3_range_reference_count'],
        'range_long_count': census['decoded_bg3_range_direct_long_count'],
        'range_rejected': [dict(address=address, hex=raw) for address, raw in census['decoded_bg3_range_rejected_sites']],
        'vram_paths': [dict(id=role, sites=sites) for role, sites in census['direct_vram_port_paths']],
        'dma': [dict(id=role, address=address) for role, address in census['dma_launch_sites']],
        'indirect': [dict(id=role, hex=raw, sites=sites) for role, raw, sites in census['indirect_write_sites']],
        'indirect_rejected': [dict(address=address, hex=raw) for address, raw in census.get('indirect_write_rejected_sites', ())],
        'indirect_count': census.get('indirect_write_decoded_reference_count'),
        'descriptor_abi': census['vram_descriptor_abi'],
    }


def generated_destinations():
    # These are the existing decoded/data-flow audit contracts, NOT newly
    # discovered sources or proof results. Go revalidates their ROM evidence.
    return json.dumps({
        'profiles': {name: destination_profile(census) for name, census in extract.CONSUMER_CENSUS_PROFILES.items()},
        'hud_regions': {role: [dict(id=name, indices=indices) for name, indices in regions]
                        for role, regions in extract.BG3_HUD_GRAPHICAL_TEXT_REGIONS.items()},
        'vram_details': extract.DIRECT_VRAM_PATH_DETAILS,
        'dma_roles': extract.DMA_LAUNCH_ROLES,
        'dma_details': extract.DMA_LAUNCH_DETAILS,
        'descriptor_abis': extract.VRAM_DESCRIPTOR_ABIS,
        'descriptor_patterns': {abi: [dict(id=role, hex=raw) for role, raw in patterns.items()]
                                for abi, patterns in extract.VRAM_DESCRIPTOR_PATTERN_HEX.items()},
        'descriptor_details': extract.VRAM_DESCRIPTOR_FAMILY_DETAILS,
        'indirect_details': extract.INDIRECT_WRITE_FAMILY_DETAILS,
    }, sort_keys=True, indent=2) + '\n'


def catalog_profile(profile):
    japanese = profile['encoding'] == 'direct-glyph'
    title_key = profile['id'] if profile['id'] in ('us', 'jp') else 'regional_mode_select'
    routes = extract.JAPANESE_INTERACTIVE_ROUTE_IDS if japanese else extract.LATIN_INTERACTIVE_ROUTE_IDS
    return {
        'id': profile['id'], 'japanese': japanese,
        'town_table': profile.get('town_name_table', 0x20000),
        'enemy_table': profile.get('enemy_name_table', 0x20043),
        'angel_start': profile['angel_start'],
        'angel_end': profile.get('angel_end', profile['handler_table']),
        'handler_table': profile['handler_table'], 'town_start': profile['town_start'],
        'offering_table': profile['offering_table'],
        'ending_table': profile.get('ending_table'), 'post_text_end': profile.get('post_text_end'),
        'stage_table': profile['action_stage_name_table'],
        'title_start': profile['title_text_start'], 'title_end': profile['title_text_end'],
        'action_start': profile['action_label_start'], 'action_end': profile['action_label_end'],
        'sound_start': profile['sound_test_start'], 'sound_end': profile['sound_test_end'],
        'lookup_tables': [dict(id=name, offset=offset, semantic_ids=ids)
                          for name, offset, ids in profile.get('lookup_source_tables', ())],
        'title_route_ids': extract.TITLE_ROUTE_IDS[title_key],
        'regional_title': title_key == 'regional_mode_select',
        'interactive_routes': {call: list(ids) if isinstance(ids, tuple) else [ids]
                               for call, ids in routes.items()},
    }


def generated_catalogs():
    return json.dumps({
        'profiles': [catalog_profile(profile) for _, profile in sorted(extract.ROM_PROFILES.items())],
        'city_ids': extract.CITY_TERM_IDS, 'city_keys': extract.CITY_KEYS,
        'enemy_ids': extract.ENEMY_TERM_IDS,
        'action_ids': [value or '' for value in extract.ACTION_HUD_IDS],
        'composer_pointer_routes': extract.COMPOSER_POINTER_ROUTE_IDS,
    }, sort_keys=True, indent=2) + '\n'


def source_profile(profile, census):
    """Only structural facts, never discovered calls, pointers or ROM text."""
    def named_sources(key):
        return [dict(id=name, address=address) for name, address in census.get(key, ())]

    direct = census.get('composer_direct_source_table')
    dictionary = extract.DICTIONARY_CONSUMER_PROFILES.get(profile['id'])
    flow = census.get('composer_flow_sources')
    if flow is not None:
        routes = extract.JAPANESE_INTERACTIVE_ROUTE_IDS if profile['id'] == 'jp' \
            else extract.LATIN_INTERACTIVE_ROUTE_IDS
        flow = dict(selector_call=flow['message_speed_selector_call_site'],
                    sample_call=next(call for call, name in routes.items()
                                     if name == 'system.message_speed.sample'),
                    choice_yield_call=flow.get('choice_yield_call_site'),
                    choice_descriptor=flow.get('choice_descriptor_pc24'),
                    choice_load_sites=flow.get('choice_descriptor_load_sites', []))
    return {
        'id': profile['id'],
        'interactive_entry': census['interactive_entry_pc24'],
        'interactive_end': census['interactive_end_pc24'],
        'composer_entry': census['composer_entry_pc24'],
        'composer_end': census['composer_end_pc24'],
        'interactive_call_count': census['interactive_call_count'],
        'nonadjacent': census['interactive_nonadjacent_layout'],
        'branch_joins': dict(census.get('interactive_branch_join_layout', ())),
        'continuations': [dict(call=call, parent=parent, source=source)
                          for call, parent, source in census.get('interactive_yield_continuations', ())],
        'dialogue_bank': census['dialogue_source_bank'],
        'wrappers': [dict(entry=entry, kind=kind, count=count, source_bank=bank)
                     for entry, kind, count, bank in census['dialogue_wrappers']],
        'relay': dict(entry=census['dialogue_source_relay'][0],
                      count=census['dialogue_source_relay'][1]),
        'handler_table': extract.offset_to_pc24(profile['handler_table']),
        'offering_table': extract.offset_to_pc24(profile.get('offering_table', 0)),
        'ending_table': extract.offset_to_pc24(profile['ending_table']) if 'ending_table' in profile else None,
        'menu_start': profile['menu_start'], 'menu_end': profile['menu_end'],
        'tables': [dict(id=name, address=address, count=count, increment=increment)
                   for name, address, count, increment in census['composer_source_tables']],
        'direct_table': dict(address=direct[0], ids=direct[1], increment=direct[2]) if direct else None,
        'indexed_direct': [dict(id=name, address=address, native_index=index,
                                table=table, ids=ids)
                           for name, address, index, table, ids in census.get('composer_indexed_direct_sources', ())],
        'dynamic': named_sources('composer_dynamic_sources'),
        'numeric': census.get('composer_numeric_source'),
        'name_entry': named_sources('composer_name_entry_sources'),
        'groups': [dict(id=name, count=count) for name, count in census['composer_groups']],
        'flow': flow,
        'dictionary_proof': dict(interactive=dictionary[0], fixed=dictionary[1],
                                 upload_flag=dictionary[2], trailing_space=dictionary[3]) if dictionary else None,
        'space_delimited_words': profile['encoding'] != 'direct-glyph',
    }


def generated_sources():
    profiles = [source_profile(profile, extract.CONSUMER_CENSUS_PROFILES[profile['id']])
                for _, profile in sorted(extract.ROM_PROFILES.items())]
    return json.dumps(profiles, sort_keys=True, indent=2) + '\n'


def generated():
    profiles = []
    for digest, profile in sorted(extract.ROM_PROFILES.items()):
        census = extract.CONSUMER_CENSUS_PROFILES[profile['id']]
        population = dict(census.get('composer_dynamic_sources', ())).get('cities_report')
        profiles.append({
            'id': profile['id'], 'locale': profile['locale'],
            'label': profile['label'], 'sha256': digest,
            'encoding': profile['encoding'],
            'dictionary': profile.get('dictionary', 0),
            'interactive_full_entry_separator': ' ' if profile['id'] in ('de', 'fr') else '',
            'glyphs': extract.base_glyph_map(profile.get('glyph_overrides')),
            'icons': profile.get('icon_glyphs', {}),
            'number_semantics': profile.get('number_semantics', {}),
            'indexed_text_semantics': profile.get('indexed_text_semantics', {}),
            'name_prefix': profile.get('dialogue_name_prefix'),
            'name_separator': profile.get('dialogue_name_separator', ''),
            'population_source': extract.pc24_to_offset(population) if population else None,
            'population_codes': [0x5b, 0x5c] if profile['id'] == 'fr' else [0x3a, 0x3b],
            'speed_codes': [0x1d, 0x1c] if profile['id'] == 'jp' else [0x3d, 0x3c],
            'speed_source_call': census.get('composer_flow_sources', {}).get(
                'message_speed_selector_call_site'),
        })
    compositions = {}
    for base in range(0x3040, 0x3100):
        for mark in ('\u3099', '\u309a'):
            pair = chr(base) + mark
            normalized = unicodedata.normalize('NFC', pair)
            if normalized != pair:
                compositions[pair] = normalized
    return json.dumps({'profiles': profiles, 'kana_compositions': compositions},
                      ensure_ascii=False, sort_keys=True, indent=2) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for output, content in ((OUTPUT, generated()), (DISCOVERY_OUTPUT, generated_sources()),
                            (CATALOG_OUTPUT, generated_catalogs()),
                            (DESTINATION_OUTPUT, generated_destinations())):
        if args.check:
            if not output.is_file() or output.read_text(encoding='utf-8') != content:
                raise SystemExit(f'{output.name} is stale; run ' + Path(__file__).name)
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(content, encoding='utf-8')
    if args.check:
        print('Go localization profiles match the authoritative facts')


if __name__ == '__main__':
    main()
