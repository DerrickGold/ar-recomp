#!/usr/bin/env python3
"""Extract preliminary ActRaiser language source IR from clean localized ROMs.

German and French use the USA-style 128-entry dictionary compressor and can
be decoded to Unicode text. Japanese uses direct font-tile codes. Its verified
hiragana, katakana, punctuation, and dakuten/handakuten sequences decode to
Unicode. Verified UI symbols become typed ``insert_icon`` operations; any
still-unassigned tile is preserved as ``native_glyphs`` rather than guessed.

The JSON output is a source/provenance format for the future pack compiler. It
is not a language pack and is not consumed by the runtime. Extracted retail text is
copyrighted ROM-derived data: keep output under the ignored game-assets tree
or another private directory and do not redistribute it.

Examples:
  tools/language_pack_extract.py ar-ger.sfc ar-fra.sfc ar-jp.sfc \
      --out-dir game-assets/languages/preliminary
  tools/language_pack_extract.py ar-ger.sfc --out-dir /tmp/ar-languages
"""

import argparse
import hashlib
import json
import sys
import unicodedata
import zlib
from pathlib import Path

# Tests load this file as a module from the repository root, while direct CLI
# use already places tools/ on sys.path.  Support both without packaging these
# intentionally standalone ROM-analysis helpers.
TOOLS_DIRECTORY = str(Path(__file__).resolve().parent)
if TOOLS_DIRECTORY not in sys.path:
    sys.path.insert(0, TOOLS_DIRECTORY)
from quintet_lzss import decompress as quintet_decompress


EXTRACTION_FORMAT = 'actraiser-language-extraction'
EXTRACTION_FORMAT_VERSION = 1
INDEX_FORMAT = 'actraiser-language-extraction-index'
INDEX_FORMAT_VERSION = 1


# Both known text paths ultimately compose a BG3 tilemap in $7F:B000.  These
# entry signatures deliberately contain only bytes shared by all five exact
# releases; helper and branch destinations differ between releases.
INTERACTIVE_CONSUMER_SIGNATURE = bytes.fromhex(
    '08 da e2 20 a9 ff 8d 01 02 ad 00 02 d0 10')
FIXED_COMPOSER_SIGNATURE = bytes.fromhex(
    '08 da c2 20 85 14 e2 20 a5 15 eb a9 40')
BG3_TEXT_BUFFER_WRITE = bytes.fromhex('9f 00 b0 7f')
ASSET_SCRIPT_BASE = 0x028000
ASSET_SCRIPT_HEADER_BYTES = 3
ASSET_SCRIPT_END = (0x07, 0x08)
ASSET_COMMAND_OPERAND_BYTES = (6, 5, 3, 1, 4, 7, 6, 6)
DIALOG_FONT_COMMAND_PREFIX = bytes((0x00, 0x08, 0x50))
DIALOG_FONT_DECODED_BYTES = 0x1000
ENDING_PAGE_COPY_SIGNATURE = bytes.fromhex(
    'a3 01 8b c2 20 29 ff 00 0a 0a 0a eb 18 69 00 40 aa a0 00 b0 '
    'a9 ff 07 54 7f 7e e2 20 ab e6')
ENDING_PAGE_BYTES = 0x800
TITLE_GRAPHICAL_TEXT_RECT = (11, 27, 248, 122)

LATIN_INTERACTIVE_NONADJACENT = (
    'conditional_immediate', 'nested_handler_table',
    'offering_pointer_table', 'yield_continuation',
    'dialogue_wrapper', 'dialogue_wrapper', 'dialogue_wrapper',
    'dialogue_wrapper', 'dialogue_wrapper', 'dialogue_wrapper',
    'dialogue_wrapper')
LATIN_DIALOGUE_WRAPPERS = (
    (0x019314, 'jsl', 18, 0x04), (0x01933C, 'jsl', 4, 0x04),
    (0x01935E, 'jsl', 4, 0x04), (0x01937D, 'jsl', 2, 0x04),
    (0x019396, 'jsl', 13, 0x04), (0x0193A8, 'jsr', 28, 0x04),
    # This lightweight menu wrapper intentionally retains the caller's bank.
    (0x0193B4, 'jsr', 6, 0x01))

BG3_OUTSIDE_WRITE_ROLES = (
    'status_strip_clear',
    'status_strip_sequential_tiles',
    'dialogue_surface_clear',
    'general_surface_clear',
    'city_pause_surface_clear',
)

BG3_HUD_GRAPHICAL_TEXT_REGIONS = {
    'action_hud_template': (
        ('action.hud.act_label', tuple(range(0, 6))),
        ('action.hud.time_label', tuple(range(11, 15))),
        ('action.hud.score_label', tuple(range(21, 26))),
        ('action.hud.player_label', tuple(range(32, 38))),
    ),
    'sim_sky_hud_template': (
        # The context label is release-specific below: the Japanese artwork
        # continues through four cells that are blank in Western releases.
        ('sim_sky.hud.context_label', ()),
        ('sim_sky.hud.angel_label', tuple(range(32, 38))),
        ('sim_sky.hud.sp_label', (53, 54)),
    ),
}

DMA_LAUNCH_ROLES = (
    'oam_shadow', 'tilemap_record', 'cgram_flicker', 'cgram_descriptor',
    'simulation_town_tilemap', 'bg3_status_rows', 'bg3_dialogue_rows',
    'generic_vram_descriptor', 'world_water_bg', 'world_water_obj',
    'world_effect', 'dma_disable')
DMA_LAUNCH_ROLES_JP = tuple(
    role for role in DMA_LAUNCH_ROLES if role != 'world_effect')


# Structural identities for the exact supported ROMs.  Ordered composer
# groups describe the sorted JSL call sites without baking their addresses
# into distributable coverage reports.  The extraction IR retains addresses
# locally for the later semantic-alignment pass.
CONSUMER_CENSUS_PROFILES = {
    'us': {
        'interactive_entry_pc24': 0x018E29,
        'interactive_end_pc24': 0x0190CD,
        'interactive_call_count': 59,
        'interactive_nonadjacent_layout': LATIN_INTERACTIVE_NONADJACENT,
        'interactive_branch_join_layout': (
            (0x01876B, 5), (0x0187E6, 5),
            (0x01898C, 5), (0x018AEE, 2)),
        'interactive_yield_continuations': (
            (0x018C3B, 0x01F699, 0x01F6C4),),
        'dialogue_source_bank': 0x04,
        'dialogue_wrappers': LATIN_DIALOGUE_WRAPPERS,
        'dialogue_source_relay': (0x038685, 6),
        'composer_source_tables': (
            ('selected_magic', 0x01F04F, 4, 1),
            ('selected_possession', 0x01F08E, 20, 1),
            ('city_name', 0x01F1BD, 7, 0),
            ('sky_root', 0x01F272, 8, 0),
            ('sky_choice', 0x01F290, 4, 0),
            ('sim_root', 0x01F34C, 15, 0),
            ('sim_choice', 0x01F36A, 6, 0)),
        'composer_direct_source_table': (
            0x01F223,
            ('give_oracle', 'listen', 'take_offering'), 2),
        'composer_dynamic_sources': (
            ('master_report', 0x01F484),
            ('cities_report', 0x01F4DC),
            ('score_report', 0x01F5BC)),
        'composer_numeric_source': 0x01F46D,
        'composer_name_entry_sources': (
            ('prompt_and_alphabet', 0x01EF3B),
            ('selection_cursor', 0x01EFE6)),
        'composer_flow_sources': {
            'message_speed_selector_call_site': 0x018B22,
            'choice_yield_call_site': 0x018C3B,
        },
        'composer_entry_pc24': 0x02BF60,
        'composer_end_pc24': 0x02C3D9,
        'composer_groups': (
            ('generated_value', 1), ('action_ui', 7),
            ('sim_and_sky_menu', 4), ('sound_test', 2),
            ('title_and_mode', 6), ('city_and_pause', 2),
            ('generated_value', 1), ('name_entry', 2)),
        'bg3_buffer_write_count': 33,
        'bg3_outside_write_roles': BG3_OUTSIDE_WRITE_ROLES,
        'bg3_direct_long_write_count': 50,
        'bg3_direct_long_auxiliary': (
            ('manual_dialogue_attribute', 0x0189D5),
            ('action_hud_template', 0x02BA66),
            ('sim_sky_hud_template', 0x02BA7D)),
        'bg3_hud_template_sources': (
            ('action_hud_template', 0x028E7E),
            ('sim_sky_hud_template', 0x028EFE)),
        'sim_sky_context_label_word_count': 6,
        'bg3_direct_long_noncode': (0x11893C, 0x128B91),
        'decoded_bg3_range_reference_count': 57,
        'decoded_bg3_range_direct_long_count': 48,
        'decoded_bg3_range_rejected_sites': (
            (0x00BA36, '7e00b2'), (0x00BA4A, '7e00b2'),
            (0x00BA5E, '7e00b2'), (0x00BA72, '7e00b2'),
            (0x00CC0B, '7e00b2'), (0x00CC1F, '7e00b2'),
            (0x00CC33, '7e00b2'), (0x00CCCF, '7e00b2'),
            (0x00CCF7, '7e00b2')),
        'direct_vram_port_paths': (
            ('asset_script_character_upload',
             (0x02B2C6, 0x02B2E0, 0x02B30B, 0x02B325)),
            ('developed_world_map_upload', (0x02B4AF,)),
            ('bg3_tilemap_clear', (0x02BAB9,)),
            ('action_obj_graphics_upload', (0x02BCB0, 0x02BCF1)),
            ('simulation_tilemap_upload', (0x038103,))),
        'dma_launch_sites': tuple(zip(DMA_LAUNCH_ROLES, (
            0x02ACC4, 0x02ADBF, 0x02AE30, 0x02AEA6,
            0x02AEE7, 0x02AF0E, 0x02AF2C, 0x02AF65,
            0x02AFBC, 0x02AFC7, 0x02AFF4, 0x02C7DB))),
        'indirect_write_sites': (
            ('oam_high_table', '929a', (
                0x008D64, 0x008E02, 0x0092B1,
                0x01ADF9, 0x01AE0D, 0x01AEBB, 0x01AECF)),
            ('developed_world_map_stamp', '97a8', (0x0286EF,)),
            ('deferred_ppu_register', '92ea', (0x02AC44,)),
            ('asset_workspace', '97a8', (0x02B39C,)),
            ('metatile_definitions', '97a8', (0x02B3DE,)),
            ('action_map', '97a8', (0x02B461,)),
            ('developed_world_map', '97a8', (0x02B485,)),
            ('character_vram_readback', '97a8', (0x02BB1D,)),
            ('lzss_ring', '92af', (0x02C5DE, 0x02C60B, 0x02C623)),
        ),
        # A rooted US CFG census is the executable-code baseline. These are
        # the seven width-confused/data decodes from its 24 reported indirect
        # stores; none is an instruction on a valid execution path.
        'indirect_write_rejected_sites': (
            (0x008115, '8103'), (0x00C852, '9190'),
            (0x00F6C7, '9202'),
            (0x02B12A, '93f0'), (0x02B136, '93f0'),
            (0x02B142, '93f0'), (0x02B14E, '93f0'),
        ),
        'indirect_write_decoded_reference_count': 24,
        'vram_descriptor_abi': 'native',
    },
    'eu-en': {
        'interactive_entry_pc24': 0x018E29,
        'interactive_end_pc24': 0x0190CD,
        'interactive_call_count': 59,
        'interactive_nonadjacent_layout': LATIN_INTERACTIVE_NONADJACENT,
        'interactive_branch_join_layout': (
            (0x01876B, 5), (0x0187E6, 5),
            (0x01898C, 5), (0x018AEE, 2)),
        'interactive_yield_continuations': (
            (0x018C3B, 0x01F6A1, 0x01F6CC),),
        'dialogue_source_bank': 0x04,
        'dialogue_wrappers': LATIN_DIALOGUE_WRAPPERS,
        'dialogue_source_relay': (0x038685, 6),
        'composer_source_tables': (
            ('selected_magic', 0x01F04F, 4, 1),
            ('selected_possession', 0x01F08E, 20, 1),
            ('city_name', 0x01F1C5, 7, 0),
            ('sky_root', 0x01F27A, 8, 0),
            ('sky_choice', 0x01F298, 4, 0),
            ('sim_root', 0x01F354, 15, 0),
            ('sim_choice', 0x01F372, 6, 0)),
        'composer_direct_source_table': (
            0x01F22B,
            ('give_oracle', 'listen', 'take_offering'), 2),
        'composer_dynamic_sources': (
            ('master_report', 0x01F48C),
            ('cities_report', 0x01F4E4),
            ('score_report', 0x01F5C4)),
        'composer_numeric_source': 0x01F475,
        'composer_name_entry_sources': (
            ('prompt_and_alphabet', 0x01EF3B),
            ('selection_cursor', 0x01EFE6)),
        'composer_flow_sources': {
            'message_speed_selector_call_site': 0x018B22,
            'choice_yield_call_site': 0x018C3B,
        },
        'composer_entry_pc24': 0x02C579,
        'composer_end_pc24': 0x02C9F2,
        'composer_groups': (
            ('action_ui', 7), ('sim_and_sky_menu', 4),
            ('sound_test', 2), ('title_and_mode', 5),
            ('city_and_pause', 2), ('generated_value', 1),
            ('name_entry', 2)),
        'bg3_buffer_write_count': 33,
        'bg3_outside_write_roles': BG3_OUTSIDE_WRITE_ROLES,
        'bg3_direct_long_write_count': 50,
        'bg3_direct_long_auxiliary': (
            ('manual_dialogue_attribute', 0x0189D5),
            ('action_hud_template', 0x02C05E),
            ('sim_sky_hud_template', 0x02C075)),
        'bg3_hud_template_sources': (
            ('action_hud_template', 0x028E7E),
            ('sim_sky_hud_template', 0x028EFE)),
        'sim_sky_context_label_word_count': 6,
        'bg3_direct_long_noncode': (0x11893C, 0x128B91),
        'decoded_bg3_range_reference_count': 49,
        'decoded_bg3_range_direct_long_count': 46,
        'decoded_bg3_range_rejected_sites': (
            (0x00B702, '7e00b2'), (0x00B72A, '7e00b2'),
            (0x00C9BE, '7e00b2')),
        'direct_vram_port_paths': (
            ('asset_script_character_upload',
             (0x02B8BD, 0x02B8D7, 0x02B902, 0x02B91C)),
            ('developed_world_map_upload', (0x02BAA6,)),
            ('bg3_tilemap_clear', (0x02C0B1,)),
            ('action_obj_graphics_upload', (0x02C2A9, 0x02C30A)),
            ('simulation_tilemap_upload', (0x038103,))),
        'dma_launch_sites': tuple(zip(DMA_LAUNCH_ROLES, (
            0x02AD5D, 0x02AE58, 0x02AEC9, 0x02AF3F,
            0x02AF80, 0x02AFA7, 0x02AFC5, 0x02AFFE,
            0x02B055, 0x02B060, 0x02B08D, 0x02CDF4))),
        'indirect_write_sites': (
            ('oam_high_table', '929b', (
                0x01ADF9, 0x01AE0D, 0x01AEBB, 0x01AECF,
                0x02B493, 0x02B531, 0x02B5D5)),
            ('developed_world_map_stamp', '97a9', (0x0286EF,)),
            ('deferred_ppu_register', '92eb', (0x02ACDD,)),
            ('asset_workspace', '97a9', (0x02B993,)),
            ('metatile_definitions', '97a9', (0x02B9D5,)),
            ('action_map', '97a9', (0x02BA58,)),
            ('developed_world_map', '97a9', (0x02BA7C,)),
            ('character_vram_readback', '97a9', (0x02C115,)),
            ('lzss_ring', '92b0', (0x02CBF7, 0x02CC24, 0x02CC3C)),
        ),
        'vram_descriptor_abi': 'shifted_western',
    },
    'de': {
        'interactive_entry_pc24': 0x018E29,
        'interactive_end_pc24': 0x0190CD,
        'interactive_call_count': 59,
        'interactive_nonadjacent_layout': LATIN_INTERACTIVE_NONADJACENT,
        'interactive_branch_join_layout': (
            (0x01876B, 5), (0x0187E6, 5),
            (0x01898C, 5), (0x018AEE, 2)),
        'interactive_yield_continuations': (
            (0x018C3B, 0x01F6B3, 0x01F6D2),),
        'dialogue_source_bank': 0x04,
        'dialogue_wrappers': LATIN_DIALOGUE_WRAPPERS,
        'dialogue_source_relay': (0x038685, 6),
        'composer_source_tables': (
            ('selected_magic', 0x01F069, 4, 1),
            ('selected_possession', 0x01F0C1, 20, 1),
            ('city_name', 0x01F210, 7, 0),
            ('sky_root', 0x01F2B1, 8, 0),
            ('sky_choice', 0x01F2CF, 4, 0),
            ('sim_root', 0x01F375, 15, 0),
            ('sim_choice', 0x01F393, 6, 0)),
        'composer_direct_source_table': (
            0x01F276,
            ('give_oracle', 'listen', 'take_offering'), 2),
        'composer_dynamic_sources': (
            ('master_report', 0x01F48A),
            ('cities_report', 0x01F4E4),
            ('score_report', 0x01F5D0)),
        'composer_numeric_source': 0x01F473,
        'composer_name_entry_sources': (
            ('prompt_and_alphabet', 0x01EF3B),
            ('selection_cursor', 0x01EFEC)),
        'composer_flow_sources': {
            'message_speed_selector_call_site': 0x018B22,
            'choice_yield_call_site': 0x018C3B,
        },
        'composer_entry_pc24': 0x02C582,
        'composer_end_pc24': 0x02C9FB,
        'composer_groups': (
            ('action_ui', 7), ('sim_and_sky_menu', 4),
            ('sound_test', 2), ('title_and_mode', 5),
            ('city_and_pause', 2), ('generated_value', 1),
            ('name_entry', 2)),
        'bg3_buffer_write_count': 33,
        'bg3_outside_write_roles': BG3_OUTSIDE_WRITE_ROLES,
        'bg3_direct_long_write_count': 50,
        'bg3_direct_long_auxiliary': (
            ('manual_dialogue_attribute', 0x0189D5),
            ('action_hud_template', 0x02C067),
            ('sim_sky_hud_template', 0x02C07E)),
        'bg3_hud_template_sources': (
            ('action_hud_template', 0x028E7E),
            ('sim_sky_hud_template', 0x028EFE)),
        'sim_sky_context_label_word_count': 6,
        'bg3_direct_long_noncode': (0x11893C, 0x128B91),
        'decoded_bg3_range_reference_count': 53,
        'decoded_bg3_range_direct_long_count': 46,
        'decoded_bg3_range_rejected_sites': (
            (0x00B6F0, '7e00b2'), (0x00B704, '7e00b2'),
            (0x00B718, '7e00b2'), (0x00B72C, '7e00b2'),
            (0x00BB08, '0c00b0'), (0x00C9C0, '7e00b2'),
            (0x00F0C1, '8df1b9')),
        'direct_vram_port_paths': (
            ('asset_script_character_upload',
             (0x02B8C6, 0x02B8E0, 0x02B90B, 0x02B925)),
            ('developed_world_map_upload', (0x02BAAF,)),
            ('bg3_tilemap_clear', (0x02C0BA,)),
            ('action_obj_graphics_upload', (0x02C2B2, 0x02C313)),
            ('simulation_tilemap_upload', (0x038103,))),
        'dma_launch_sites': tuple(zip(DMA_LAUNCH_ROLES, (
            0x02AD66, 0x02AE61, 0x02AED2, 0x02AF48,
            0x02AF89, 0x02AFB0, 0x02AFCE, 0x02B007,
            0x02B05E, 0x02B069, 0x02B096, 0x02CDFD))),
        'indirect_write_sites': (
            ('oam_high_table', '929b', (
                0x01ADF9, 0x01AE0D, 0x01AEBB, 0x01AECF,
                0x02B49C, 0x02B53A, 0x02B5DE)),
            ('developed_world_map_stamp', '97a9', (0x0286EF,)),
            ('deferred_ppu_register', '92eb', (0x02ACE6,)),
            ('asset_workspace', '97a9', (0x02B99C,)),
            ('metatile_definitions', '97a9', (0x02B9DE,)),
            ('action_map', '97a9', (0x02BA61,)),
            ('developed_world_map', '97a9', (0x02BA85,)),
            ('character_vram_readback', '97a9', (0x02C11E,)),
            ('lzss_ring', '92b0', (0x02CC00, 0x02CC2D, 0x02CC45)),
        ),
        'vram_descriptor_abi': 'shifted_western',
    },
    'fr': {
        'interactive_entry_pc24': 0x018E29,
        'interactive_end_pc24': 0x0190CD,
        'interactive_call_count': 59,
        'interactive_nonadjacent_layout': LATIN_INTERACTIVE_NONADJACENT,
        'interactive_branch_join_layout': (
            (0x01876B, 5), (0x0187E6, 5),
            (0x01898C, 5), (0x018AEE, 2)),
        'interactive_yield_continuations': (
            (0x018C3B, 0x01F6EA, 0x01F719),),
        'dialogue_source_bank': 0x04,
        'dialogue_wrappers': LATIN_DIALOGUE_WRAPPERS,
        'dialogue_source_relay': (0x038685, 6),
        'composer_source_tables': (
            ('selected_magic', 0x01F04D, 4, 1),
            ('selected_possession', 0x01F094, 20, 1),
            ('city_name', 0x01F1D1, 7, 0),
            ('sky_root', 0x01F27E, 8, 0),
            ('sky_choice', 0x01F29C, 4, 0),
            ('sim_root', 0x01F352, 15, 0),
            ('sim_choice', 0x01F370, 6, 0)),
        'composer_direct_source_table': (
            0x01F237,
            ('give_oracle', 'listen', 'take_offering'), 2),
        'composer_dynamic_sources': (
            ('master_report', 0x01F48E),
            ('cities_report', 0x01F4E8),
            ('score_report', 0x01F5F6)),
        'composer_numeric_source': 0x01F477,
        'composer_name_entry_sources': (
            ('prompt_and_alphabet', 0x01EF3B),
            ('selection_cursor', 0x01EFE9)),
        'composer_flow_sources': {
            'message_speed_selector_call_site': 0x018B22,
            'choice_yield_call_site': 0x018C3B,
        },
        'composer_entry_pc24': 0x02C56B,
        'composer_end_pc24': 0x02C9E4,
        'composer_groups': (
            ('action_ui', 7), ('sim_and_sky_menu', 4),
            ('sound_test', 2), ('title_and_mode', 5),
            ('city_and_pause', 2), ('generated_value', 1),
            ('name_entry', 2)),
        'bg3_buffer_write_count': 33,
        'bg3_outside_write_roles': BG3_OUTSIDE_WRITE_ROLES,
        'bg3_direct_long_write_count': 50,
        'bg3_direct_long_auxiliary': (
            ('manual_dialogue_attribute', 0x0189D5),
            ('action_hud_template', 0x02C050),
            ('sim_sky_hud_template', 0x02C067)),
        'bg3_hud_template_sources': (
            ('action_hud_template', 0x028E7E),
            ('sim_sky_hud_template', 0x028EFE)),
        'sim_sky_context_label_word_count': 6,
        'bg3_direct_long_noncode': (0x11893C, 0x128B91),
        'decoded_bg3_range_reference_count': 53,
        'decoded_bg3_range_direct_long_count': 47,
        'decoded_bg3_range_rejected_sites': (
            (0x00B6F3, '7e00b2'), (0x00B71B, '7e00b2'),
            (0x00BB0B, '0c00b0'), (0x00C722, '8cb0b7'),
            (0x00C8D7, '7e00b2'), (0x00C8FF, '7e00b2')),
        'direct_vram_port_paths': (
            ('asset_script_character_upload',
             (0x02B8AF, 0x02B8C9, 0x02B8F4, 0x02B90E)),
            ('developed_world_map_upload', (0x02BA98,)),
            ('bg3_tilemap_clear', (0x02C0A3,)),
            ('action_obj_graphics_upload', (0x02C29B, 0x02C2FC)),
            ('simulation_tilemap_upload', (0x038103,))),
        'dma_launch_sites': tuple(zip(DMA_LAUNCH_ROLES, (
            0x02AD4F, 0x02AE4A, 0x02AEBB, 0x02AF31,
            0x02AF72, 0x02AF99, 0x02AFB7, 0x02AFF0,
            0x02B047, 0x02B052, 0x02B07F, 0x02CDE6))),
        'indirect_write_sites': (
            ('oam_high_table', '929b', (
                0x01ADF9, 0x01AE0D, 0x01AEBB, 0x01AECF,
                0x02B485, 0x02B523, 0x02B5C7)),
            ('developed_world_map_stamp', '97a9', (0x0286EF,)),
            ('deferred_ppu_register', '92eb', (0x02ACCF,)),
            ('asset_workspace', '97a9', (0x02B985,)),
            ('metatile_definitions', '97a9', (0x02B9C7,)),
            ('action_map', '97a9', (0x02BA4A,)),
            ('developed_world_map', '97a9', (0x02BA6E,)),
            ('character_vram_readback', '97a9', (0x02C107,)),
            ('lzss_ring', '92b0', (0x02CBE9, 0x02CC16, 0x02CC2E)),
        ),
        'vram_descriptor_abi': 'shifted_western',
    },
    'jp': {
        'interactive_entry_pc24': 0x018DAA,
        'interactive_end_pc24': 0x01900D,
        'interactive_call_count': 60,
        'interactive_branch_join_layout': (
            (0x018720, 5), (0x01879B, 5),
            (0x018969, 5), (0x018A85, 2)),
        'interactive_nonadjacent_layout': (
            'conditional_immediate', 'nested_handler_table',
            'offering_pointer_table',
            'dialogue_wrapper', 'dialogue_wrapper', 'dialogue_wrapper',
            'dialogue_wrapper', 'dialogue_wrapper', 'dialogue_wrapper',
            'dialogue_wrapper'),
        'dialogue_source_bank': 0x02,
        'dialogue_wrappers': (
            (0x0192A9, 'jsl', 19, 0x02),
            (0x0192D1, 'jsl', 4, 0x02),
            (0x0192F3, 'jsl', 4, 0x02),
            (0x019312, 'jsl', 2, 0x02),
            (0x01932B, 'jsl', 13, 0x02),
            (0x01933D, 'jsr', 28, 0x02),
            (0x019349, 'jsr', 7, 0x01)),
        'dialogue_source_relay': (0x038635, 6),
        'composer_source_tables': (
            ('selected_magic', 0x01EF15, 4, 1),
            ('selected_possession', 0x01EF48, 20, 1),
            ('city_name', 0x01F059, 7, 0),
            ('sky_root', 0x01F196, 8, 0),
            ('sky_choice', 0x01F1B4, 4, 0),
            ('sim_root', 0x01F24E, 15, 0),
            ('sim_choice', 0x01F26C, 6, 0)),
        # Unlike the Western code-side three-way descriptor selection,
        # Japanese opcode 08 reads $032C and dispatches through this table.
        'composer_indexed_direct_sources': (
            ('offering_action', 0x01F159, 0x032C, 0x01F15F,
             ('give_oracle', 'listen', 'take_offering')),),
        'composer_dynamic_sources': (
            ('master_report', 0x01F35A),
            ('cities_report', 0x01F3B8)),
        'composer_numeric_source': 0x01F343,
        'composer_name_entry_sources': (
            ('prompt_and_hiragana', 0x01ED34),
            ('prompt_and_katakana', 0x01EDFB),
            ('selection_cursor', 0x01EEC2)),
        'composer_flow_sources': {
            'message_speed_selector_call_site': 0x018AB9,
            'choice_descriptor_pc24': 0x01F51A,
            'choice_descriptor_load_sites': (0x018D17, 0x018D76),
        },
        'composer_entry_pc24': 0x048F56,
        'composer_end_pc24': 0x049314,
        'composer_groups': (
            ('generated_value', 1), ('action_ui', 7),
            ('sim_and_sky_menu', 4), ('sound_test', 2),
            ('title_and_mode', 6), ('city_and_pause', 2),
            ('generated_value', 1), ('name_entry', 2)),
        'bg3_buffer_write_count': 26,
        'bg3_outside_write_roles': BG3_OUTSIDE_WRITE_ROLES,
        'bg3_direct_long_write_count': 37,
        'bg3_direct_long_auxiliary': (
            ('action_hud_template', 0x048A61),
            ('sim_sky_hud_template', 0x048A78)),
        'bg3_hud_template_sources': (
            ('action_hud_template', 0x028D27),
            ('sim_sky_hud_template', 0x028DA7)),
        'sim_sky_context_label_word_count': 10,
        'bg3_direct_long_noncode': (0x11BEA9, 0x11EE88),
        'decoded_bg3_range_reference_count': 41,
        'decoded_bg3_range_direct_long_count': 34,
        'decoded_bg3_range_rejected_sites': (
            (0x009007, '3e00bd'), (0x00E95E, '1c00b9'),
            (0x00EFD8, '3e20bc'), (0x00F714, '1ca9b2'),
            (0x01B96A, '1c00b9'), (0x01C7C9, '8c03bf'),
            (0x03EB5D, '9dedb0')),
        'direct_vram_port_paths': (
            ('asset_script_character_upload',
             (0x0482CE, 0x0482E8, 0x048313, 0x04832D)),
            ('developed_world_map_upload', (0x0484B7,)),
            ('bg3_tilemap_clear', (0x048AB4,)),
            ('action_obj_graphics_upload', (0x048CAB, 0x048CEC)),
            ('simulation_tilemap_upload', (0x038100,))),
        'dma_launch_sites': tuple(zip(DMA_LAUNCH_ROLES_JP, (
            0x02A9EA, 0x02AAE5, 0x02AB56, 0x02ABCC,
            0x02AC0D, 0x02AC34, 0x02AC52, 0x02AC8B,
            0x02ACE2, 0x02ACED, 0x04983B))),
        'indirect_write_sites': (
            ('oam_high_table', '929a', (
                0x008DBF, 0x008DD3, 0x00929D, 0x0092B2,
                0x01ADBC, 0x01ADD0, 0x01AE7E, 0x01AE92)),
            ('developed_world_map_stamp', '97a8', (0x02859F,)),
            ('deferred_ppu_register', '92ed', (0x02A96D,)),
            ('asset_workspace', '97a8', (0x0483A4,)),
            ('metatile_definitions', '97a8', (0x0483E6,)),
            ('action_map', '97a8', (0x048469,)),
            ('developed_world_map', '97a8', (0x04848D,)),
            ('character_vram_readback', '97a8', (0x048B18,)),
            ('lzss_ring', '92af', (0x04963E, 0x04966B, 0x049683)),
        ),
        'vram_descriptor_abi': 'native',
    },
}


# Verified directly against the exact Japanese 16x16 dialog-font atlas. The
# native stream writes dakuten/handakuten as following overlay tiles DE/DF;
# Decoder composes those onto these base kana below.
JAPANESE_GLYPH_MAP = {}
for start, characters in (
        (0x86, 'をぁぃぅぇぉゃゅょっ'),
        (0x91, 'あいうえおかきくけこさしすせそ'),
        (0xA6, 'ヲァィゥェォャュョッ'),
        (0xB1, 'アイウエオカキクケコサシスセソ'),
        (0xC0, 'タチツテトナニヌネノハヒフヘホマ'),
        (0xD0, 'ミムメモヤユヨラリルレロワン'),
        (0xE0, 'たちつてとなにぬねのはひふへほま'),
        (0xF0, 'みむめもやゆよらりるれろわん')):
    JAPANESE_GLYPH_MAP.update(
        {start + index: character
         for index, character in enumerate(characters)})
JAPANESE_GLYPH_MAP.update({
    # The Japanese atlas uses the Western colon/selector slots for corner
    # quotes. FE/FF are spacing marks selectable from the name-entry grid;
    # following DE/DF remain combining overlay controls in Decoder. The
    # message-speed scale retains its own verified equals/less-than tiles.
    0x1C: '<', 0x1D: '=',
    0x3A: '」', 0x3B: '「', 0x3E: '!',
    0xA1: '。', 0xA4: '、', 0xA5: '・', 0xB0: 'ー',
    0xFE: '゛', 0xFF: '゜',
})


def icon_part(value, part_index=0, part_count=1):
    """Describe one native tile belonging to a functional UI symbol."""
    return {
        'value': value,
        'part_index': part_index,
        'part_count': part_count,
    }


WESTERN_COMMON_ICON_GLYPHS = {
    # 7F visibly contains the tiny letters "BS"; 7E is the adjacent end
    # action. They are selectable name-entry functions, not name characters.
    0x7E: icon_part('name_entry.finish'),
    0x7F: icon_part('name_entry.backspace'),
    0x7B: icon_part('status.life'),
}
WESTERN_POINTER_ICON_GLYPHS = {
    0x3A: icon_part('ui.selection_pointer', 0, 2),
    0x3B: icon_part('ui.selection_pointer', 1, 2),
}
FRENCH_POINTER_ICON_GLYPHS = {
    0x5B: icon_part('ui.selection_pointer', 0, 2),
    0x5C: icon_part('ui.selection_pointer', 1, 2),
}
JAPANESE_ICON_GLYPHS = {
    0x1B: icon_part('ui.selection_pointer'),
    0x90: icon_part('name_entry.cursor'),
}

CITY_TERM_IDS = (
    'city.fillmore.name', 'city.bloodpool.name', 'city.kasandora.name',
    'city.aitos.name', 'city.marahna.name', 'city.northwall.name',
    'city.death_heim.name')
CITY_KEYS = (
    'fillmore', 'bloodpool', 'kasandora',
    'aitos', 'marahna', 'northwall')
ENEMY_TERM_IDS = (
    'enemy.blue_dragon.name', 'enemy.napper_bat.name',
    'enemy.red_demon.name', 'enemy.skull_head.name')
ACTION_HUD_IDS = (
    'action.hud.act_1', 'action.hud.act_2', None, 'action.hud.clear',
    'action.hud.ready', 'action.hud.time_up', 'action.hud.pause')

MAGIC_MENU_IDS = tuple(
    f'sky.menu.magic.{name}' for name in ('fire', 'stardust', 'aura', 'light'))
POSSESSION_MENU_IDS = tuple(
    f'sim.menu.possession.slot_{index:02d}' for index in range(20))
SKY_ROOT_MENU_IDS = (
    'sky.menu.movement', 'sky.menu.observe_people',
    'sky.menu.fight_monsters', 'sky.menu.select_magic',
    'sky.menu.status_master', 'sky.menu.status_cities',
    'sky.menu.progress_log', 'sky.menu.message_speed')
SKY_CHOICE_MENU_IDS = (
    'sky.menu.choice.movement', 'sky.menu.choice.fight',
    'sky.menu.choice.status', 'sky.menu.choice.other')
SIM_ROOT_MENU_IDS = (
    'sim.menu.return_to_palace', 'sim.menu.sky_palace_movement',
    'sim.menu.building_direction', 'sim.menu.listen',
    'sim.menu.lightning', 'sim.menu.rain', 'sim.menu.sun', 'sim.menu.wind',
    'sim.menu.earthquake', 'sim.menu.take_offering',
    'sim.menu.use_offering', 'sim.menu.status_master',
    'sim.menu.status_cities', 'sim.menu.progress_log',
    'sim.menu.message_speed')
SIM_CHOICE_MENU_IDS = (
    'sim.menu.choice.movement', 'sim.menu.choice.direct_people',
    'sim.menu.choice.miracles', 'sim.menu.choice.offerings',
    'sim.menu.choice.status', 'sim.menu.choice.other')

COMPOSER_POINTER_ROUTE_IDS = {
    'selected_magic': MAGIC_MENU_IDS,
    'selected_possession': POSSESSION_MENU_IDS,
    'city_name': CITY_TERM_IDS,
    'sky_root': SKY_ROOT_MENU_IDS,
    'sky_choice': SKY_CHOICE_MENU_IDS,
    'sim_root': SIM_ROOT_MENU_IDS,
    'sim_choice': SIM_CHOICE_MENU_IDS,
}

TITLE_ROUTE_IDS = {
    'us': (
        'title.save_choice.labels', 'title.start_prompt', 'title.copyright',
        'title.selector.continue', 'title.selector.new_game',
        'title.selector.professional'),
    'jp': (
        'title.save_choice.labels', 'title.start_prompt', 'title.copyright',
        'title.selector.continue', 'title.selector.new_game',
        'title.selector.special'),
    'regional_mode_select': (
        'title.copyright', 'title.mode_select.without_save',
        'title.mode_select.with_save', 'title.difficulty.labels',
        'title.selector.beginner', 'title.selector.normal',
        'title.selector.expert'),
}


# Exact call-flow identities for prompts owned directly by the interactive
# interpreter.  Tables and dialogue wrappers are catalogued structurally
# below.  Entries with several IDs describe branch alternatives in the same
# order as the proven immediate-Y join.
LATIN_INTERACTIVE_ROUTE_IDS = {
    0x018247: 'sim.development.direction.prompt',
    0x018251: 'sim.development.direction.cancel_confirm',
    0x018293: 'sim.miracle.lightning.description',
    0x0182AC: 'sim.miracle.lightning.confirm',
    0x0182B7: 'sim.miracle.lightning.target_prompt',
    0x0182E1: 'sim.miracle.lightning.target_cancel',
    0x0182E9: 'sim.miracle.lightning.insufficient_sp',
    0x0182FE: 'sim.miracle.rain.description',
    0x018317: 'sim.miracle.rain.confirm',
    0x018322: 'sim.miracle.rain.target_prompt',
    0x01834C: 'sim.miracle.rain.target_cancel',
    0x018354: 'sim.miracle.rain.insufficient_sp',
    0x018369: 'sim.miracle.sun.description',
    0x018382: 'sim.miracle.sun.confirm',
    0x01838D: 'sim.miracle.sun.target_prompt',
    0x0183B7: 'sim.miracle.sun.target_cancel',
    0x0183BF: 'sim.miracle.sun.insufficient_sp',
    0x0183D4: 'sim.miracle.earthquake.description',
    0x0183ED: 'sim.miracle.earthquake.confirm',
    0x018417: 'sim.miracle.earthquake.cancel',
    0x01841F: 'sim.miracle.earthquake.insufficient_sp',
    0x018434: 'sim.miracle.wind.description',
    0x01844D: 'sim.miracle.wind.confirm',
    0x018477: 'sim.miracle.wind.cancel',
    0x01847F: 'sim.miracle.wind.insufficient_sp',
    0x0184AA: 'sim.inventory.full',
    0x0184BD: 'sim.inventory.empty',
    0x0184C5: 'sim.inventory.choose_item',
    0x0184F5: 'sim.inventory.cancelled',
    0x018698: 'sky.action_mode.unavailable.no_residents',
    0x01873D: 'sky.action_mode.confirm',
    0x018748: 'sky.action_mode.begin_blessing',
    0x01876B: (
        'sky.action_mode.unavailable.master_level',
        'sky.action_mode.unavailable.already_cleared',
        'sky.action_mode.unavailable.cannot_land',
        'sky.action_mode.cancel_confirm',
        'sky.action_mode.unavailable.no_monsters'),
    0x018789: 'sky.magic.empty',
    0x018791: 'sky.magic.choose',
    0x0187E6: (
        'sky.magic.cancelled', 'sky.magic.selected.fire',
        'sky.magic.selected.stardust', 'sky.magic.selected.aura',
        'sky.magic.selected.light'),
    0x01886C: 'sim.town.gratitude',
    0x0188C6: 'sim.offerings.choose',
    0x018971: 'sim.offerings.choose_more',
    0x01898C: (
        'sim.offerings.unavailable.no_residents',
        'sim.offerings.unavailable.none', 'sim.offerings.completed',
        'sim.offerings.cancelled', 'sim.offerings.unavailable.inventory_full'),
    0x018A9D: 'system.save.confirm',
    0x018ABB: 'system.save.completed_continue_confirm',
    0x018ACA: 'system.save.rest_message',
    0x018AEE: ('system.save.continue', 'system.save.cancelled'),
    0x018AF8: 'system.message_speed.choose',
    0x018B67: 'system.message_speed.cancelled',
    0x018B74: 'system.message_speed.sample',
    0x018C2F: 'variant.western.sim.command_menu.prompt',
    0x018C3B: 'variant.western.sim.command_menu.prompt_continuation',
    0x018737: (
        'sky.action_mode.outcome.final_battle',
        'sky.action_mode.outcome.first_lair',
        'sky.action_mode.outcome.repeat_lair'),
}

JAPANESE_INTERACTIVE_ROUTE_IDS = {
    0x018227: 'sim.development.direction.prompt',
    0x01825A: 'sim.miracle.lightning.description',
    0x018273: 'sim.miracle.lightning.confirm',
    0x01827E: 'sim.miracle.lightning.target_prompt',
    0x0182A8: 'sim.miracle.lightning.target_cancel',
    0x0182B0: 'sim.miracle.lightning.insufficient_sp',
    0x0182C5: 'sim.miracle.rain.description',
    0x0182DE: 'sim.miracle.rain.confirm',
    0x0182E9: 'sim.miracle.rain.target_prompt',
    0x018313: 'sim.miracle.rain.target_cancel',
    0x01831B: 'sim.miracle.rain.insufficient_sp',
    0x018330: 'sim.miracle.sun.description',
    0x018349: 'sim.miracle.sun.confirm',
    0x018354: 'sim.miracle.sun.target_prompt',
    0x01837E: 'sim.miracle.sun.target_cancel',
    0x018386: 'sim.miracle.sun.insufficient_sp',
    0x01839B: 'sim.miracle.earthquake.description',
    0x0183B4: 'sim.miracle.earthquake.confirm',
    0x0183DE: 'sim.miracle.earthquake.cancel',
    0x0183E6: 'sim.miracle.earthquake.insufficient_sp',
    0x0183FB: 'sim.miracle.wind.description',
    0x018414: 'sim.miracle.wind.confirm',
    0x01843E: 'sim.miracle.wind.cancel',
    0x018446: 'sim.miracle.wind.insufficient_sp',
    0x018471: 'sim.inventory.full',
    0x018484: 'sim.inventory.empty',
    0x01848C: 'sim.inventory.choose_item',
    0x0184BC: 'sim.inventory.cancelled',
    0x01864D: 'sky.action_mode.unavailable.no_residents',
    0x0186F2: 'sky.action_mode.confirm',
    0x0186FD: 'sky.action_mode.begin_blessing',
    0x018720: (
        'sky.action_mode.unavailable.master_level',
        'sky.action_mode.unavailable.already_cleared',
        'sky.action_mode.unavailable.cannot_land',
        'sky.action_mode.cancel_confirm',
        'sky.action_mode.unavailable.no_monsters'),
    0x01873E: 'sky.magic.empty',
    0x018746: 'sky.magic.choose',
    0x01879B: (
        'sky.magic.cancelled', 'sky.magic.selected.fire',
        'sky.magic.selected.stardust', 'sky.magic.selected.aura',
        'sky.magic.selected.light'),
    0x018821: 'variant.jp.sim.town.no_buildable_space',
    0x01882E: 'variant.jp.sim.town.needs_development_direction',
    0x01883B: 'variant.jp.sim.town.dry_fields',
    0x018848: 'variant.jp.sim.town.low_productivity',
    0x018850: 'sim.town.gratitude',
    0x0188AF: 'sim.offerings.choose',
    0x01894E: 'sim.offerings.choose_more',
    0x018969: (
        'sim.offerings.unavailable.no_residents',
        'sim.offerings.unavailable.none', 'sim.offerings.completed',
        'sim.offerings.cancelled', 'sim.offerings.unavailable.inventory_full'),
    0x018A38: 'system.save.confirm',
    0x018A5A: 'system.save.completed_continue_confirm',
    0x018A69: 'system.save.rest_message',
    0x018A85: ('system.save.continue', 'system.save.cancelled'),
    0x018A8F: 'system.message_speed.choose',
    0x018AFE: 'system.message_speed.cancelled',
    0x018B0B: 'system.message_speed.sample',
    0x0186EC: (
        'sky.action_mode.outcome.final_battle',
        'sky.action_mode.outcome.first_lair',
        'sky.action_mode.outcome.repeat_lair'),
}

REQUIRED_LEVEL_TERM_IDS = tuple(
    f'required_master_level.{value}' for value in (1, 2, 4, 6, 8, 10))
GROWTH_STATE_TERM_IDS = tuple(
    f'simulation.growth_state.{state}'
    for state in ('none', 'stopped', 'slow', 'normal', 'fast', 'maximum'))


def native_address(address):
    return f'${address:04X}'


def number_semantics(total_population, current_city_population,
                     city_population, city_growth, city_level, city_items,
                     master_sp, master_max_sp, master_level, master_hp,
                     master_magic_points, next_level_population,
                     score_start=None, include_lives=False):
    """Build the typed value ABI for one exact retail WRAM layout."""
    result = {
        '$0006': 'lair_count',
        '$0010': 'sound_music_id',
        '$0012': 'sound_effect_id',
        native_address(total_population): 'total_population',
        native_address(current_city_population): 'current_city_population',
        native_address(master_sp): 'master_sp',
        native_address(master_max_sp): 'master_max_sp',
        native_address(master_level): 'master_level',
        native_address(master_hp): 'master_hp',
        native_address(master_magic_points): 'master_magic_points',
        native_address(next_level_population): 'next_level_population',
    }
    for index, city in enumerate(CITY_KEYS):
        result[native_address(city_population + index * 2)] = \
            f'city_{city}_population'
        result[native_address(city_growth + index)] = \
            f'city_{city}_growth_state'
        result[native_address(city_level + index * 2)] = \
            f'city_{city}_level'
        result[native_address(city_items + index * 2)] = \
            f'city_{city}_items'
    if include_lives:
        # The report caller copies $02AB + 1 into this scratch word before
        # invoking the composer.  The semantic value is the displayed count,
        # not a stable address-backed field.
        result['$0002'] = 'master_lives_display'
    if score_start is not None:
        # The caller accumulates the packed-BCD stage values into $0000 before
        # rendering the report. Width $86/$84 retains the native BCD format.
        result['$0000'] = 'total_score'
        for index, city in enumerate(CITY_KEYS):
            for act in range(2):
                address = score_start + (index * 2 + act) * 2
                result[native_address(address)] = \
                    f'score_{city}_act_{act + 1}'
    return result


def growth_lookup_semantics(index_start, pointer_table):
    return {
        f'{index_start + index & 0xFF:02X} '
        f'{(index_start + index) >> 8:02X} '
        f'{pointer_table & 0xFF:02X} {pointer_table >> 8:02X}':
            f'city_{city}_growth_state'
        for index, city in enumerate(CITY_KEYS)
    }


ROM_PROFILES = {
    'b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0': {
        'id': 'us',
        # $01:8F4B emits this ROM prefix, then the name and a space tile.
        # The fixed composer inserts the plain name instead.
        'dialogue_name_prefix': 0x0F048,
        'dialogue_name_separator': ' ',
        'locale': 'en-US',
        'label': 'USA English',
        'encoding': 'dictionary-12',
        'dictionary': 0x258F3,
        'angel_start': 0x20077,
        'angel_end': 0x21397,
        'handler_table': 0x21397,
        'town_start': 0x21532,
        'offering_table': 0x246AE,
        'ending_table': 0x24C8A,
        'action_stage_name_table': 0x02843,
        'action_label_start': 0x028CB,
        'action_label_end': 0x028F6,
        'title_text_start': 0x129A7,
        'title_text_end': 0x12A76,
        'sound_test_start': 0x11871,
        'sound_test_end': 0x118B7,
        'menu_start': 0x0EF3B,
        'menu_end': 0x0FF98,
        'lookup_source_tables': (
            ('required_master_level', 0x0EFE8, REQUIRED_LEVEL_TERM_IDS),
            ('growth_state', 0x0F66F, GROWTH_STATE_TERM_IDS)),
        'indexed_text_semantics': {
            '02 00 43 80': 'enemy_name',
            '41 03 00 80': 'town_name',
            '02 00 E8 EF': 'required_master_level',
            '3F 03 BD F1': 'current_city_name',
            **growth_lookup_semantics(0x0228, 0xF66F),
        },
        'number_semantics': number_semantics(
            0x0218, 0x021A, 0x021C, 0x0228, 0x022E, 0x023A,
            0x0282, 0x0284, 0x0291, 0x0293, 0x0295, 0x0297,
            score_start=0x02B3, include_lives=True),
        'glyph_overrides': {0x7D: '×'},
        'icon_glyphs': {
            **WESTERN_COMMON_ICON_GLYPHS,
            **WESTERN_POINTER_ICON_GLYPHS,
        },
    },
    '146a68436fa9dbe728ddc7355821384765325e356cb8b9b193a4f22333ed52a0': {
        'id': 'eu-en',
        'dialogue_name_prefix': 0x0F048,
        'dialogue_name_separator': ' ',
        'locale': 'en-GB',
        'label': 'Europe English',
        'encoding': 'dictionary-12',
        'dictionary': 0x258F1,
        'angel_start': 0x20077,
        'angel_end': 0x21397,
        'handler_table': 0x21397,
        'town_start': 0x21532,
        'offering_table': 0x246AE,
        'ending_table': 0x24C8A,
        'action_stage_name_table': 0x02418,
        'action_label_start': 0x024A0,
        'action_label_end': 0x024CB,
        'title_text_start': 0x12A1D,
        'title_text_end': 0x12B03,
        'sound_test_start': 0x11871,
        'sound_test_end': 0x118B7,
        'menu_start': 0x0EF3B,
        'menu_end': 0x0FF95,
        'lookup_source_tables': (
            ('required_master_level', 0x0EFE8, REQUIRED_LEVEL_TERM_IDS),
            ('growth_state', 0x0F677, GROWTH_STATE_TERM_IDS)),
        'indexed_text_semantics': {
            '02 00 43 80': 'enemy_name',
            '43 03 00 80': 'town_name',
            '02 00 E8 EF': 'required_master_level',
            '41 03 C5 F1': 'current_city_name',
            **growth_lookup_semantics(0x022A, 0xF677),
        },
        'number_semantics': number_semantics(
            0x021A, 0x021C, 0x021E, 0x022A, 0x0230, 0x023C,
            0x0284, 0x0286, 0x0293, 0x0295, 0x0297, 0x0299,
            score_start=0x02B5, include_lives=True),
        'glyph_overrides': {0x7D: '×'},
        'icon_glyphs': {
            **WESTERN_COMMON_ICON_GLYPHS,
            **WESTERN_POINTER_ICON_GLYPHS,
        },
    },
    '01923db83e0e8b19d476483649d956e3e24cfc918ca04a6aa04faa29ba8e4c41': {
        'id': 'de',
        'dialogue_name_prefix': 0x0F062,
        'locale': 'de-DE',
        'label': 'Europe German',
        'encoding': 'dictionary-12',
        'dictionary': 0x25A75,
        'angel_start': 0x20077,
        'handler_table': 0x213A6,
        'town_start': 0x21532,
        'offering_table': 0x247B0,
        'ending_table': 0x24DBC,
        'action_stage_name_table': 0x02418,
        'action_label_start': 0x024A0,
        'action_label_end': 0x024CD,
        'title_text_start': 0x12A1D,
        'title_text_end': 0x12B0C,
        'sound_test_start': 0x11871,
        'sound_test_end': 0x118B7,
        'menu_start': 0x0EF3B,
        'menu_end': 0x0FFF2,
        'lookup_source_tables': (
            ('required_master_level', 0x0EFEE, REQUIRED_LEVEL_TERM_IDS),
            ('growth_state', 0x0F689, GROWTH_STATE_TERM_IDS)),
        'indexed_text_semantics': {
            '02 00 43 80': 'enemy_name',
            '43 03 00 80': 'town_name',
            '02 00 EE EF': 'required_master_level',
            '41 03 10 F2': 'current_city_name',
            **growth_lookup_semantics(0x022A, 0xF689),
        },
        'number_semantics': number_semantics(
            0x021A, 0x021C, 0x021E, 0x022A, 0x0230, 0x023C,
            0x0284, 0x0286, 0x0293, 0x0295, 0x0297, 0x0299,
            score_start=0x02B5, include_lives=True),
        'glyph_overrides': {0x5B: 'ü', 0x5C: 'ä', 0x5D: 'ö',
                            0x5E: 'ß', 0x7D: '×'},
        'icon_glyphs': {
            **WESTERN_COMMON_ICON_GLYPHS,
            **WESTERN_POINTER_ICON_GLYPHS,
        },
    },
    '6cc2cadfcb4fba4c1abb2a1d06b49b840bec65d75acaa0ac8831576442e7e96a': {
        'id': 'fr',
        'dialogue_name_prefix': 0x0F043,
        'locale': 'fr-FR',
        'label': 'Europe French',
        'encoding': 'dictionary-12',
        'dictionary': 0x25989,
        'angel_start': 0x20077,
        'handler_table': 0x2146F,
        'town_start': 0x215FB,
        'offering_table': 0x246C4,
        'ending_table': 0x24D53,
        'action_stage_name_table': 0x02418,
        'action_label_start': 0x024A0,
        'action_label_end': 0x024D0,
        'title_text_start': 0x12A07,
        'title_text_end': 0x12AF5,
        'sound_test_start': 0x1185B,
        'sound_test_end': 0x118A1,
        'menu_start': 0x0EF3B,
        'menu_end': 0x0FFB8,
        'lookup_source_tables': (
            ('required_master_level', 0x0EFEB, REQUIRED_LEVEL_TERM_IDS),
            ('growth_state', 0x0F6C0, GROWTH_STATE_TERM_IDS)),
        'indexed_text_semantics': {
            '02 00 43 80': 'enemy_name',
            '43 03 00 80': 'town_name',
            '02 00 EB EF': 'required_master_level',
            '41 03 D1 F1': 'current_city_name',
            **growth_lookup_semantics(0x022A, 0xF6C0),
        },
        'number_semantics': number_semantics(
            0x021A, 0x021C, 0x021E, 0x022A, 0x0230, 0x023C,
            0x0284, 0x0286, 0x0293, 0x0295, 0x0297, 0x0299,
            score_start=0x02B5, include_lives=True),
        # The retail French script uses this slot as an apostrophe. Unlike
        # the other Western atlases, its two-tile selection pointer moved to
        # 5B/5C; 3A remains an ordinary colon.
        'glyph_overrides': {0x60: "'", 0x7D: '×'},
        'icon_glyphs': {
            **WESTERN_COMMON_ICON_GLYPHS,
            **FRENCH_POINTER_ICON_GLYPHS,
        },
    },
    '3655833fd0fb4985c3dbbf28141f65564f7a228ee92d81a12982ef7cb952a51b': {
        'id': 'jp',
        'locale': 'ja-JP',
        'label': 'Japan',
        'encoding': 'direct-glyph',
        'town_name_table': 0x12CF1,
        'enemy_name_table': 0x12D31,
        'angel_start': 0x12D61,
        'handler_table': 0x1321B,
        'town_start': 0x133A9,
        'offering_table': 0x16066,
        'post_text_end': 0x176E8,
        'action_stage_name_table': 0x02806,
        'action_label_start': 0x0288B,
        'action_label_end': 0x028B5,
        'title_text_start': 0x1271D,
        'title_text_end': 0x127BC,
        'sound_test_start': 0x11604,
        'sound_test_end': 0x1164A,
        # Japanese name entry includes two direct-glyph kana grids before the
        # later shared menu/status catalogue.
        'menu_start': 0x0ED34,
        'menu_end': 0x0FFB0,
        'lookup_source_tables': (
            ('required_master_level', 0x0EEC4, REQUIRED_LEVEL_TERM_IDS),
            ('growth_state', 0x0F4EE, GROWTH_STATE_TERM_IDS)),
        'indexed_text_semantics': {
            '02 00 31 AD': 'enemy_name',
            '2F 03 F1 AC': 'town_name',
            '02 00 C4 EE': 'required_master_level',
            '2D 03 59 F0': 'current_city_name',
            '2C 03 5F F1': 'selected_offering_action',
            **growth_lookup_semantics(0x0227, 0xF4EE),
        },
        'number_semantics': number_semantics(
            0x0217, 0x0219, 0x021B, 0x0227, 0x022D, 0x0239,
            0x0281, 0x0283, 0x0290, 0x0292, 0x0294, 0x0296),
        'glyph_overrides': JAPANESE_GLYPH_MAP,
        'icon_glyphs': JAPANESE_ICON_GLYPHS,
    },
}

DEFAULT_ROMS = ('ar.sfc', 'ar-eu.sfc', 'ar-ger.sfc', 'ar-fra.sfc',
                'ar-jp.sfc')
ROM_SIZE = 0x100000
DICTIONARY_ENTRY_BYTES = 12
DICTIONARY_ENTRY_COUNT = 128

# Reader entries, not the top-level dialogue/composer entries. The dictionary
# storage is shared, but the two consumers have different termination rules.
DICTIONARY_CONSUMER_PROFILES = {
    'us': (0x018FC5, 0x02C0DF, 0xF1, False),
    'eu-en': (0x018FC5, 0x02C6F8, 0xF2, False),
    'de': (0x018FBD, 0x02C701, 0xF2, True),
    'fr': (0x018FBD, 0x02C6EA, 0xF2, True),
}
OFFERING_POINTER_COUNT = 21
ENDING_POINTER_COUNT = 8
HANDLER_CITY_COUNT = 6
HANDLER_SLOTS_PER_CITY = 32

# Argument counts are verified from the localized bank-$01 interpreter.
# Reachable 08 arguments are promoted through each exact-ROM profile; unknown
# 08 arguments and the currently unreachable two-level 0A lookup remain inert
# until their callers and lookup tables are catalogued.
CONTROL_ARGUMENTS = {0x07: 1, 0x08: 4, 0x09: 3, 0x0A: 6, 0x0B: 1}


def internal_title(rom):
    return bytes(rom[0x7FC0:0x7FD5]).rstrip(b' \0').decode(
        'ascii', errors='replace')


def u16(data, offset):
    return data[offset] | data[offset + 1] << 8


def offset_to_snes(offset):
    return offset // 0x8000, 0x8000 + offset % 0x8000


def snes_string(offset):
    bank, address = offset_to_snes(offset)
    return f'${bank:02X}:{address:04X}'


def pc24_to_offset(address):
    """Translate a canonical low-bank LoROM address to a file offset."""
    bank = address >> 16
    local = address & 0xFFFF
    if bank > 0x7F or local < 0x8000:
        raise ValueError(f'unsupported LoROM PC24 address ${address:06X}')
    return bank * 0x8000 + (local & 0x7FFF)


def offset_to_pc24(offset):
    return (offset // 0x8000) << 16 | 0x8000 | (offset & 0x7FFF)


def pc24_string(address):
    return f'${address >> 16:02X}:{address & 0xFFFF:04X}'


def scan_pattern_pc24(rom, pattern):
    matches = []
    position = 0
    while True:
        position = rom.find(pattern, position)
        if position < 0:
            return matches
        matches.append(offset_to_pc24(position))
        position += 1


def routine_call_pattern(entry_pc24, opcode):
    local = entry_pc24 & 0xFFFF
    if opcode == 'jsr':
        return bytes((0x20, local & 0xFF, local >> 8))
    if opcode == 'jsl':
        return bytes((0x22, local & 0xFF, local >> 8,
                      (entry_pc24 >> 16) & 0xFF))
    raise ValueError(f'unsupported call opcode {opcode!r}')


def assign_ordered_groups(call_sites, layout):
    expected = sum(count for _, count in layout)
    if len(call_sites) != expected:
        raise ValueError(
            f'composer call-site count changed: expected {expected}, '
            f'found {len(call_sites)}')
    groups = []
    cursor = 0
    for name, count in layout:
        groups.extend([name] * count)
        cursor += count
    assert cursor == len(call_sites)
    return groups


def direct_y_source(rom, call_site):
    """Return an immediate Y operand only for the unambiguous adjacent form."""
    offset = pc24_to_offset(call_site)
    if offset >= 3 and rom[offset - 3] == 0xA0:  # LDY #imm16; JSR
        return rom[offset - 2] | rom[offset - 1] << 8
    return None


def relative_branch_target(rom, branch_offset):
    opcode = rom[branch_offset]
    if opcode in (0x10, 0x30, 0x50, 0x70, 0x80,
                  0x90, 0xB0, 0xD0, 0xF0):
        displacement = rom[branch_offset + 1]
        if displacement >= 0x80:
            displacement -= 0x100
        return branch_offset + 2 + displacement
    if opcode == 0x82:
        displacement = u16(rom, branch_offset + 1)
        if displacement >= 0x8000:
            displacement -= 0x10000
        return branch_offset + 3 + displacement
    return None


def incoming_immediate_y_branches(rom, target_pc24):
    """Find exact branch-to-call shapes whose path loads an immediate Y.

    The supported code uses either ``LDY #value; branch`` or
    ``LDY #value; DEC A; conditional branch``. Requiring those complete byte
    shapes filters branch-looking operand data while retaining both short and
    long joins.
    """
    target_offset = pc24_to_offset(target_pc24)
    bank_start = target_offset // 0x8000 * 0x8000
    bank_end = min(bank_start + 0x8000, len(rom))
    sources = []
    for branch_offset in range(bank_start, bank_end - 2):
        if relative_branch_target(rom, branch_offset) != target_offset:
            continue
        source_y = None
        shape = None
        if branch_offset >= bank_start + 3 and \
                rom[branch_offset - 3] == 0xA0:
            source_y = u16(rom, branch_offset - 2)
            shape = 'ldy_immediate_then_branch'
        elif branch_offset >= bank_start + 4 and \
                rom[branch_offset - 4] == 0xA0 and \
                rom[branch_offset - 1] == 0x3A:
            source_y = u16(rom, branch_offset - 3)
            shape = 'ldy_immediate_dec_a_then_branch'
        if source_y is not None:
            sources.append({
                'branch_site': offset_to_pc24(branch_offset),
                'source_y': source_y,
                'shape': shape,
            })
    return sources


def conditional_immediate_y_sources(rom, call_site):
    """Resolve the exact three-way conditional source join before a call."""
    call_offset = pc24_to_offset(call_site)
    if call_offset < 21 or rom[call_offset - 1] != 0x68:
        raise ValueError(
            f'conditional source join changed at {pc24_string(call_site)}')
    source_offsets = (call_offset - 21, call_offset - 11, call_offset - 4)
    if any(rom[offset] != 0xA0 for offset in source_offsets):
        raise ValueError(
            f'conditional source loads changed at {pc24_string(call_site)}')
    for branch_offset in (call_offset - 13, call_offset - 6):
        if rom[branch_offset] != 0xF0 or \
                relative_branch_target(rom, branch_offset) != call_offset - 1:
            raise ValueError(
                f'conditional source branch changed at '
                f'{pc24_string(call_site)}')
    return [u16(rom, offset + 1) for offset in source_offsets]


def classify_outside_bg3_write(rom, site_pc24, role):
    """Validate and classify one direct base-buffer maintenance loop."""
    offset = pc24_to_offset(site_pc24)
    loop_tail = rom[offset + 4:offset + 11]
    row = {
        'write_site': pc24_string(site_pc24),
        'role': role,
        'classification': 'verified_non_language_maintenance',
        'language_bearing_candidate': False,
    }
    if role == 'status_strip_clear':
        if rom[offset - 12:offset - 9] != bytes.fromhex('a9 00 00') or \
                rom[offset - 3:offset] != bytes.fromhex('a2 c0 00') or \
                loop_tail != bytes.fromhex('e8 e8 e0 00 01 d0 f5'):
            raise ValueError(
                f'status-strip clear changed at {pc24_string(site_pc24)}')
        row['destination_range'] = '$7F:B0C0-$7F:B0FF'
        row['value'] = '$0000'
    elif role == 'status_strip_sequential_tiles':
        if rom[offset - 6:offset - 3] != bytes.fromhex('a2 c0 00') or \
                rom[offset - 3] != 0xA9 or \
                rom[offset + 4:offset + 12] != bytes.fromhex(
                    '1a e8 e8 e0 cc 00 d0 f4'):
            raise ValueError(
                f'status tile strip changed at {pc24_string(site_pc24)}')
        row['classification'] = 'classified_graphical_text'
        row['coverage_class'] = 'graphical_text'
        row['semantic_id'] = 'action.hud.enemy_label'
        row['language_bearing_candidate'] = False
        row['destination_range'] = '$7F:B0C0-$7F:B0CB'
        row['tile_word_start'] = f'${u16(rom, offset - 2):04X}'
        row['tile_count'] = 6
    elif role == 'dialogue_surface_clear':
        if rom[offset - 6:offset] != bytes.fromhex(
                'a9 00 20 a2 00 01') or \
                loop_tail != bytes.fromhex('e8 e8 e0 00 08 d0 f5'):
            raise ValueError(
                f'dialogue clear changed at {pc24_string(site_pc24)}')
        row['destination_range'] = '$7F:B100-$7F:B7FF'
        row['value'] = '$2000'
    elif role in ('general_surface_clear', 'city_pause_surface_clear'):
        if rom[offset - 6:offset] != bytes.fromhex(
                'a9 00 20 a2 00 00') or \
                loop_tail != bytes.fromhex('e8 e8 e0 00 07 d0 f5'):
            raise ValueError(
                f'{role} changed at {pc24_string(site_pc24)}')
        row['destination_range'] = '$7F:B000-$7F:B6FF'
        row['value'] = '$2000'
    else:
        raise ValueError(f'unknown outside BG3 write role {role!r}')
    return row


def classify_bg3_hud_template_copy(rom, site_pc24, role, census_profile):
    """Resolve one 32x2 HUD template and its pre-rendered text regions."""
    if role not in BG3_HUD_GRAPHICAL_TEXT_REGIONS:
        raise ValueError(f'unknown BG3 HUD template role {role!r}')

    offset = pc24_to_offset(site_pc24)
    if rom[offset - 7] != 0xBF or \
            rom[offset - 3:offset] != bytes.fromhex('09 00 20') or \
            rom[offset:offset + 4] != bytes.fromhex('9f 40 b0 7f') or \
            rom[offset + 4:offset + 11] != bytes.fromhex(
                'e8 e8 e0 80 00 d0 ee'):
        raise ValueError(
            f'{role} copy changed at {pc24_string(site_pc24)}')

    source_pc24 = (rom[offset - 4] << 16 |
                   rom[offset - 5] << 8 |
                   rom[offset - 6])
    expected_sources = dict(census_profile.get(
        'bg3_hud_template_sources', ()))
    expected_source = expected_sources.get(role)
    if expected_source is not None and source_pc24 != expected_source:
        raise ValueError(
            f'{role} source changed at {pc24_string(site_pc24)}: '
            f'expected {pc24_string(expected_source)}, '
            f'found {pc24_string(source_pc24)}')

    source_offset = pc24_to_offset(source_pc24)
    raw = rom[source_offset:source_offset + 0x80]
    if len(raw) != 0x80:
        raise ValueError(
            f'{role} source is truncated at {pc24_string(source_pc24)}')
    words = [u16(raw, index) for index in range(0, len(raw), 2)]

    regions = []
    for semantic_id, configured_indices in \
            BG3_HUD_GRAPHICAL_TEXT_REGIONS[role]:
        indices = configured_indices
        if semantic_id == 'sim_sky.hud.context_label':
            count = census_profile.get('sim_sky_context_label_word_count')
            if count is None:
                raise ValueError('sim/Sky HUD context-label width is unprofiled')
            indices = tuple(range(count))
        # A localized word may deliberately leave one cell blank for
        # centering (French's shorter angel label does this), so validate the
        # region as a non-empty run rather than requiring every cell inked.
        if not indices or not any((words[index] & 0x03FF) != 0
                                  for index in indices):
            raise ValueError(
                f'{semantic_id} graphical region changed at '
                f'{pc24_string(source_pc24)}')
        regions.append({
            'semantic_id': semantic_id,
            'classification': 'graphical_text',
            'word_indices': list(indices),
            'font_tile_ids': [f'${words[index] & 0x03FF:03X}'
                              for index in indices],
        })

    classified_indices = {
        index for region in regions for index in region['word_indices']
    }
    return {
        'classification': 'classified_graphical_text_template',
        'coverage_class': 'graphical_text',
        'language_bearing_candidate': False,
        'source_tilemap': {
            'pc24': pc24_string(source_pc24),
            'byte_count': len(raw),
            'raw_sha256': hashlib.sha256(raw).hexdigest(),
            'dimensions_cells': [32, 2],
        },
        'destination_range': '$7F:B040-$7F:B0BF',
        'graphical_text_regions': regions,
        'graphical_text_region_count': len(regions),
        'graphical_text_word_count': len(classified_indices),
        'other_template_word_count': len(words) - len(classified_indices),
        'other_words_classification': (
            'dynamic_placeholder_or_non_language_decoration'),
    }


def scan_direct_long_bg3_writes(rom):
    """Find raw STA long/long-X encodings into the BG3 staging buffer."""
    rows = []
    for offset in range(len(rom) - 3):
        opcode = rom[offset]
        destination = (rom[offset + 3] << 16 |
                       rom[offset + 2] << 8 |
                       rom[offset + 1])
        if opcode not in (0x8F, 0x9F) or \
                not 0x7FB000 <= destination <= 0x7FBFFF:
            continue
        rows.append({
            'site_pc24_value': offset_to_pc24(offset),
            'write_site': pc24_string(offset_to_pc24(offset)),
            'opcode': 'sta_long' if opcode == 0x8F else 'sta_long_x',
            'destination': pc24_string(destination),
        })
    return rows


def asset_command_high_bit(value):
    """Return the command selector used by the asset-script dispatcher."""
    for bit in range(7, -1, -1):
        if value & (1 << bit):
            return bit
    return None


def iter_asset_script(rom):
    """Yield the exact per-scene asset commands rooted at ROM $05:8000."""
    cursor = ASSET_SCRIPT_BASE + ASSET_SCRIPT_HEADER_BYTES
    limit = ASSET_SCRIPT_BASE + 0x7FF0
    while cursor < limit:
        mode, submode = rom[cursor:cursor + 2]
        cursor += 2
        commands = []
        while cursor < limit:
            command = rom[cursor]
            cursor += 1
            if command == 0:
                break
            selector = asset_command_high_bit(command)
            operand_count = ASSET_COMMAND_OPERAND_BYTES[selector]
            operands = rom[cursor:cursor + operand_count]
            if len(operands) != operand_count:
                raise ValueError('asset script ended inside command operands')
            cursor += operand_count
            commands.append((command, operands))
        else:
            raise ValueError('asset script entry has no terminator')
        yield mode, submode, commands
        if (mode, submode) == ASSET_SCRIPT_END:
            return
    raise ValueError('asset script end marker was not found')


def serialize_asset_script_entry(mode, submode, commands):
    """Serialize one decoded asset entry for a stable local fingerprint."""
    raw = bytearray((mode, submode))
    for command, operands in commands:
        raw.append(command)
        raw.extend(operands)
    raw.append(0)
    return bytes(raw)


def build_title_graphical_text_surface(rom):
    """Fingerprint the title scene without extracting its retail artwork."""
    matches = [
        commands for mode, submode, commands in iter_asset_script(rom)
        if (mode, submode) == (0, 0)
    ]
    if len(matches) != 1:
        raise ValueError(
            f'expected one title asset-script entry, found {len(matches)}')
    raw = serialize_asset_script_entry(0, 0, matches[0])
    entry_offset = ASSET_SCRIPT_BASE + ASSET_SCRIPT_HEADER_BYTES
    if rom[entry_offset:entry_offset + len(raw)] != raw:
        raise ValueError('title asset-script entry is no longer first')
    return {
        'id': 'title.logo_and_publisher',
        'classification': 'graphical_text_full_surface',
        'replacement_path': 'future_graphics_surface_replacement',
        'runtime_selector': {'mode_18': '$00', 'submode_19': '$00'},
        'surface': 'mode7_bg1',
        'settled_capture_rect_xyxy': list(TITLE_GRAPHICAL_TEXT_RECT),
        'structured_menu_text_exclusion': 'screen_y_greater_than_or_equal_140',
        'asset_scene': {
            'source_file_offset': f'0x{entry_offset:06X}',
            'byte_count': len(raw),
            'command_count': len(matches[0]),
            'raw_sha256': hashlib.sha256(raw).hexdigest(),
        },
        'source_asset_status': (
            'scene_fingerprinted_individual_art_blob_not_claimed'),
        'redistribution': 'fingerprint_only_rom_art_not_redistributable',
    }


def find_all(data, pattern, start=0, end=None):
    """Return every overlapping occurrence of a non-empty byte pattern."""
    if not pattern:
        raise ValueError('cannot scan for an empty pattern')
    if end is None:
        end = len(data)
    matches = []
    cursor = start
    while cursor < end:
        match = data.find(pattern, cursor, end)
        if match < 0:
            break
        matches.append(match)
        cursor = match + 1
    return matches


def build_ending_graphical_text_surface(rom):
    """Prove the ending/credits graphical-page presentation ABI."""
    signature_sites = find_all(rom, ENDING_PAGE_COPY_SIGNATURE)
    if len(signature_sites) != 1:
        raise ValueError(
            'expected one ending page-copy signature, found '
            f'{len(signature_sites)}')
    signature_offset = signature_sites[0]
    routine_offset = signature_offset - 20
    if routine_offset < 0 or rom[routine_offset:routine_offset + 5] != \
            bytes.fromhex('48 b0 0e a9 0f'):
        raise ValueError('ending page-copy entry shape changed')

    routine_pc24 = offset_to_pc24(routine_offset)
    local = routine_pc24 & 0xFFFF
    call_pattern = bytes((0x20, local & 0xFF, local >> 8))
    bank_start = routine_offset // 0x8000 * 0x8000
    bank_end = min(bank_start + 0x8000, len(rom))
    call_offsets = find_all(rom, call_pattern, bank_start, bank_end)
    if len(call_offsets) != 5:
        raise ValueError(
            'ending page-copy caller count changed: expected 5, found '
            f'{len(call_offsets)}')

    immediate_calls = []
    for call_offset in call_offsets:
        if call_offset >= 3 and rom[call_offset - 3] == 0xA9 and \
                rom[call_offset - 1] in (0x18, 0x38):
            immediate_calls.append({
                'call_site': pc24_string(offset_to_pc24(call_offset)),
                'page_index': rom[call_offset - 2],
                'carry': ('clear' if rom[call_offset - 1] == 0x18
                          else 'set'),
            })
    immediate_indices = [row['page_index'] for row in immediate_calls]
    if len(immediate_calls) != 4 or immediate_indices[0] != 0 or \
            immediate_indices[-2:] != [18, 19]:
        raise ValueError(
            f'ending immediate page selectors changed: {immediate_indices}')

    second_call = call_offsets[1]
    loop_tail = rom[second_call + 3:second_call + 7]
    if len(loop_tail) != 4 or loop_tail[0:2] != bytes.fromhex('1a c9') or \
            loop_tail[3] != 0x90 or loop_tail[2] not in (0x10, 0x11):
        raise ValueError('ending sequential-page loop bound changed')
    sequential_stop = loop_tail[2]
    active_indices = sorted(
        set(range(sequential_stop)) | set(immediate_indices))
    maximum_page_index = max(active_indices)
    addressable_page_count = maximum_page_index + 1
    source_end = 0x4000 + addressable_page_count * ENDING_PAGE_BYTES
    dormant_indices = sorted(
        set(range(addressable_page_count)) - set(active_indices))

    return {
        'id': 'ending.credits.graphical_pages',
        'classification': 'graphical_text_paged_surface',
        'replacement_path': 'future_graphics_surface_replacement',
        'runtime_selector': {'mode_18': '$08'},
        'copy_routine': {
            'entry_pc24': pc24_string(routine_pc24),
            'mvn_site_pc24': pc24_string(offset_to_pc24(
                signature_offset + ENDING_PAGE_COPY_SIGNATURE.index(
                    bytes.fromhex('54 7f 7e')))),
            'signature_sha256': hashlib.sha256(
                ENDING_PAGE_COPY_SIGNATURE).hexdigest(),
            'caller_count': len(call_offsets),
            'call_sites': [pc24_string(offset_to_pc24(offset))
                           for offset in call_offsets],
        },
        'page_abi': {
            'page_byte_count': ENDING_PAGE_BYTES,
            'sequential_page_stop_exclusive': sequential_stop,
            'immediate_calls': immediate_calls,
            'active_page_indices': active_indices,
            'dormant_page_indices': dormant_indices,
            'addressable_page_count': addressable_page_count,
            'source_wram_range': f'$7E:4000-$7E:{source_end - 1:04X}',
            'destination_wram_range': '$7F:B000-$7F:B7FF',
        },
        'source_asset_status': (
            'runtime_prepared_wram_original_rom_producer_not_claimed'),
        'redistribution': 'structural_provenance_only_no_retail_pages',
    }


def build_dialog_font_census(rom):
    """Resolve and verify the regional font named by the asset script."""
    references = []
    for mode, submode, commands in iter_asset_script(rom):
        for command, operands in commands:
            if asset_command_high_bit(command) != 7 or \
                    operands[:3] != DIALOG_FONT_COMMAND_PREFIX:
                continue
            source = (operands[3] | operands[4] << 8 |
                      operands[5] << 16)
            references.append({
                'mode': f'${mode:02X}',
                'submode': f'${submode:02X}',
                'source_file_offset': f'0x{source:06X}',
            })
    sources = sorted({row['source_file_offset'] for row in references})
    if len(sources) != 1:
        raise ValueError(
            'expected exactly one regional dialog-font source, found '
            f'{sources}')
    source = int(sources[0], 16)
    decoded, compressed_bytes = quintet_decompress(rom, source)
    if len(decoded) != DIALOG_FONT_DECODED_BYTES:
        raise ValueError(
            f'dialog font decoded to {len(decoded)} bytes, expected '
            f'{DIALOG_FONT_DECODED_BYTES}')
    tile_bytes = 16
    tiles = [decoded[offset:offset + tile_bytes]
             for offset in range(0, len(decoded), tile_bytes)]
    return {
        'status': 'asset_script_source_verified',
        'classification': 'regional_graphical_text_font',
        'source_file_offset': f'0x{source:06X}',
        'source_pc24': snes_string(source),
        'script_reference_count': len(references),
        'script_references': references,
        'decoded_byte_count': len(decoded),
        'compressed_byte_count': compressed_bytes,
        'decoded_sha256': hashlib.sha256(decoded).hexdigest(),
        'tile_count': len(tiles),
        'nonblank_tile_count': sum(any(tile) for tile in tiles),
        'unique_tile_count': len(set(tiles)),
        'redistribution': 'rom_derived_do_not_distribute',
    }


def build_graphical_text_census(profile, rom, consumer_census):
    """Classify every language-bearing graphical surface in v1 evidence.

    This closes an ownership obligation; it does not claim that graphical
    lettering has been OCRed or converted to editable language-pack text.
    """
    resources = []
    font = consumer_census['dialog_font_asset']
    resources.append({
        'id': 'font.dialog.native',
        'classification': 'regional_graphical_text_font',
        'replacement_path': 'native_or_enhanced_runtime_font',
        'source': font,
    })

    base_graphical = [
        row for row in consumer_census['bg3_buffer_writes'][
            'outside_classifications']
        if row.get('coverage_class') == 'graphical_text'
    ]
    direct_graphical = [
        row for row in consumer_census['bg3_direct_long_writes']['sites']
        if row.get('coverage_class') == 'graphical_text'
    ]
    enemy_rows = [
        row for row in base_graphical
        if row.get('semantic_id') == 'action.hud.enemy_label'
    ]
    direct_enemy_rows = [
        row for row in direct_graphical
        if row.get('semantic_id') == 'action.hud.enemy_label'
    ]
    if len(enemy_rows) != 1 or len(direct_enemy_rows) != 1:
        raise ValueError(
            f"{profile['id']}: action ENEMY graphical source changed")
    enemy = enemy_rows[0]
    resources.append({
        'id': enemy['semantic_id'],
        'classification': 'graphical_text_tile_strip',
        'replacement_path': 'enhanced_text_or_native_tile_strip',
        'write_site': enemy['write_site'],
        'destination_range': enemy['destination_range'],
        'tile_word_start': enemy['tile_word_start'],
        'tile_count': enemy['tile_count'],
    })

    expected_regions = {
        semantic_id
        for regions in BG3_HUD_GRAPHICAL_TEXT_REGIONS.values()
        for semantic_id, _ in regions
    }
    found_regions = set()
    template_rows = [
        row for row in direct_graphical
        if row.get('classification') == 'classified_graphical_text_template'
    ]
    if len(template_rows) != len(BG3_HUD_GRAPHICAL_TEXT_REGIONS):
        raise ValueError(
            f"{profile['id']}: graphical HUD template count changed")
    for template in template_rows:
        for region in template['graphical_text_regions']:
            semantic_id = region['semantic_id']
            if semantic_id in found_regions:
                raise ValueError(
                    f'{profile["id"]}: duplicate graphical region '
                    f'{semantic_id}')
            found_regions.add(semantic_id)
            resources.append({
                'id': semantic_id,
                'classification': 'graphical_text_tilemap_region',
                'replacement_path': 'enhanced_text_or_native_tilemap_region',
                'source_tilemap': template['source_tilemap'],
                'destination_range': template['destination_range'],
                'word_indices': region['word_indices'],
                'font_tile_ids': region['font_tile_ids'],
            })
    if found_regions != expected_regions:
        raise ValueError(
            f"{profile['id']}: graphical HUD identities changed: "
            f'{sorted(found_regions)}')

    resources.append(build_title_graphical_text_surface(rom))
    resources.append(build_ending_graphical_text_surface(rom))

    path_audits = {
        'unclassified_base_bg3_writes': consumer_census[
            'bg3_buffer_writes']['outside_unclassified_count'],
        'unclassified_direct_long_bg3_writes': consumer_census[
            'bg3_direct_long_writes']['unclassified_count'],
        'unclassified_direct_vram_paths': consumer_census[
            'direct_vram_port_writes']['unclassified_path_count'],
        'unclassified_dma_launches': consumer_census[
            'dma_launches']['unclassified_count'],
        'unclassified_vram_descriptor_families': consumer_census[
            'generic_vram_descriptor']['unclassified_family_count'],
        'unclassified_indirect_write_sites': consumer_census[
            'indirect_write_paths']['unclassified_count'],
    }
    complete = (
        consumer_census['whole_game_consumer_discovery_complete'] and
        font['status'] == 'asset_script_source_verified' and
        len(resources) == 11 and
        not any(path_audits.values()))
    return {
        'status': (
            'complete_language_bearing_graphical_surface_classification'
            if complete else
            'incomplete_language_bearing_graphical_surface_classification'),
        'complete': complete,
        'scope': 'whole_game_language_bearing_graphical_surfaces',
        'claim': (
            'All language-bearing graphical surfaces are owned by a stable '
            'replacement class; only structured text is author-editable.'),
        'resource_count': len(resources),
        'font_resource_count': 1,
        'tile_strip_or_tilemap_region_count': 8,
        'full_surface_count': 2,
        'path_audits': path_audits,
        'resources': resources,
        'boundaries': [
            ('Non-language regional art and gameplay graphics belong to the '
             'future regional-graphics registry, not language extraction.'),
            ('Title and ending/credits lettering remains graphical and is '
             'not represented as editable Unicode text.'),
            ('The ending page producer is deliberately not inferred from '
             'the runtime WRAM copy ABI.'),
        ],
    }


DIRECT_VRAM_PATH_DETAILS = {
    'asset_script_character_upload': {
        'destination': 'dynamic asset-script VMADD',
        'classification': 'contains_classified_language_graphics',
        'language_bearing_candidate': False,
        'detail': ('The regional dialog font is the independently resolved '
                   '$5000 upload; remaining commands are graphics assets.'),
    },
    'developed_world_map_upload': {
        'destination': '$0000-$3FFF',
        'classification': 'verified_non_language_graphics',
        'language_bearing_candidate': False,
    },
    'bg3_tilemap_clear': {
        'destination': '$5800-$5BFF',
        'classification': 'verified_non_language_clear',
        'language_bearing_candidate': False,
    },
    'action_obj_graphics_upload': {
        'destination': '$2000-$2FFF and $2D40-$2DBF',
        'classification': 'verified_non_language_obj_graphics',
        'language_bearing_candidate': False,
    },
    'simulation_tilemap_upload': {
        'destination': '$6000-$6FFF',
        'classification': 'verified_non_language_tilemap',
        'language_bearing_candidate': False,
    },
}


DMA_LAUNCH_DETAILS = {
    'oam_shadow': ('OAM shadow transfer', 'verified_non_language_oam'),
    'tilemap_record': ('queued tilemap record',
                       'verified_non_language_tilemap'),
    'cgram_flicker': ('flicker palette transfer',
                      'verified_non_language_palette'),
    'cgram_descriptor': ('queued palette descriptor',
                         'verified_non_language_palette'),
    'simulation_town_tilemap': ('simulation town tilemap',
                                'verified_non_language_tilemap'),
    'bg3_status_rows': ('$7F:B000 to BG3 VRAM $5800 (256 bytes)',
                        'known_text_staging_transfer'),
    'bg3_dialogue_rows': ('$7F:B100 to BG3 VRAM $5880 (1472 bytes)',
                          'known_text_staging_transfer'),
    'generic_vram_descriptor': ('two-slot generic VRAM descriptor',
                                'classified_graphics_descriptor'),
    'world_water_bg': ('world-map water background graphics',
                       'verified_non_language_graphics'),
    'world_water_obj': ('world-map water object graphics',
                        'verified_non_language_graphics'),
    'world_effect': ('$7F:47F0 world-map effect graphics',
                     'verified_non_language_graphics'),
    'dma_disable': ('DMA disable/acknowledgement', 'non_transfer_write'),
}

VRAM_DESCRIPTOR_ABIS = {
    'native': {
        'slot_0': {
            'source_address': '$D0', 'source_bank': '$D2',
            'vram_destination': '$D3', 'byte_count': '$D5',
        },
        'slot_1': {
            'source_address': '$D7', 'source_bank': '$D9',
            'vram_destination': '$DA', 'byte_count': '$DC',
        },
    },
    'shifted_western': {
        'slot_0': {
            'source_address': '$D1', 'source_bank': '$D3',
            'vram_destination': '$D4', 'byte_count': '$D6',
        },
        'slot_1': {
            'source_address': '$D8', 'source_bank': '$DA',
            'vram_destination': '$DB', 'byte_count': '$DD',
        },
    },
}

VRAM_DESCRIPTOR_PATTERN_HEX = {
    'native': {
        'action_magic_overlay':
            'a90685d2c220a9802d85d3a9800085d5',
        'action_spell_upload_a':
            '84d085d2c220faa9001f85d3',
        'action_spell_upload_b':
            'a9007e85d0e220a90785d2c220a9001f85d3',
        'tile_animation_source':
            '6900b885d7e220c210a6e186dc',
        'world_water_selector':
            '29c000186900b085d7a9400085dc',
        'scene_initialization_source':
            'a97fa200b886d78dd900',
        'animation_target_config':
            '1003a0001084da',
    },
    'shifted_western': {
        'action_magic_overlay':
            'a90685d3c220a9802d85d4a9800085d6',
        'action_spell_upload_a':
            '84d185d3c220faa9001f85d4',
        'action_spell_upload_b':
            'a9007e85d1e220a90785d3c220a9001f85d4',
        'tile_animation_source':
            '6900b885d8e220c210a6e286dd',
        'world_water_selector':
            '29c000186900b085d8a9400085dd',
        'scene_initialization_source':
            'a97fa200b886d88dda00',
        'animation_target_config':
            '1003a0001084db',
    },
}

VRAM_DESCRIPTOR_FAMILY_DETAILS = {
    'action_magic_overlay': {
        'slot': 0,
        'destination': '$2D80',
        'byte_count': '$0080',
        'classification': 'verified_non_language_action_graphics',
    },
    'action_spell_upload_a': {
        'slot': 0,
        'destination': '$1F00-$27FF in $0200-byte chunks',
        'classification': 'verified_non_language_spell_graphics',
    },
    'action_spell_upload_b': {
        'slot': 0,
        'destination': '$1F00-$27FF in $0200-byte chunks',
        'classification': 'verified_non_language_spell_graphics',
    },
    'tile_animation_source': {
        'slot': 1,
        'destination': '$0000 or $1000 (paired target configuration)',
        'classification': 'verified_non_language_animation_graphics',
    },
    'world_water_selector': {
        'slot': 1,
        'destination': '$0000 and $2A80 (fixed DMA consumers)',
        'classification': 'verified_non_language_water_graphics',
    },
    'scene_initialization_source': {
        'slot': 1,
        'destination': 'paired animation target',
        'classification': 'verified_non_language_scene_graphics',
    },
    'animation_target_config': {
        'slot': 1,
        'destination': '$0000 or $1000',
        'classification': 'verified_non_language_animation_target',
    },
}


INDIRECT_WRITE_FAMILY_DETAILS = {
    'oam_high_table': {
        'destination': '$7E:0580-$7E:05FF',
        'classification': 'verified_non_language_oam_bit_table',
        'proof': ('the owning renderer initializes the pointer to $0580 and '
                  'uses it as the 128-byte OAM high table'),
    },
    'developed_world_map_stamp': {
        'destination': '$7E:C000-$7E:FFFF',
        'classification': 'verified_non_language_world_map_tilemap',
        'proof': ('the owning routine initializes the long pointer to $C000 '
                  'and stamps the developed Mode-7 map'),
    },
    'deferred_ppu_register': {
        'destination': '$00:2100-$00:2132 selected PPU registers',
        'classification': 'verified_non_language_ppu_register_write',
        'proof': ('all producers install a bank-$00 PPU-register address '
                  'before the NMI tail performs the deferred byte store'),
    },
    'asset_workspace': {
        'destination': '$7E:6000-$7E:67FF',
        'classification': 'verified_non_language_asset_workspace',
        'proof': ('the command-5 asset path initializes the destination to '
                  '$6000 and copies $0800 bytes'),
    },
    'metatile_definitions': {
        'destination': '$7E:2100/$2900/$3100 definition tables',
        'classification': 'verified_non_language_metatile_definitions',
        'proof': ('the only callers select the three fixed metatile-table '
                  'bases before byte-swapping the decompressed words'),
    },
    'action_map': {
        'destination': '$7E:8000 or $7E:C000 action map',
        'classification': 'verified_non_language_action_tilemap',
        'proof': ('the command-4 destination table contains the two action '
                  'map workspaces; the copy length is the decoded map size'),
    },
    'developed_world_map': {
        'destination': '$7E:C000-$7E:FFFF',
        'classification': 'verified_non_language_world_map_tilemap',
        'proof': ('the world-map load path fixes the destination at $C000 '
                  'and copies $4000 bytes'),
    },
    'character_vram_readback': {
        'destination': '$7F:B800-$7F:BFFF',
        'classification': 'verified_non_language_character_graphics_snapshot',
        'proof': ('the VRAM readback path fixes the destination at $B800 and '
                  'copies $1000 bytes; this is outside the text canvas '
                  '$B000-$B7FF'),
    },
    'lzss_ring': {
        'destination': '$7E:2000-$7E:20FF',
        'classification': 'verified_non_language_lzss_ring_buffer',
        'proof': ('the decompressor initializes its bank-$7E circular output '
                  'pointer to $2000 and wraps its low byte'),
    },
}


def classify_indirect_write_paths(rom, census_profile):
    """Revalidate every structurally identified live indirect-store family.

    The US rooted CFG is the discovery baseline. Localized executables move
    several implementations and shift their direct-page ABI, so each exact
    ROM profile carries its own revalidated counterpart sites rather than
    inheriting US addresses. Destination ownership comes from the initializer
    and bounded callers of the containing routine, not from the two-byte store
    instruction alone.
    """
    rows = []
    seen_sites = set()
    for role, expected_hex, sites in census_profile.get(
            'indirect_write_sites', ()):
        detail = INDIRECT_WRITE_FAMILY_DETAILS[role]
        expected = bytes.fromhex(expected_hex)
        site_rows = []
        for site in sites:
            if site in seen_sites:
                raise ValueError(
                    f'duplicate indirect write site {pc24_string(site)}')
            seen_sites.add(site)
            offset = pc24_to_offset(site)
            actual = rom[offset:offset + len(expected)]
            if actual != expected:
                raise ValueError(
                    f'{role} indirect write changed at {pc24_string(site)}')
            site_rows.append({
                'write_site': pc24_string(site),
                'bytes_hex': actual.hex(' '),
            })
        rows.append({
            'id': role,
            'destination': detail['destination'],
            'classification': detail['classification'],
            'proof': detail['proof'],
            'language_bearing_candidate': False,
            'write_site_count': len(site_rows),
            'write_sites': site_rows,
        })

    rejected = []
    for site, expected_hex in census_profile.get(
            'indirect_write_rejected_sites', ()):
        expected = bytes.fromhex(expected_hex)
        offset = pc24_to_offset(site)
        actual = rom[offset:offset + len(expected)]
        if actual != expected:
            raise ValueError(
                f'rejected indirect decode changed at {pc24_string(site)}')
        rejected.append({
            'site': pc24_string(site),
            'bytes_hex': actual.hex(' '),
            'classification': 'non_executable_or_width_confused_decode',
        })

    live_count = sum(row['write_site_count'] for row in rows)
    decoded_count = census_profile.get(
        'indirect_write_decoded_reference_count')
    if decoded_count is not None and live_count + len(rejected) != decoded_count:
        raise ValueError('indirect-write evidence count is inconsistent')
    return {
        'status': 'all_identified_pointer_destinations_classified',
        'method': ('rooted US decoded-store census plus structurally matched '
                   'and data-flow-revalidated counterparts in every exact '
                   'supported regional executable'),
        'family_count': len(rows),
        'live_write_site_count': live_count,
        'rejected_decode_count': len(rejected),
        'decoded_reference_count': decoded_count,
        'unclassified_count': 0,
        'language_bearing_candidate_count': 0,
        'families': rows,
        'rejected_candidates': rejected,
    }


def find_unique_pattern_pc24(rom, pattern, role):
    matches = scan_pattern_pc24(rom, pattern)
    if len(matches) != 1:
        raise ValueError(
            f'{role}: expected one structural match, found {len(matches)}')
    return matches[0]


def classify_dma_launches(rom, census_profile):
    """Revalidate every decoded write that launches or disables DMA."""
    launch_sites = census_profile.get('dma_launch_sites', ())
    expected_roles = (DMA_LAUNCH_ROLES_JP if
                      len(launch_sites) == len(DMA_LAUNCH_ROLES_JP) else
                      DMA_LAUNCH_ROLES)
    observed_roles = tuple(role for role, _ in launch_sites)
    if observed_roles != expected_roles:
        raise ValueError('DMA launch roles or ordering changed')

    rows = []
    seen_sites = set()
    instruction_names = {
        bytes.fromhex('8d0b42'): 'sta_abs_mdasen',
        bytes.fromhex('8e0b42'): 'stx_abs_mdasen',
        bytes.fromhex('9c0b42'): 'stz_abs_mdasen',
    }
    for role, site in launch_sites:
        if site in seen_sites:
            raise ValueError(f'duplicate DMA launch site {pc24_string(site)}')
        seen_sites.add(site)
        offset = pc24_to_offset(site)
        raw = rom[offset:offset + 3]
        instruction = instruction_names.get(raw)
        expected_raw = bytes.fromhex(
            '9c0b42' if role == 'dma_disable' else
            '8e0b42' if role in ('tilemap_record', 'cgram_flicker') else
            '8d0b42')
        if instruction is None or raw != expected_raw:
            raise ValueError(f'DMA launch changed at {pc24_string(site)}')
        transfer, classification = DMA_LAUNCH_DETAILS[role]
        rows.append({
            'id': role,
            'write_site': pc24_string(site),
            'instruction': instruction,
            'bytes_hex': raw.hex(' '),
            'transfer': transfer,
            'classification': classification,
            'language_bearing_candidate': False,
        })
    return {
        'status': 'all_decoded_dma_launches_classified',
        'method': ('snesbuild xref v2 $420B write census, with exact '
                   'instruction bytes revalidated during extraction'),
        'decoded_write_site_count': len(rows),
        'bg3_known_text_transfer_count': sum(
            row['classification'] == 'known_text_staging_transfer'
            for row in rows),
        'generic_vram_descriptor_transfer_count': sum(
            row['id'] == 'generic_vram_descriptor' for row in rows),
        'unclassified_count': 0,
        'sites': rows,
    }


def build_vram_descriptor_census(rom, census_profile):
    """Locate the structural producer families for generic VRAM DMA."""
    abi_id = census_profile['vram_descriptor_abi']
    patterns = VRAM_DESCRIPTOR_PATTERN_HEX[abi_id]
    families = []
    for role, pattern_hex in patterns.items():
        site = find_unique_pattern_pc24(rom, bytes.fromhex(pattern_hex), role)
        families.append({
            'id': role,
            'site_pc24': pc24_string(site),
            **VRAM_DESCRIPTOR_FAMILY_DETAILS[role],
        })
    return {
        'status': 'all_structural_producer_families_classified',
        'method': ('exact producer/control-dependency signatures across the '
                   'supported ROM hash'),
        'abi': abi_id,
        'slots': VRAM_DESCRIPTOR_ABIS[abi_id],
        'producer_or_dependency_family_count': len(families),
        'unclassified_family_count': 0,
        'language_bearing_candidate_count': 0,
        'families': families,
        'limitation': ('The signatures cover the structurally identified '
                       'descriptor producers and dependencies; they do not '
                       'replace the separate computed-pointer write audit.'),
    }


def classify_direct_vram_port_paths(rom, census_profile):
    """Validate every decoded direct CPU write to the VRAM data ports."""
    paths = []
    seen_sites = set()
    for role, sites in census_profile.get('direct_vram_port_paths', ()):
        detail = DIRECT_VRAM_PATH_DETAILS[role]
        site_rows = []
        for site in sites:
            if site in seen_sites:
                raise ValueError(
                    f'duplicate direct VRAM write site {pc24_string(site)}')
            seen_sites.add(site)
            offset = pc24_to_offset(site)
            if rom[offset:offset + 4] == bytes.fromhex('8f182100'):
                instruction = 'sta_long'
                operand_width = 24
                raw = rom[offset:offset + 4]
            else:
                raw = rom[offset:offset + 3]
                instruction = {
                    bytes.fromhex('8d1821'): 'sta_abs_vmdatal',
                    bytes.fromhex('8d1921'): 'sta_abs_vmdatah',
                    bytes.fromhex('9c1821'): 'stz_abs_vmdatal',
                }.get(raw)
                operand_width = 16
            if instruction is None:
                raise ValueError(
                    f'direct VRAM write changed at {pc24_string(site)}')
            site_rows.append({
                'write_site': pc24_string(site),
                'instruction': instruction,
                'operand_width': operand_width,
                'bytes_hex': raw.hex(' '),
            })
        paths.append({
            'id': role,
            'destination': detail['destination'],
            'classification': detail['classification'],
            'language_bearing_candidate': detail[
                'language_bearing_candidate'],
            'write_site_count': len(site_rows),
            'write_sites': site_rows,
            **({'detail': detail['detail']} if 'detail' in detail else {}),
        })
    if len(seen_sites) != 9:
        raise ValueError(
            f'expected nine decoded direct VRAM-port writes, found '
            f'{len(seen_sites)}')
    return {
        'status': 'all_decoded_direct_paths_classified',
        'method': ('snesbuild xref v2 address-range census, with exact '
                   'instruction bytes revalidated during extraction'),
        'destination_ports': ['$2118', '$2119', '$00:2118', '$00:2119'],
        'decoded_write_site_count': len(seen_sites),
        'unclassified_path_count': sum(
            row['language_bearing_candidate'] for row in paths),
        'paths': paths,
    }


def classify_decoded_bg3_range_evidence(rom, census_profile):
    """Revalidate the non-long candidates from the decoded range audit.

    The raw long-address census remains authoritative for encoded long stores.
    This companion evidence rules out the absolute/DB-relative instructions
    surfaced by CFG decoding for the exact supported ROM; pointer-computed
    destinations remain an explicit limitation.
    """
    rejected = []
    for site, expected_hex in census_profile.get(
            'decoded_bg3_range_rejected_sites', ()):
        expected = bytes.fromhex(expected_hex)
        offset = pc24_to_offset(site)
        actual = rom[offset:offset + len(expected)]
        if actual != expected:
            raise ValueError(
                f'decoded BG3 range candidate changed at '
                f'{pc24_string(site)}')
        rejected.append({
            'site': pc24_string(site),
            'bytes_hex': actual.hex(' '),
            'classification': 'non_executable_or_overlapping_decode',
        })
    total = census_profile.get('decoded_bg3_range_reference_count')
    direct = census_profile.get('decoded_bg3_range_direct_long_count')
    if total is not None and direct + len(rejected) != total:
        raise ValueError('decoded BG3 range evidence count is inconsistent')
    return {
        'status': 'all_address_bearing_candidates_classified',
        'method': ('snesbuild xref v2 decoded $B000-$BFFF WRAM-mirror '
                   'address-range audit, pinned to the exact ROM hash'),
        'decoded_reference_count': total,
        'decoded_direct_long_reference_count': direct,
        'rejected_nonlong_candidate_count': len(rejected),
        'decoded_nonlong_bg3_write_count': 0,
        'rejected_nonlong_candidates': rejected,
        'limitation': ('Computed pointer destinations do not encode $B000 in '
                       'the instruction operand and require separate '
                       'consumer/data-flow evidence.'),
    }


def scan_calls_to_entry(rom, entry_pc24, call_kind):
    calls = scan_pattern_pc24(
        rom, routine_call_pattern(entry_pc24, call_kind))
    if call_kind == 'jsr':
        calls = [call for call in calls
                 if call >> 16 == entry_pc24 >> 16]
    return calls


def source_pc24(bank, local):
    if local < 0x8000:
        raise ValueError(
            f'unsupported source local address ${local:04X} in bank ${bank:02X}')
    return bank << 16 | local


def source_reference(address, reference_kind, via_call_site=None,
                     via_source_table=None, via_consumer_entry=None):
    reference = {
        'source_pc24': pc24_string(address),
        'reference_kind': reference_kind,
    }
    provenance_edges = (
        via_call_site is not None,
        via_source_table is not None,
        via_consumer_entry is not None,
    )
    if sum(provenance_edges) != 1:
        raise ValueError('source reference needs exactly one provenance edge')
    if via_call_site is not None:
        reference['via_call_site'] = pc24_string(via_call_site)
    elif via_source_table is not None:
        reference['via_source_table'] = pc24_string(via_source_table)
    else:
        reference['via_consumer_entry'] = pc24_string(via_consumer_entry)
    return reference


def pointer_targets(rom, table_pc24, count, target_bank):
    table_offset = pc24_to_offset(table_pc24)
    targets = []
    for index in range(count):
        entry_offset = table_offset + index * 2
        local = rom[entry_offset] | rom[entry_offset + 1] << 8
        targets.append(source_pc24(target_bank, local))
    return targets


def native_dictionary_consumers(profile, rom):
    """Verify each reader's bounded expansion and retain local provenance.

    In particular, fixed composition stops BEFORE a zero, and DE/FR dialogue
    appends a space only when all twelve entry bytes were emitted. Neither
    behavior follows from interpreting both space tiles as Unicode whitespace.
    """
    if profile['encoding'] != 'dictionary-12':
        return {}
    release = profile['id']
    interactive, fixed, upload_flag, trailing_space = \
        DICTIONARY_CONSUMER_PROFILES[release]
    dictionary_pc = offset_to_pc24(profile['dictionary'])
    dictionary_load = bytes((0xB9, dictionary_pc & 255,
                             (dictionary_pc >> 8) & 255))
    # Reader dispatch, saved cursor, (token & $7f)*12, bank, and count.
    prefix = bytes.fromhex(
        'b9 00 00 30 02 c8 60 c8 8b 5a c2 20 29 7f 00 48 0a 18 '
        '63 01 0a 0a a8 68 e2 20 a9') + bytes((dictionary_pc >> 16,)) + \
        bytes.fromhex('48 ab a9 0c')
    result = {}
    for consumer, entry in (('interactive', interactive), ('fixed', fixed)):
        if consumer == 'interactive':
            suffix = (bytes((0xEB, 0xE6, upload_flag)) + dictionary_load +
                      bytes.fromhex('9f 00 b0 7f e8 e8 c8 c9 20 f0') +
                      bytes((0x11 if trailing_space else 0x09,)) +
                      bytes.fromhex('eb 48 20 1c 90 68 3a d0 e6'))
            if trailing_space:
                suffix += bytes.fromhex('a9 20 9f 00 b0 7f e8 e8')
            suffix += bytes.fromhex('7a ab 80') + bytes((
                0xBA if trailing_space else 0xC2,))
        else:
            suffix = (bytes((0xEB,)) + dictionary_load + bytes.fromhex(
                'f0 0f 9f 00 b0 7f e8 e8 c8 c9 20 f0 04 eb 3a d0 eb '
                '7a ab 80 c7'))
        expected = prefix + suffix
        offset = pc24_to_offset(entry)
        if rom[offset:offset + len(expected)] != expected:
            raise ValueError(
                f'{release}: unrecognized {consumer} dictionary reader at '
                f'{pc24_string(entry)}')
        census = CONSUMER_CENSUS_PROFILES[release]
        owner = 'interactive' if consumer == 'interactive' else 'composer'
        start = pc24_to_offset(census[f'{owner}_entry_pc24'])
        end = pc24_to_offset(census[f'{owner}_end_pc24'])
        if routine_call_pattern(entry, 'jsr') not in rom[start:end]:
            raise ValueError(f'{release}: {consumer} dictionary reader is '
                             'not called by its profiled consumer')
        result[consumer] = {
            'reader_pc24': pc24_string(entry),
            'entry_bytes': DICTIONARY_ENTRY_BYTES,
            'stop_after_space': '20',
            'stop_before_zero': consumer == 'fixed',
            'append_space_on_full_entry':
                consumer == 'interactive' and trailing_space,
        }
    return result


def native_dialogue_layout(profile, rom):
    """Read cell geometry from the verified native clear loop, not HD bounds."""
    entry = CONSUMER_CENSUS_PROFILES[profile['id']]['interactive_entry_pc24']
    offset = pc24_to_offset(entry)
    if rom[offset + 14] != 0xA2 or rom[offset + 17] != 0x20:
        raise ValueError('unrecognized dialogue cursor/clear entry')
    clear_pc = (entry & 0xFF0000) | u16(rom, offset + 18)
    clear = pc24_to_offset(clear_pc)
    if (rom[clear:clear + 2] != bytes.fromhex('da a2') or
            rom[clear + 4:clear + 12] != bytes.fromhex('a9 06 48 da a9 00 eb a9') or
            rom[clear + 13:clear + 20] != bytes.fromhex('eb 9f 00 b0 7f e8 e8')):
        raise ValueError('unrecognized dialogue clear-loop geometry')
    origin = u16(rom, clear + 2)
    columns = rom[clear + 12]
    if (origin & 1 or origin >= 0x800 or not 1 <= columns <= 32 or
            (origin & 63) // 2 + columns > 32):
        raise ValueError('invalid native dialogue row extent')
    return {
        'column': (origin & 63) // 2, 'row': origin // 64,
        'columns': columns, 'glyph_advance_cells': 1,
        # Japanese does not have reliable space-delimited word boundaries.
        # Keep its authored breaks until its line-breaking rules are modeled.
        'space_delimited_words': profile['id'] != 'jp',
        'clear_routine_pc24': pc24_string(clear_pc),
    }


def build_consumer_census(profile, rom):
    """Census the two known BG3 consumers and every direct buffer write.

    This is a structural, address-only artifact.  It proves that the known
    interpreter/composer paths are located consistently across the five ROMs;
    it intentionally does not claim that no other language renderer exists.
    """
    census_profile = CONSUMER_CENSUS_PROFILES.get(profile['id'])
    if census_profile is None:
        raise ValueError(f"{profile['id']}: no text-consumer census profile")

    interactive_entry = census_profile['interactive_entry_pc24']
    composer_entry = census_profile['composer_entry_pc24']
    signatures = (
        ('interactive_dialogue', interactive_entry,
         INTERACTIVE_CONSUMER_SIGNATURE),
        ('fixed_text_composer', composer_entry, FIXED_COMPOSER_SIGNATURE),
    )
    for name, entry, signature in signatures:
        offset = pc24_to_offset(entry)
        if rom[offset:offset + len(signature)] != signature:
            raise ValueError(
                f"{profile['id']}: {name} signature changed at "
                f'{pc24_string(entry)}')

    interactive_calls = scan_pattern_pc24(
        rom, routine_call_pattern(interactive_entry, 'jsr'))
    expected_interactive = census_profile['interactive_call_count']
    if len(interactive_calls) != expected_interactive:
        raise ValueError(
            f"{profile['id']}: interactive consumer call-site count changed: "
            f'expected {expected_interactive}, found {len(interactive_calls)}')
    if any(call >> 16 != interactive_entry >> 16
           for call in interactive_calls):
        raise ValueError(
            f"{profile['id']}: bank-local interpreter has cross-bank raw call")

    interactive_sites = []
    source_references = []
    direct_sources = set()
    nonadjacent_calls = []
    nested_handler_matrices = []
    branch_join_layout = dict(
        census_profile.get('interactive_branch_join_layout', ()))
    observed_branch_joins = set()
    for call in interactive_calls:
        source_y = direct_y_source(rom, call)
        site = {'call_site': pc24_string(call)}
        if source_y is None:
            if call in branch_join_layout:
                raise ValueError(
                    f"{profile['id']}: branch-joined interpreter call lost "
                    f'its fallthrough source at {pc24_string(call)}')
            site['source_origin'] = 'control_flow_table_or_argument'
            nonadjacent_calls.append((call, site))
        else:
            incoming_branches = incoming_immediate_y_branches(rom, call)
            if incoming_branches:
                if call not in branch_join_layout:
                    raise ValueError(
                        f"{profile['id']}: unexpected branch-joined "
                        f'interpreter source at {pc24_string(call)}')
                candidate_sources = [
                    branch['source_y'] for branch in incoming_branches
                ] + [source_y]
                candidate_sources = list(dict.fromkeys(candidate_sources))
                expected_count = branch_join_layout[call]
                if len(candidate_sources) != expected_count:
                    raise ValueError(
                        f"{profile['id']}: branch-joined interpreter call "
                        f'{pc24_string(call)} expected {expected_count} '
                        f'sources, found {len(candidate_sources)}')
                observed_branch_joins.add(call)
                site['source_origin'] = 'branch_join_immediate_y'
                site['fallthrough_source_y'] = f'${source_y:04X}'
                site['candidate_source_y'] = [
                    f'${candidate:04X}' for candidate in candidate_sources]
                site['incoming_branches'] = [{
                    'branch_site': pc24_string(branch['branch_site']),
                    'source_y': f"${branch['source_y']:04X}",
                    'shape': branch['shape'],
                } for branch in incoming_branches]
                for candidate in candidate_sources:
                    direct_sources.add(candidate)
                    source_references.append(source_reference(
                        source_pc24(interactive_entry >> 16, candidate),
                        'interpreter_branch_join_immediate_y', call))
            else:
                if call in branch_join_layout:
                    raise ValueError(
                        f"{profile['id']}: expected branch join missing at "
                        f'{pc24_string(call)}')
                site['source_origin'] = 'adjacent_immediate_y'
                site['source_y'] = f'${source_y:04X}'
                direct_sources.add(source_y)
                source_references.append(source_reference(
                    source_pc24(interactive_entry >> 16, source_y),
                    'interpreter_adjacent_immediate_y', call))
        interactive_sites.append(site)

    missing_branch_joins = sorted(
        set(branch_join_layout) - observed_branch_joins)
    if missing_branch_joins:
        raise ValueError(
            f"{profile['id']}: missing branch-joined interpreter calls: "
            f'{[pc24_string(call) for call in missing_branch_joins]}')

    nonadjacent_layout = census_profile['interactive_nonadjacent_layout']
    if len(nonadjacent_calls) != len(nonadjacent_layout):
        raise ValueError(
            f"{profile['id']}: non-adjacent interpreter layout changed: "
            f'expected {len(nonadjacent_layout)}, '
            f'found {len(nonadjacent_calls)}')
    dialogue_source_bank = census_profile['dialogue_source_bank']
    for (call, site), origin in zip(nonadjacent_calls, nonadjacent_layout):
        site['source_origin'] = origin
        if origin == 'conditional_immediate':
            candidates = conditional_immediate_y_sources(rom, call)
            if len(candidates) != 3:
                raise ValueError(
                    f"{profile['id']}: conditional interpreter source at "
                    f'{pc24_string(call)} has {len(candidates)} candidates')
            site['target_resolution'] = 'verified_three_way_branch_join'
            site['candidate_source_y'] = [
                f'${candidate:04X}' for candidate in candidates]
            for candidate in candidates:
                source_references.append(source_reference(
                    source_pc24(interactive_entry >> 16, candidate),
                    'interpreter_conditional_immediate_y', call))
        elif origin == 'nested_handler_table':
            call_offset = pc24_to_offset(call)
            if profile['encoding'] == 'direct-glyph':
                pattern_offset = call_offset - 24
                table_pc24 = None
                while pattern_offset < call_offset - 4:
                    if rom[pattern_offset] == 0xBF:
                        candidate = (rom[pattern_offset + 1] |
                                     rom[pattern_offset + 2] << 8 |
                                     rom[pattern_offset + 3] << 16)
                        if candidate == offset_to_pc24(profile['handler_table']):
                            table_pc24 = candidate
                            break
                    pattern_offset += 1
            else:
                if rom[call_offset - 7] != 0x7D:
                    raise ValueError(
                        f"{profile['id']}: handler lookup shape changed at "
                        f'{pc24_string(call)}')
                local = rom[call_offset - 6] | rom[call_offset - 5] << 8
                table_pc24 = source_pc24(dialogue_source_bank, local)
            if table_pc24 is None:
                raise ValueError(
                    f"{profile['id']}: handler lookup table not found at "
                    f'{pc24_string(call)}')
            expected_table_pc24 = offset_to_pc24(profile['handler_table'])
            if table_pc24 != expected_table_pc24:
                raise ValueError(
                    f"{profile['id']}: handler lookup table changed: "
                    f'expected {pc24_string(expected_table_pc24)}, '
                    f'found {pc24_string(table_pc24)}')
            site['source_table_pc24'] = pc24_string(table_pc24)
            row_tables = pointer_targets(
                rom, table_pc24, HANDLER_CITY_COUNT,
                dialogue_source_bank)
            expected_first_row = table_pc24 + HANDLER_CITY_COUNT * 2
            expected_rows = [
                expected_first_row +
                row * HANDLER_SLOTS_PER_CITY * 2
                for row in range(HANDLER_CITY_COUNT)]
            if row_tables != expected_rows:
                raise ValueError(
                    f"{profile['id']}: nested handler row layout changed at "
                    f'{pc24_string(table_pc24)}')

            matrix_rows = []
            matrix_targets = []
            for row_index, row_table in enumerate(row_tables):
                targets = pointer_targets(
                    rom, row_table, HANDLER_SLOTS_PER_CITY,
                    dialogue_source_bank)
                matrix_targets.extend(targets)
                for target in targets:
                    source_references.append(source_reference(
                        target, 'interpreter_nested_handler_pointer',
                        via_source_table=row_table))
                matrix_rows.append({
                    'city_slot': row_index,
                    'source_table_pc24': pc24_string(row_table),
                    'pointer_count': len(targets),
                    'unique_target_count': len(set(targets)),
                    'target_pc24s': [
                        pc24_string(target) for target in targets],
                })
            site['target_resolution'] = 'resolved_pointer_matrix'
            site['row_count'] = len(matrix_rows)
            site['slots_per_row'] = HANDLER_SLOTS_PER_CITY
            site['unique_source_count'] = len(set(matrix_targets))
            nested_handler_matrices.append({
                'row_table_pc24': pc24_string(table_pc24),
                'row_count': len(matrix_rows),
                'slots_per_row': HANDLER_SLOTS_PER_CITY,
                'pointer_slot_count': len(matrix_targets),
                'unique_target_count': len(set(matrix_targets)),
                'rows': matrix_rows,
            })
        elif origin == 'offering_pointer_table':
            table_pc24 = offset_to_pc24(profile['offering_table'])
            site['source_table_pc24'] = pc24_string(table_pc24)
            site['pointer_count'] = OFFERING_POINTER_COUNT
            for target in pointer_targets(
                    rom, table_pc24, OFFERING_POINTER_COUNT,
                    dialogue_source_bank):
                if 'ending_table' in profile and \
                        pc24_to_offset(target) >= profile['ending_table']:
                    # Latin slot zero is a sentinel into the following ending
                    # table, not an offering-text source.
                    continue
                source_references.append(source_reference(
                    target, 'interpreter_offering_pointer_table', call))
        elif origin == 'yield_continuation':
            continuations = {
                continuation_call: (parent_source, continuation_source)
                for continuation_call, parent_source, continuation_source in
                census_profile.get('interactive_yield_continuations', ())
            }
            if call not in continuations:
                raise ValueError(
                    f"{profile['id']}: unprofiled yield continuation at "
                    f'{pc24_string(call)}')
            parent_source, continuation_source = continuations[call]
            decoded_parent = Decoder(rom, profile).decode_record(
                pc24_to_offset(parent_source))
            actual_continuation = offset_to_pc24(decoded_parent['end'])
            if actual_continuation != continuation_source:
                raise ValueError(
                    f"{profile['id']}: yield continuation at "
                    f'{pc24_string(call)} expected '
                    f'{pc24_string(continuation_source)}, found '
                    f'{pc24_string(actual_continuation)}')
            site['parent_source_pc24'] = pc24_string(parent_source)
            site['source_pc24'] = pc24_string(continuation_source)
            site['target_resolution'] = 'verified_returned_y_cursor'
            source_references.append(source_reference(
                continuation_source,
                'interpreter_yield_continuation', call))
        elif origin == 'dialogue_wrapper':
            # Dialogue wrapper callers are enumerated independently below.
            pass
        else:
            raise ValueError(
                f"{profile['id']}: unknown interpreter source origin {origin}")

    wrapper_rows = []
    relay_entry, relay_expected_calls = census_profile['dialogue_source_relay']
    relay_calls = scan_calls_to_entry(rom, relay_entry, 'jsr')
    if len(relay_calls) != relay_expected_calls:
        raise ValueError(
            f"{profile['id']}: dialogue source relay call count changed: "
            f'expected {relay_expected_calls}, found {len(relay_calls)}')
    relay_sources = []
    for call in relay_calls:
        source_y = direct_y_source(rom, call)
        if source_y is None:
            raise ValueError(
                f"{profile['id']}: unresolved dialogue relay source at "
                f'{pc24_string(call)}')
        incoming_branches = incoming_immediate_y_branches(rom, call)
        candidates = [
            branch['source_y'] for branch in incoming_branches
        ] + [source_y]
        for candidate in dict.fromkeys(candidates):
            address = source_pc24(dialogue_source_bank, candidate)
            relay_sources.append(pc24_string(address))
            source_references.append(source_reference(
                address,
                ('dialogue_wrapper_relay_branch_join_immediate_y'
                 if incoming_branches else
                 'dialogue_wrapper_relay_immediate_y'),
                call))

    for index, (entry, call_kind, expected_calls, wrapper_source_bank) in enumerate(
            census_profile['dialogue_wrappers']):
        calls = scan_calls_to_entry(rom, entry, call_kind)
        if len(calls) != expected_calls:
            raise ValueError(
                f"{profile['id']}: dialogue wrapper {index} call count "
                f'changed: expected {expected_calls}, found {len(calls)}')
        call_rows = []
        for call in calls:
            source_y = direct_y_source(rom, call)
            row = {'call_site': pc24_string(call)}
            if source_y is not None:
                incoming_branches = incoming_immediate_y_branches(rom, call)
                candidates = [
                    branch['source_y'] for branch in incoming_branches
                ] + [source_y]
                candidates = list(dict.fromkeys(candidates))
                if incoming_branches:
                    row['source_origin'] = 'branch_join_immediate_y'
                    row['source_pc24s'] = [pc24_string(source_pc24(
                        wrapper_source_bank, candidate))
                        for candidate in candidates]
                    row['incoming_branches'] = [{
                        'branch_site': pc24_string(branch['branch_site']),
                        'source_y': f"${branch['source_y']:04X}",
                        'shape': branch['shape'],
                    } for branch in incoming_branches]
                else:
                    row['source_origin'] = 'adjacent_immediate_y'
                    row['source_pc24'] = pc24_string(source_pc24(
                        wrapper_source_bank, source_y))
                for candidate in candidates:
                    address = source_pc24(wrapper_source_bank, candidate)
                    source_references.append(source_reference(
                        address,
                        ('dialogue_wrapper_branch_join_immediate_y'
                         if incoming_branches else
                         'dialogue_wrapper_adjacent_immediate_y'),
                        call))
            else:
                call_offset = pc24_to_offset(call)
                if rom[call_offset - 5] == 0xBF and \
                        rom[call_offset - 1] == 0xA8:
                    table_pc24 = (rom[call_offset - 4] |
                                  rom[call_offset - 3] << 8 |
                                  rom[call_offset - 2] << 16)
                    row['source_origin'] = 'indexed_long_pointer_table'
                    row['source_table_pc24'] = pc24_string(table_pc24)
                    row['pointer_count'] = 7
                    for target in pointer_targets(
                            rom, table_pc24, 7, wrapper_source_bank):
                        source_references.append(source_reference(
                            target, 'dialogue_wrapper_indexed_pointer_table',
                            call))
                elif call == relay_entry + 2:
                    row['source_origin'] = 'forwarded_y_from_source_relay'
                    row['relay_entry_pc24'] = pc24_string(relay_entry)
                else:
                    raise ValueError(
                        f"{profile['id']}: unresolved dialogue wrapper "
                        f'source at {pc24_string(call)}')
            call_rows.append(row)
        wrapper_rows.append({
            'id': f'dialogue_wrapper_{index}',
            'entry_pc24': pc24_string(entry),
            'call_kind': call_kind,
            'source_bank': f'${wrapper_source_bank:02X}',
            'call_site_count': len(call_rows),
            'call_sites': call_rows,
        })

    composer_table_rows = []
    for table_id, table_pc24, count, target_increment in \
            census_profile['composer_source_tables']:
        targets = [target + target_increment for target in pointer_targets(
            rom, table_pc24, count, table_pc24 >> 16)]
        for target in targets:
            target_offset = pc24_to_offset(target)
            if not profile['menu_start'] <= target_offset < profile['menu_end']:
                raise ValueError(
                    f"{profile['id']}: {table_id} target "
                    f'{pc24_string(target)} is outside the menu catalogue')
            source_references.append(source_reference(
                target, f'fixed_composer_{table_id}_pointer',
                via_source_table=table_pc24))
        composer_table_rows.append({
            'id': table_id,
            'source_table_pc24': pc24_string(table_pc24),
            'pointer_count': count,
            'target_increment': target_increment,
            'unique_target_count': len(set(targets)),
            'target_pc24s': [pc24_string(target) for target in targets],
        })

    def validate_composer_source(source_id, address):
        source_offset = pc24_to_offset(address)
        if not profile['menu_start'] <= source_offset < profile['menu_end']:
            raise ValueError(
                f"{profile['id']}: {source_id} source "
                f'{pc24_string(address)} is outside the menu catalogue')

    composer_direct_rows = []
    direct_source_table = census_profile.get('composer_direct_source_table')
    direct_source_table_row = None
    if direct_source_table is not None:
        table_pc24, source_ids, target_increment = direct_source_table
        targets = [target + target_increment for target in pointer_targets(
            rom, table_pc24, len(source_ids), table_pc24 >> 16)]
        for source_id, address in zip(source_ids, targets):
            validate_composer_source(source_id, address)
            source_references.append(source_reference(
                address, f'fixed_composer_direct_{source_id}',
                via_source_table=table_pc24))
            composer_direct_rows.append({
                'id': source_id,
                'source_pc24': pc24_string(address),
                'confidence': 'call_flow_verified',
            })
        direct_source_table_row = {
            'source_table_pc24': pc24_string(table_pc24),
            'pointer_count': len(source_ids),
            'target_increment': target_increment,
            'target_pc24s': [pc24_string(target) for target in targets],
        }

    composer_indexed_direct_rows = []
    for (source_id, address, native_index_address, table_pc24,
         target_ids) in census_profile.get(
             'composer_indexed_direct_sources', ()):
        validate_composer_source(source_id, address)
        source_offset = pc24_to_offset(address)
        expected_selector = bytes((
            0x08,
            native_index_address & 0xFF,
            native_index_address >> 8,
            table_pc24 & 0xFF,
            table_pc24 >> 8 & 0xFF,
            0x00,
        ))
        if rom[source_offset:source_offset + len(expected_selector)] != \
                expected_selector:
            raise ValueError(
                f"{profile['id']}: {source_id} indexed composer selector "
                f'changed at {pc24_string(address)}')
        source_references.append(source_reference(
            address, f'fixed_composer_indexed_{source_id}',
            via_consumer_entry=composer_entry))

        targets = pointer_targets(
            rom, table_pc24, len(target_ids), table_pc24 >> 16)
        target_rows = []
        for target_id, target in zip(target_ids, targets):
            validate_composer_source(target_id, target)
            source_references.append(source_reference(
                target, f'fixed_composer_indexed_{target_id}',
                via_source_table=table_pc24))
            target_rows.append({
                'id': target_id,
                'source_pc24': pc24_string(target),
            })
        composer_indexed_direct_rows.append({
            'id': source_id,
            'source_pc24': pc24_string(address),
            'selector_opcode': '08',
            'native_index_address': f'${native_index_address:04X}',
            'source_table_pc24': pc24_string(table_pc24),
            'pointer_count': len(target_rows),
            'targets': target_rows,
            'confidence': 'call_flow_verified',
        })

    composer_dynamic_rows = []
    for source_id, address in census_profile.get(
            'composer_dynamic_sources', ()):
        validate_composer_source(source_id, address)
        source_references.append(source_reference(
            address, f'fixed_composer_dynamic_{source_id}',
            via_consumer_entry=composer_entry))
        composer_dynamic_rows.append({
            'id': source_id,
            'source_pc24': pc24_string(address),
            'classification': 'language_bearing_dynamic_report',
        })

    composer_numeric_source = census_profile.get('composer_numeric_source')
    composer_numeric_row = None
    if composer_numeric_source is not None:
        validate_composer_source(
            'native_numeric_only', composer_numeric_source)
        composer_numeric_row = {
            'id': 'native_numeric_only',
            'source_pc24': pc24_string(composer_numeric_source),
            'classification': 'typed_values_without_language_text',
            'included_in_language_source_seeds': False,
        }

    composer_name_entry_rows = []
    for source_id, address in census_profile.get(
            'composer_name_entry_sources', ()):
        source_offset = pc24_to_offset(address)
        if not profile['menu_start'] <= source_offset < profile['menu_end']:
            raise ValueError(
                f"{profile['id']}: name-entry source {pc24_string(address)} "
                'is outside the menu catalogue')
        source_references.append(source_reference(
            address, f'fixed_composer_name_entry_{source_id}',
            via_consumer_entry=composer_entry))
        composer_name_entry_rows.append({
            'id': source_id,
            'source_pc24': pc24_string(address),
            'classification': 'language_bearing_name_entry',
            'confidence': 'call_flow_verified',
        })

    composer_calls = scan_pattern_pc24(
        rom, routine_call_pattern(composer_entry, 'jsl'))
    composer_groups = assign_ordered_groups(
        composer_calls, census_profile['composer_groups'])
    composer_sites = [
        {'call_site': pc24_string(call), 'surface_group': group}
        for call, group in zip(composer_calls, composer_groups)
    ]

    # Some fixed-composer streams are reached through stateful menu flow
    # rather than a ROM pointer table.  They still need explicit ownership:
    # otherwise the common Yes/No labels and message-speed scale can disappear
    # from an extraction which incorrectly reports itself as complete.
    composer_flow_rows = []
    flow_profile = census_profile.get('composer_flow_sources')
    if flow_profile is not None:
        selector_call = flow_profile['message_speed_selector_call_site']
        if selector_call not in composer_calls:
            raise ValueError(
                f"{profile['id']}: message-speed selector call is not a "
                'fixed-composer call site')
        selector_y = direct_y_source(rom, selector_call)
        if selector_y is None:
            raise ValueError(
                f"{profile['id']}: message-speed selector lost its direct "
                f'Y source at {pc24_string(selector_call)}')
        selector_source = source_pc24(selector_call >> 16, selector_y)
        validate_composer_source('message_speed_selector', selector_source)
        scale_descriptor = selector_source + 4
        scale_source = selector_source + 6
        validate_composer_source('message_speed_scale_labels', scale_source)

        composer_decoder = FixedComposerDecoder(Decoder(rom, profile))
        selector_record = composer_decoder.decode_record(
            pc24_to_offset(selector_source), profile['menu_end'])
        if offset_to_pc24(selector_record['end']) != scale_descriptor:
            raise ValueError(
                f"{profile['id']}: message-speed selector no longer ends "
                'at its scale descriptor')
        scale_record = composer_decoder.decode_record(
            pc24_to_offset(scale_source), profile['menu_end'])
        specifications = (JAPANESE_INTERACTIVE_ROUTE_IDS
                          if profile['id'] == 'jp' else
                          LATIN_INTERACTIVE_ROUTE_IDS)
        sample_call = next(
            call for call, semantic_id in specifications.items()
            if semantic_id == 'system.message_speed.sample')
        sample_y = direct_y_source(rom, sample_call)
        sample_source = source_pc24(sample_call >> 16, sample_y)
        if not scale_record['terminated'] or \
                offset_to_pc24(scale_record['end']) != sample_source:
            raise ValueError(
                f"{profile['id']}: message-speed scale no longer terminates "
                'at the sample dialogue source')

        choice_descriptor = flow_profile.get('choice_descriptor_pc24')
        choice_provenance = None
        if choice_descriptor is None:
            choice_call = flow_profile['choice_yield_call_site']
            continuation_rows = {
                call: continuation for call, _, continuation in
                census_profile.get('interactive_yield_continuations', ())
            }
            continuation = continuation_rows.get(choice_call)
            if continuation is None:
                raise ValueError(
                    f"{profile['id']}: choice-label continuation is missing")
            continuation_record = Decoder(rom, profile).decode_record(
                pc24_to_offset(continuation))
            if not continuation_record['terminated'] or \
                    continuation_record['operations'][-1]['op'] != 'yield':
                raise ValueError(
                    f"{profile['id']}: choice-label continuation no longer "
                    'returns a descriptor cursor')
            choice_descriptor = offset_to_pc24(continuation_record['end'])
            choice_provenance = choice_call
        else:
            expected_load = bytes((
                0xA2, choice_descriptor & 0xFF,
                choice_descriptor >> 8 & 0xFF))
            load_sites = flow_profile.get('choice_descriptor_load_sites', ())
            if not load_sites:
                raise ValueError(
                    f"{profile['id']}: choice descriptor has no load-site "
                    'evidence')
            for load_site in load_sites:
                load_offset = pc24_to_offset(load_site)
                if rom[load_offset:load_offset + 3] != expected_load:
                    raise ValueError(
                        f"{profile['id']}: choice descriptor load changed "
                        f'at {pc24_string(load_site)}')
            choice_provenance = load_sites[0]

        def validate_flow_descriptor(source_id, address):
            validate_composer_source(source_id, address)
            offset = pc24_to_offset(address)
            column, row = rom[offset:offset + 2]
            if column >= 32 or row >= 32:
                raise ValueError(
                    f"{profile['id']}: {source_id} has invalid BG3 cell "
                    f'destination {column:02X}/{row:02X}')
            return {'column': column, 'row': row}

        scale_destination = validate_flow_descriptor(
            'message_speed_scale_descriptor', scale_descriptor)
        choice_destination = validate_flow_descriptor(
            'choice_labels_descriptor', choice_descriptor)
        choice_source = choice_descriptor + 2
        validate_composer_source('choice_labels', choice_source)
        choice_record = composer_decoder.decode_record(
            pc24_to_offset(choice_source), profile['menu_end'])
        if not choice_record['terminated']:
            raise ValueError(
                f"{profile['id']}: choice labels have no terminator")

        composer_flow_rows.extend((
            {
                'id': 'message_speed_selector',
                'source_pc24': pc24_string(selector_source),
                'classification': 'typed_non_language_indicator',
                'included_in_language_source_seeds': False,
                'via_call_site': pc24_string(selector_call),
                'confidence': 'direct_call_flow_verified',
            },
            {
                'id': 'message_speed_scale_labels',
                'source_pc24': pc24_string(scale_source),
                'descriptor_pc24': pc24_string(scale_descriptor),
                'destination': scale_destination,
                'classification': 'fixed_composer_text',
                'included_in_language_source_seeds': True,
                'via_call_site': pc24_string(selector_call),
                'confidence': 'direct_call_and_adjacent_flow_verified',
            },
            {
                'id': 'choice_labels',
                'source_pc24': pc24_string(choice_source),
                'descriptor_pc24': pc24_string(choice_descriptor),
                'destination': choice_destination,
                'classification': 'fixed_composer_text',
                'included_in_language_source_seeds': True,
                'via_call_site': pc24_string(choice_provenance),
                'confidence': 'stateful_flow_and_destination_verified',
            },
        ))
        source_references.append(source_reference(
            scale_source,
            'fixed_composer_flow_message_speed_scale_labels',
            via_call_site=selector_call))
        source_references.append(source_reference(
            choice_source, 'fixed_composer_flow_choice_labels',
            via_call_site=choice_provenance))

    buffer_writes = scan_pattern_pc24(rom, BG3_TEXT_BUFFER_WRITE)
    expected_writes = census_profile['bg3_buffer_write_count']
    if len(buffer_writes) != expected_writes:
        raise ValueError(
            f"{profile['id']}: BG3 buffer-write census changed: expected "
            f'{expected_writes}, found {len(buffer_writes)}')
    interactive_end = census_profile['interactive_end_pc24']
    composer_end = census_profile['composer_end_pc24']
    write_sites = []
    for site in buffer_writes:
        if interactive_entry <= site < interactive_end:
            owner = 'interactive_dialogue_and_helpers'
        elif composer_entry <= site < composer_end:
            owner = 'fixed_text_composer_and_helpers'
        else:
            owner = 'outside_known_text_consumers'
        write_sites.append({'write_site': pc24_string(site), 'owner': owner})

    outside_writes = sum(
        site['owner'] == 'outside_known_text_consumers'
        for site in write_sites)
    if outside_writes != 5:
        raise ValueError(
            f"{profile['id']}: expected five buffer maintenance/write sites "
            f'outside known consumers, found {outside_writes}')
    outside_sites = [
        site for site in write_sites
        if site['owner'] == 'outside_known_text_consumers'
    ]
    outside_roles = census_profile.get('bg3_outside_write_roles')
    outside_classifications = []
    if outside_roles is not None:
        if len(outside_roles) != len(outside_sites):
            raise ValueError(
                f"{profile['id']}: outside BG3 write role count changed")
        for site, role in zip(outside_sites, outside_roles):
            site_pc24 = (int(site['write_site'][1:3], 16) << 16 |
                         int(site['write_site'][4:], 16))
            classification = classify_outside_bg3_write(
                rom, site_pc24, role)
            site.update({
                'role': classification['role'],
                'classification': classification['classification'],
                'language_bearing_candidate': classification[
                    'language_bearing_candidate'],
            })
            outside_classifications.append(classification)

    direct_long_rows = scan_direct_long_bg3_writes(rom)
    expected_direct_long_count = census_profile.get(
        'bg3_direct_long_write_count')
    if expected_direct_long_count is not None and \
            len(direct_long_rows) != expected_direct_long_count:
        raise ValueError(
            f"{profile['id']}: direct long BG3 write count changed: "
            f'expected {expected_direct_long_count}, '
            f'found {len(direct_long_rows)}')
    base_classifications = {
        row['write_site']: row for row in outside_classifications
    }
    auxiliary_roles = {
        address: role for role, address in
        census_profile.get('bg3_direct_long_auxiliary', ())
    }
    noncode_sites = set(census_profile.get('bg3_direct_long_noncode', ()))
    unclassified_direct_long = []
    for row in direct_long_rows:
        site = row.pop('site_pc24_value')
        if site in noncode_sites:
            row['owner'] = 'non_executable_rom_data'
            row['classification'] = 'decoded_cfg_rejected_instruction'
            row['language_bearing_candidate'] = False
        elif interactive_entry <= site < interactive_end:
            row['owner'] = 'interactive_dialogue_and_helpers'
            row['classification'] = 'known_text_consumer'
            row['language_bearing_candidate'] = False
        elif composer_entry <= site < composer_end:
            row['owner'] = 'fixed_text_composer_and_helpers'
            row['classification'] = 'known_text_consumer'
            row['language_bearing_candidate'] = False
        elif row['write_site'] in base_classifications:
            classification = base_classifications[row['write_site']]
            row['owner'] = 'profiled_auxiliary_surface_write'
            row['role'] = classification['role']
            row['classification'] = classification['classification']
            row['language_bearing_candidate'] = classification[
                'language_bearing_candidate']
            for key in ('coverage_class', 'semantic_id',
                        'destination_range', 'tile_word_start', 'tile_count'):
                if key in classification:
                    row[key] = classification[key]
        elif site in auxiliary_roles:
            role = auxiliary_roles[site]
            row['owner'] = 'profiled_auxiliary_surface_write'
            row['role'] = role
            if role in BG3_HUD_GRAPHICAL_TEXT_REGIONS:
                row.update(classify_bg3_hud_template_copy(
                    rom, site, role, census_profile))
            elif role == 'manual_dialogue_attribute':
                row['classification'] = 'presentation_attribute_write'
                row['language_bearing_candidate'] = False
            else:
                raise ValueError(
                    f"{profile['id']}: unknown direct long BG3 role {role}")
        else:
            row['owner'] = 'unclassified'
            row['classification'] = 'unclassified'
            row['language_bearing_candidate'] = True
            unclassified_direct_long.append(row['write_site'])
    if expected_direct_long_count is not None and unclassified_direct_long:
        raise ValueError(
            f"{profile['id']}: unclassified direct long BG3 writes at "
            f'{unclassified_direct_long}')

    if 'direct_vram_port_paths' in census_profile:
        decoded_range_evidence = classify_decoded_bg3_range_evidence(
            rom, census_profile)
        direct_vram_paths = classify_direct_vram_port_paths(
            rom, census_profile)
        dma_launches = classify_dma_launches(rom, census_profile)
        vram_descriptor = build_vram_descriptor_census(
            rom, census_profile)
        indirect_writes = classify_indirect_write_paths(
            rom, census_profile)
        dialog_font = build_dialog_font_census(rom)
    else:
        decoded_range_evidence = {'status': 'not_profiled'}
        direct_vram_paths = {'status': 'not_profiled'}
        dma_launches = {'status': 'not_profiled'}
        vram_descriptor = {'status': 'not_profiled'}
        indirect_writes = {'status': 'not_profiled'}
        dialog_font = {'status': 'not_profiled'}

    consumer_discovery_complete = indirect_writes['status'] != 'not_profiled'
    return {
        'status': ('all_text_consumer_families_censused'
                   if consumer_discovery_complete else
                   'known_consumer_families_censused'),
        'whole_game_consumer_discovery_complete': consumer_discovery_complete,
        'method': ('exact routine signatures, exhaustive raw call-pattern '
                   'and $7F:B000 long-store scans, decoded address-bearing '
                   'and indirect-store audits, and complete CPU/DMA VRAM '
                   'destination classification'),
        'consumers': [
            {
                'id': 'interactive_dialogue',
                'entry_pc24': pc24_string(interactive_entry),
                'call_kind': 'bank_local_jsr',
                'call_site_count': len(interactive_sites),
                'adjacent_immediate_y_call_count': sum(
                    site['source_origin'] in (
                        'adjacent_immediate_y', 'branch_join_immediate_y')
                    for site in interactive_sites),
                'branch_join_immediate_y_call_count': sum(
                    site['source_origin'] == 'branch_join_immediate_y'
                    for site in interactive_sites),
                'unique_immediate_y_sources': len(direct_sources),
                'nonadjacent_source_origin_call_count': sum(
                    site['source_origin'] not in (
                        'adjacent_immediate_y', 'branch_join_immediate_y')
                    for site in interactive_sites),
                'unresolved_source_origin_call_count': sum(
                    site['source_origin'] == 'control_flow_table_or_argument'
                    for site in interactive_sites),
                'call_sites': interactive_sites,
            },
            {
                'id': 'fixed_text_composer',
                'entry_pc24': pc24_string(composer_entry),
                'call_kind': 'long_jsl',
                'call_site_count': len(composer_sites),
                'surface_group_counts': {
                    group: composer_groups.count(group)
                    for group in sorted(set(composer_groups))
                },
                'call_sites': composer_sites,
            },
        ],
        'dialogue_forwarding': {
            'source_bank': f'${dialogue_source_bank:02X}',
            'wrappers': wrapper_rows,
            'source_relay': {
                'entry_pc24': pc24_string(relay_entry),
                'call_site_count': len(relay_calls),
                'source_pc24s': relay_sources,
            },
        },
        'nested_handler_sources': {
            'status': 'pointer_matrix_censused',
            'matrices': nested_handler_matrices,
            'pointer_slot_count': sum(
                matrix['pointer_slot_count']
                for matrix in nested_handler_matrices),
            'unique_target_count': len({
                target for matrix in nested_handler_matrices
                for row in matrix['rows']
                for target in row['target_pc24s']}),
        },
        'fixed_composer_sources': {
            'status': (
                'tables_direct_dynamic_and_stateful_flows_censused'
                if composer_flow_rows else
                'known_tables_direct_dynamic_censused'),
            'pointer_tables': composer_table_rows,
            'pointer_slot_count': sum(
                row['pointer_count'] for row in composer_table_rows),
            'unique_pointer_target_count': len({
                target for row in composer_table_rows
                for target in row['target_pc24s']}),
            'direct_sources': composer_direct_rows,
            'direct_source_table': direct_source_table_row,
            'indexed_direct_sources': composer_indexed_direct_rows,
            'direct_source_candidates': [],
            'dynamic_reports': composer_dynamic_rows,
            'score_report_present': any(
                row['id'] == 'score_report'
                for row in composer_dynamic_rows),
            'numeric_only_source': composer_numeric_row,
            'name_entry_sources': composer_name_entry_rows,
            'flow_sources': composer_flow_rows,
        },
        'source_reference_seeds': {
            'status': 'known_call_paths_censused',
            'reference_count': len(source_references),
            'unique_source_count': len({
                reference['source_pc24'] for reference in source_references}),
            'references': source_references,
        },
        'bg3_buffer_writes': {
            'destination_base': '$7F:B000',
            'direct_write_site_count': len(write_sites),
            'outside_known_text_consumer_count': outside_writes,
            'outside_write_classification_status': (
                'all_profiled_sites_classified'
                if outside_roles is not None else 'not_profiled'),
            'outside_unclassified_count': (
                0 if outside_roles is not None else outside_writes),
            'outside_graphical_candidate_count': sum(
                row['language_bearing_candidate']
                for row in outside_classifications),
            'outside_graphical_text_source_count': sum(
                row.get('coverage_class') == 'graphical_text'
                for row in outside_classifications),
            'outside_classifications': outside_classifications,
            'sites': write_sites,
        },
        'bg3_direct_long_writes': {
            'destination_range': '$7F:B000-$7F:BFFF',
            'opcodes': ['STA long', 'STA long,X'],
            'status': (
                'all_raw_candidates_classified'
                if expected_direct_long_count is not None else
                'not_profiled'),
            'raw_candidate_count': len(direct_long_rows),
            'decoded_instruction_count': sum(
                row['owner'] != 'non_executable_rom_data'
                for row in direct_long_rows),
            'noncode_pattern_count': sum(
                row['owner'] == 'non_executable_rom_data'
                for row in direct_long_rows),
            'unclassified_count': len(unclassified_direct_long),
            'graphical_candidate_count': sum(
                row['language_bearing_candidate']
                for row in direct_long_rows),
            'graphical_text_source_count': sum(
                row.get('coverage_class') == 'graphical_text'
                for row in direct_long_rows),
            'graphical_text_region_count': sum(
                row.get('graphical_text_region_count',
                        1 if row.get('coverage_class') == 'graphical_text'
                        else 0)
                for row in direct_long_rows),
            'sites': direct_long_rows,
        },
        'bg3_decoded_address_range_audit': decoded_range_evidence,
        'direct_vram_port_writes': direct_vram_paths,
        'dma_launches': dma_launches,
        'generic_vram_descriptor': vram_descriptor,
        'indirect_write_paths': indirect_writes,
        'dialog_font_asset': dialog_font,
        'limitations': [
            ('Raw instruction patterns are exact for supported ROM hashes, '
             'but do not by themselves prove whole-ROM language coverage.'),
            ('Known immediate, branch-joined, nested-table, wrapper, relay, '
             'and continuation paths are resolved. The exact-ROM gates '
             'fail closed if any profiled instruction or family changes.'),
            ('All decoded direct CPU VRAM-port and DMA-launch paths are '
             'classified, the regional font upload is resolved from the '
             'asset script, known generic VRAM-descriptor producers are '
             'classified as graphics, and the identified indirect-store '
             'families have bounded non-text destinations.'),
        ],
    }


def resolve_consumer_reference_seeds(consumer_census, messages, menu):
    """Map each discovered source address to one decoded local record.

    A source may point into a record after an earlier yield, so containment is
    intentional; requiring an exact record start would lose continuation
    evidence. This proves that every current reference seed is preserved by
    extraction, not that every possible consumer family has been discovered.
    """
    records = list(messages) + list(menu['segments'])
    ranges = []
    for record in records:
        source = record['source']
        ranges.append((
            int(source['file_offset'], 16),
            int(source['end_file_offset_exclusive'], 16),
            record['id']))

    references = consumer_census['source_reference_seeds']['references']
    unique_sources = {}
    for reference in references:
        address_text = reference['source_pc24']
        address = (int(address_text[1:3], 16) << 16 |
                   int(address_text[4:], 16))
        offset = pc24_to_offset(address)
        matches = [
            (start, record_id) for start, end, record_id in ranges
            if start <= offset < end]
        if matches:
            # Pointer targets can intentionally begin inside a longer record
            # reached from another slot. Prefer an exact target start, then
            # the closest containing start for a continuation cursor.
            exact = [match for match in matches if match[0] == offset]
            if len(exact) > 1:
                raise ValueError(
                    f'{address_text}: duplicate exact source records')
            start, record_id = (exact[0] if exact else max(matches))
            reference['resolved_record_id'] = record_id
            reference['source_offset_within_record'] = offset - start
            unique_sources[address_text] = record_id
        else:
            reference['resolved_record_id'] = None
            unique_sources.setdefault(address_text, None)

    unmapped = sorted(
        source for source, record_id in unique_sources.items()
        if record_id is None)
    resolution = {
        'unique_source_count': len(unique_sources),
        'mapped_unique_source_count': len(unique_sources) - len(unmapped),
        'unmapped_unique_source_count': len(unmapped),
        'all_current_seeds_mapped': not unmapped,
        'unmapped_source_pc24s': unmapped,
    }
    consumer_census['source_reference_resolution'] = resolution
    return resolution


def expand_unmapped_dialogue_seeds(consumer_census, messages, menu, profile,
                                   rom, decoder):
    """Decode interpreter sources that fall outside bounded inventories.

    Runtime pointer matrices can intentionally target a prefix immediately
    before a sequential block. Fixed-composer misses instead indicate a menu
    catalogue defect and remain fatal rather than being decoded with the
    interactive grammar.
    """
    resolution = resolve_consumer_reference_seeds(
        consumer_census, messages, menu)
    if resolution['all_current_seeds_mapped']:
        consumer_census['source_seed_expansion'] = {
            'added_record_count': 0,
            'added_record_ids': [],
        }
        return resolution

    references = consumer_census['source_reference_seeds']['references']
    by_source = {}
    for reference in references:
        if reference['resolved_record_id'] is None:
            by_source.setdefault(reference['source_pc24'], []).append(
                reference['reference_kind'])

    by_offset = {
        int(message['source']['file_offset'], 16): message
        for message in messages
    }
    added_ids = []
    for address_text in sorted(by_source):
        reference_kinds = by_source[address_text]
        if not all(kind.startswith(('interpreter_', 'dialogue_wrapper_'))
                   for kind in reference_kinds):
            raise ValueError(
                f'{profile["id"]}: non-dialogue consumer source '
                f'{address_text} is absent from the bounded catalogues')
        address = (int(address_text[1:3], 16) << 16 |
                   int(address_text[4:], 16))
        offset = pc24_to_offset(address)
        message, _ = add_message(
            messages, by_offset, profile, rom, decoder, offset,
            'dialogue_consumer_seed',
            f'dialogue.consumer_seed.{address_text[1:3].lower()}.'
            f'{address_text[4:].lower()}',
            discovery='consumer_reference_seed')
        if not message['terminated']:
            raise ValueError(
                f'{profile["id"]}: consumer source {address_text} does not '
                'reach an end control')
        added_ids.append(message['id'])

    consumer_census['source_seed_expansion'] = {
        'added_record_count': len(added_ids),
        'added_record_ids': added_ids,
    }
    return resolve_consumer_reference_seeds(
        consumer_census, messages, menu)


def native_id(profile, offset):
    bank, address = offset_to_snes(offset)
    return f"native.{profile['id']}.bank{bank:02x}.{address:04x}"


def pointer_to_offset(table_offset, pointer):
    """Map a 16-bit pointer into the same LoROM bank as its table."""
    if pointer < 0x8000:
        raise ValueError(f'non-ROM pointer ${pointer:04X} at {snes_string(table_offset)}')
    return table_offset // 0x8000 * 0x8000 + pointer - 0x8000


def base_glyph_map(overrides=None):
    result = {
        0x20: ' ', 0x21: '!', 0x28: '`', 0x29: "'", 0x2A: '"',
        0x2B: '"', 0x2C: ',', 0x2D: '-', 0x2E: '.', 0x2F: '/',
        0x3A: ':', 0x3C: '<', 0x3D: '=', 0x3E: '>',
        0x3F: '?', 0x40: ' ', 0x5F: '_',
        0x60: '`',
    }
    result.update({code: chr(code) for code in range(0x30, 0x3A)})
    result.update({code: chr(code) for code in range(0x41, 0x5B)})
    result.update({code: chr(code) for code in range(0x61, 0x7B)})
    result.update(overrides or {})
    return result


class Decoder:
    def __init__(self, rom, profile):
        self.rom = rom
        self.profile = profile
        self.encoding = profile['encoding']
        self.glyphs = base_glyph_map(profile.get('glyph_overrides'))
        self.icon_glyphs = profile.get('icon_glyphs', {})
        self.dictionary = None
        if self.encoding == 'dictionary-12':
            start = profile['dictionary']
            size = DICTIONARY_ENTRY_COUNT * DICTIONARY_ENTRY_BYTES
            self.dictionary = rom[start:start + size]
            if len(self.dictionary) != size:
                raise ValueError('truncated dictionary')

    def dictionary_bytes(self, token, *, consumer):
        if consumer not in ('interactive', 'fixed'):
            raise ValueError('dictionary expansion requires a known consumer')
        if self.dictionary is None or not 0x80 <= token <= 0xFF:
            raise ValueError('invalid dictionary token')
        index = token & 0x7F
        start = index * DICTIONARY_ENTRY_BYTES
        entry = self.dictionary[start:start + DICTIONARY_ENTRY_BYTES]
        for offset, code in enumerate(entry):
            if consumer == 'fixed' and code == 0:
                return entry[:offset]
            if code == 0x20:
                return entry[:offset + 1]
        # $40 is also a blank glyph, but never a dictionary terminator.
        # Only the DE/FR interactive loop emits an extra space after count=0.
        if consumer == 'interactive' and self.profile.get('id') in ('de', 'fr'):
            return entry + b'\x20'
        return entry

    def decode_record(self, start, limit=None, stop_on_yield=True):
        limit = len(self.rom) if limit is None else limit
        position = start
        operations = []
        text_buffer = []
        glyph_buffer = []
        dictionary_tokens = []
        terminated = False

        def flush_text():
            if text_buffer:
                operations.append({'op': 'text', 'value': ''.join(text_buffer)})
                text_buffer.clear()

        def flush_glyphs():
            if glyph_buffer:
                operations.append({
                    'op': 'native_glyphs',
                    'codes': ' '.join(f'{code:02X}' for code in glyph_buffer),
                    'unicode': None,
                })
                glyph_buffer.clear()

        def flush():
            flush_text()
            flush_glyphs()

        def emit_glyph(code):
            icon = self.icon_glyphs.get(code)
            if icon is not None:
                append_operation({
                    'op': 'insert_icon',
                    'value': icon['value'],
                    'native_code': f'{code:02X}',
                    'part_index': icon['part_index'],
                    'part_count': icon['part_count'],
                    'confidence': 'mapped_semantic',
                })
                return
            character = self.glyphs.get(code)
            if character is not None:
                flush_glyphs()
                text_buffer.append(character)
            else:
                flush_text()
                glyph_buffer.append(code)

        def append_operation(operation):
            flush()
            operations.append(operation)

        def emit_diacritic(code):
            combining = '\u3099' if code == 0xDE else '\u309A'
            if text_buffer:
                base = text_buffer[-1]
                if len(base) == 1 and ('\u3040' <= base <= '\u30FF'):
                    text_buffer[-1] = unicodedata.normalize(
                        'NFC', base + combining)
                    return
            append_operation({
                'op': 'native_diacritic',
                'code': f'{code:02X}',
                'kind': 'dakuten' if code == 0xDE else 'handakuten',
                'unicode': None,
            })

        while position < limit:
            code = self.rom[position]
            position += 1

            if code == 0x00:
                flush()
                operations.append({
                    'op': 'end',
                    'native_cursor_after': snes_string(position),
                })
                terminated = True
                break
            if code == 0x01:
                if stop_on_yield:
                    append_operation({
                        'op': 'yield',
                        'native_cursor_after': snes_string(position),
                    })
                    terminated = True
                    break
                append_operation({
                    'op': 'yield',
                    'native_cursor_after': snes_string(position),
                })
                continue
            if code == 0x02:
                append_operation({'op': 'page_break', 'confidence': 'mapped'})
                continue
            if code == 0x03:
                append_operation({'op': 'delay', 'frames': 30})
                continue
            if code == 0x04:
                append_operation({
                    'op': 'toggle_text_state',
                    'native_control': '04',
                    'confidence': 'mapped_presentation_only',
                })
                continue
            if code == 0x05:
                append_operation({'op': 'reset_text_cursor',
                                  'confidence': 'mapped'})
                continue
            if code == 0x06:
                # Decode addressing words from the supplied ROM, never from
                # hardcoded English or the runtime's plain player-name value.
                prefix = self.profile.get('dialogue_name_prefix')
                if prefix is not None:
                    record = FixedComposerDecoder(self).decode_record(
                        prefix, min(prefix + 32, len(self.rom)))
                    for operation in record['operations']:
                        if operation['op'] == 'text':
                            append_operation(operation)
                        elif operation['op'] != 'end':
                            raise ValueError('non-text dialogue name prefix')
                append_operation({'op': 'insert_master_name'})
                separator = self.profile.get('dialogue_name_separator')
                if separator:
                    append_operation({'op': 'text', 'value': separator})
                continue
            if code == 0x0D:
                append_operation({'op': 'line_break'})
                continue
            if code in CONTROL_ARGUMENTS:
                argument_count = CONTROL_ARGUMENTS[code]
                if position + argument_count > limit:
                    append_operation({
                        'op': 'truncated_native_control',
                        'code': f'{code:02X}',
                        'available_args_hex': self.rom[position:limit].hex(' ').upper(),
                    })
                    position = limit
                    break
                arguments = self.rom[position:position + argument_count]
                position += argument_count
                if code == 0x09:
                    native_address = f'${u16(arguments, 1):04X}'
                    append_operation({
                        'op': 'format_number',
                        'width': arguments[0],
                        'native_address': native_address,
                        'value': self.profile.get(
                            'number_semantics', {}).get(native_address),
                        'confidence': ('mapped_semantic' if native_address in
                                       self.profile.get('number_semantics', {})
                                       else 'mapped_address_only'),
                    })
                elif code == 0x0B:
                    flush_glyphs()
                    text_buffer.extend(' ' * arguments[0])
                elif code == 0x08:
                    args_hex = arguments.hex(' ').upper()
                    semantic = self.profile.get(
                        'indexed_text_semantics', {}).get(args_hex)
                    append_operation({
                        'op': 'insert_indexed_text',
                        'code': f'{code:02X}',
                        'args_hex': args_hex,
                        'index_address': f'${u16(arguments, 0):04X}',
                        'pointer_table': f'${u16(arguments, 2):04X}',
                        'value': semantic,
                        'confidence': ('mapped_semantic' if semantic else
                                       'unresolved_semantic'),
                    })
                else:
                    append_operation({
                        'op': 'native_control',
                        'code': f'{code:02X}',
                        'args_hex': arguments.hex(' ').upper(),
                        'confidence': 'unresolved' if code in (0x08, 0x0A)
                        else 'mapped_reserved',
                    })
                continue

            if self.encoding == 'dictionary-12' and code >= 0x80:
                dictionary_tokens.append(f'{code:02X}')
                for expanded in self.dictionary_bytes(
                        code, consumer='interactive'):
                    emit_glyph(expanded)
                continue

            if self.encoding == 'direct-glyph' and code in (0xDE, 0xDF):
                emit_diacritic(code)
                continue

            emit_glyph(code)

        flush()
        return {
            'operations': operations,
            'end': position,
            'terminated': terminated,
            'dictionary_tokens': dictionary_tokens,
        }


class FixedComposerDecoder:
    """Decode the distinct fixed-text compositor stream grammar.

    Unlike interactive dialogue, composer bytes 02-05/07/0A are inert
    one-byte controls, 08 owns four operand bytes, 09 owns three, 0B owns a
    spacing count, and 0D advances one tilemap row. Treating this mixed bank
    with the dialogue grammar was the source of the prototype's fake unknown
    controls and overlong records.
    """

    def __init__(self, decoder):
        self.decoder = decoder
        self.rom = decoder.rom
        self.profile = decoder.profile
        self.glyphs = decoder.glyphs
        self.icon_glyphs = decoder.icon_glyphs
        census = CONSUMER_CENSUS_PROFILES.get(self.profile.get('id'), {})
        population = dict(census.get('composer_dynamic_sources', ())).get(
            'cities_report')
        self.population_source = pc24_to_offset(population) if population else None
        self.speed_source = None
        flow = census.get('composer_flow_sources')
        if flow:
            call = flow['message_speed_selector_call_site']
            selector = direct_y_source(self.rom, call)
            if selector is not None:
                self.speed_source = pc24_to_offset(
                    source_pc24(call >> 16, selector) + 6)

    def decode_record(self, start, limit):
        position = start
        operations = []
        text_buffer = []
        glyph_buffer = []
        dictionary_tokens = []
        terminated = False

        def flush_text():
            if text_buffer:
                operations.append({'op': 'text',
                                   'value': ''.join(text_buffer)})
                text_buffer.clear()

        def flush_glyphs():
            if glyph_buffer:
                operations.append({
                    'op': 'native_glyphs',
                    'codes': ' '.join(
                        f'{code:02X}' for code in glyph_buffer),
                    'unicode': None,
                })
                glyph_buffer.clear()

        def flush():
            flush_text()
            flush_glyphs()

        def append_operation(operation):
            flush()
            operations.append(operation)

        def emit_glyph(code):
            icon = self.icon_glyphs.get(code)
            # The same code can be punctuation in the dialogue atlas and a
            # pictogram in fixed reports. Classify at the proven consumer.
            population_codes = (0x5B, 0x5C) if self.profile.get('id') == 'fr' \
                else (0x3A, 0x3B)
            speed_codes = (0x1D, 0x1C) if self.profile.get('id') == 'jp' \
                else (0x3D, 0x3C)
            if start == self.population_source and code in population_codes:
                icon = icon_part('status.population', population_codes.index(code), 2)
            elif start == self.speed_source and code in speed_codes:
                icon = icon_part('ui.speed_direction', speed_codes.index(code), 2)
            if icon is not None:
                append_operation({
                    'op': 'insert_icon',
                    'value': icon['value'],
                    'native_code': f'{code:02X}',
                    'part_index': icon['part_index'],
                    'part_count': icon['part_count'],
                    'confidence': 'mapped_semantic',
                })
                return
            character = self.glyphs.get(code)
            if character is not None:
                flush_glyphs()
                text_buffer.append(character)
            else:
                flush_text()
                glyph_buffer.append(code)

        def emit_diacritic(code):
            combining = '\u3099' if code == 0xDE else '\u309A'
            if text_buffer:
                base = text_buffer[-1]
                if len(base) == 1 and '\u3040' <= base <= '\u30FF':
                    text_buffer[-1] = unicodedata.normalize(
                        'NFC', base + combining)
                    return
            append_operation({
                'op': 'native_diacritic',
                'code': f'{code:02X}',
                'kind': 'dakuten' if code == 0xDE else 'handakuten',
                'unicode': None,
            })

        while position < limit:
            code = self.rom[position]
            position += 1

            if self.profile['encoding'] == 'dictionary-12' and code >= 0x80:
                dictionary_tokens.append(f'{code:02X}')
                for expanded in self.decoder.dictionary_bytes(
                        code, consumer='fixed'):
                    emit_glyph(expanded)
                continue
            if self.profile['encoding'] == 'direct-glyph' and \
                    code in (0xDE, 0xDF):
                emit_diacritic(code)
                continue
            if code in (0x00, 0x01):
                flush()
                operation = {
                    'op': 'end',
                    'native_cursor_after': snes_string(position),
                }
                if code == 0x01:
                    operation['variant'] = 'control_01'
                operations.append(operation)
                terminated = True
                break
            if code == 0x06:
                append_operation({'op': 'insert_master_name'})
                continue
            if code == 0x08:
                if position + 4 > limit:
                    append_operation({
                        'op': 'truncated_composer_control',
                        'code': '08',
                        'available_args_hex':
                            self.rom[position:limit].hex(' ').upper(),
                    })
                    position = limit
                    break
                arguments = self.rom[position:position + 4]
                position += 4
                args_hex = arguments.hex(' ').upper()
                semantic = self.profile.get(
                    'indexed_text_semantics', {}).get(args_hex)
                append_operation({
                    'op': 'insert_indexed_text',
                    'code': '08',
                    'args_hex': args_hex,
                    'index_address': f'${u16(arguments, 0):04X}',
                    'pointer_table': f'${u16(arguments, 2):04X}',
                    'value': semantic,
                    'confidence': ('mapped_semantic' if semantic else
                                   'unresolved_semantic'),
                })
                continue
            if code == 0x09:
                if position + 3 > limit:
                    append_operation({
                        'op': 'truncated_composer_control',
                        'code': '09',
                        'available_args_hex':
                            self.rom[position:limit].hex(' ').upper(),
                    })
                    position = limit
                    break
                arguments = self.rom[position:position + 3]
                position += 3
                native_address = f'${u16(arguments, 1):04X}'
                semantic = self.profile.get(
                    'number_semantics', {}).get(native_address)
                append_operation({
                    'op': 'format_number',
                    'width': arguments[0],
                    'native_address': native_address,
                    'value': semantic,
                    'confidence': ('mapped_semantic' if semantic else
                                   'mapped_address_only'),
                })
                continue
            if code == 0x0B:
                if position >= limit:
                    append_operation({
                        'op': 'truncated_composer_control',
                        'code': '0B',
                        'available_args_hex': '',
                    })
                    break
                count = self.rom[position]
                position += 1
                flush_glyphs()
                text_buffer.extend(' ' * count)
                continue
            if code == 0x0D:
                append_operation({'op': 'line_break'})
                continue
            if code < 0x0C:
                append_operation({
                    'op': 'composer_noop_control',
                    'code': f'{code:02X}',
                    'confidence': 'mapped_reserved',
                })
                continue
            emit_glyph(code)

        flush()
        if not terminated:
            operations.append({
                'op': 'truncated_composer_record',
                'native_cursor_after': snes_string(position),
            })
        return {
            'operations': operations,
            'end': position,
            'terminated': terminated,
            'dictionary_tokens': dictionary_tokens,
        }


def visible_units(operations):
    total = 0
    for operation in operations:
        if operation['op'] == 'text':
            total += sum(not character.isspace() for character in operation['value'])
        elif operation['op'] == 'native_glyphs':
            total += len(operation['codes'].split())
        elif operation['op'] == 'insert_icon':
            total += 1
    return total


def make_message(profile, rom, decoder, start, category, semantic_ids,
                 alignment='positional_unverified', limit=None,
                 discovery='decoded_structure', stop_on_yield=True):
    decoded = decoder.decode_record(start, limit, stop_on_yield)
    raw = rom[start:decoded['end']]
    message = {
        'id': native_id(profile, start),
        'candidate_semantic_ids': list(semantic_ids),
        'alignment_status': alignment,
        'category': category,
        'discovery': discovery,
        'source': {
            'file_offset': f'0x{start:06X}',
            'snes': snes_string(start),
            'end_file_offset_exclusive': f"0x{decoded['end']:06X}",
            'byte_count': len(raw),
            'raw_sha256': hashlib.sha256(raw).hexdigest(),
        },
        'terminated': decoded['terminated'],
        'operations': decoded['operations'],
    }
    if decoded['dictionary_tokens']:
        message['source_dictionary_tokens'] = decoded['dictionary_tokens']
    return message, decoded['end']


def add_message(messages, by_offset, profile, rom, decoder, start, category,
                semantic_id, alignment='positional_unverified', limit=None,
                discovery='decoded_structure', stop_on_yield=True):
    if start in by_offset:
        message = by_offset[start]
        if semantic_id not in message['candidate_semantic_ids']:
            message['candidate_semantic_ids'].append(semantic_id)
        if category != message['category']:
            aliases = message.setdefault('category_aliases', [])
            if category not in aliases:
                aliases.append(category)
        return message, int(message['source']['end_file_offset_exclusive'], 16)
    message, end = make_message(
        profile, rom, decoder, start, category, [semantic_id], alignment,
        limit, discovery, stop_on_yield)
    messages.append(message)
    by_offset[start] = message
    return message, end


def add_sequential_block(messages, by_offset, profile, rom, decoder, start,
                         end, category, semantic_prefix):
    position = start
    index = 0
    starts = []
    while position < end:
        message, next_position = add_message(
            messages, by_offset, profile, rom, decoder, position, category,
            f'{semantic_prefix}.{index:03d}', limit=end)
        if not message['terminated']:
            raise ValueError(
                f'{category} record at {snes_string(position)} reaches '
                f'{snes_string(end)} without an end/yield control')
        starts.append(position)
        if next_position <= position:
            raise ValueError(f'parser did not advance at {snes_string(position)}')
        position = next_position
        index += 1
    if position != end:
        raise ValueError(
            f'{category} block overran {snes_string(end)} at {snes_string(position)}')
    return starts


def add_pointer_set(messages, by_offset, profile, rom, decoder, table_offset,
                    count, category, semantic_prefix, allowed_end=None):
    slots = []
    ends = []
    for index in range(count):
        pointer = u16(rom, table_offset + index * 2)
        target = pointer_to_offset(table_offset, pointer)
        slot = {
            'slot': index,
            'candidate_semantic_id': f'{semantic_prefix}.{index:02d}',
            'native_pointer': f'${pointer:04X}',
            'target_file_offset': f'0x{target:06X}',
        }
        if allowed_end is not None and target >= allowed_end:
            slot['target_id'] = None
            slot['classification'] = 'sentinel_or_next_table'
        else:
            message, end = add_message(
                messages, by_offset, profile, rom, decoder, target, category,
                slot['candidate_semantic_id'], limit=allowed_end,
                stop_on_yield=True)
            slot['target_id'] = message['id']
            ends.append(end)
        slots.append(slot)
    return {
        'id': f'native.{profile["id"]}.{semantic_prefix}',
        'source_table': {
            'file_offset': f'0x{table_offset:06X}',
            'snes': snes_string(table_offset),
            'pointer_count': count,
        },
        'slots': slots,
    }, ends


def add_name_sets(messages, by_offset, profile, rom, decoder, town_table,
                  enemy_table):
    pointer_sets = []
    towns, _ = add_pointer_set(
        messages, by_offset, profile, rom, decoder, town_table, 7,
        'town_name', 'town_name')
    enemies, _ = add_pointer_set(
        messages, by_offset, profile, rom, decoder, enemy_table, 4,
        'enemy_name', 'enemy_name')
    pointer_sets.extend((towns, enemies))

    # Slot zero is a non-town sentinel/alias (Eldorado in Japan, a Fillmore
    # duplicate in the Latin releases). The gameplay selector uses 1..6.
    for slot, semantic_id in zip(towns['slots'][1:], CITY_TERM_IDS[:6]):
        verify_message_semantic(
            by_offset[int(slot['target_file_offset'], 16)], semantic_id)
    for slot, semantic_id in zip(enemies['slots'], ENEMY_TERM_IDS):
        verify_message_semantic(
            by_offset[int(slot['target_file_offset'], 16)], semantic_id)
    return pointer_sets


def verify_message_semantic(message, semantic_id):
    existing = message.get('verified_semantic_id')
    if existing and existing != semantic_id:
        raise ValueError(
            f"{message.get('id')}: conflicting verified semantics "
            f'{existing!r} and {semantic_id!r}')
    message['verified_semantic_id'] = semantic_id
    message['alignment_status'] = 'callsite_verified'
    if semantic_id not in message['candidate_semantic_ids']:
        message['candidate_semantic_ids'].insert(0, semantic_id)


def add_dynamic_lookup_sets(messages, by_offset, pointer_sets, profile, rom,
                            decoder):
    """Extract every language-bearing target of a typed runtime lookup."""
    rows = []
    for table_id, table_offset, semantic_ids in profile.get(
            'lookup_source_tables', ()):
        pointer_set, _ = add_pointer_set(
            messages, by_offset, profile, rom, decoder,
            table_offset, len(semantic_ids), 'dynamic_lookup_text',
            f'lookup.{table_id}')
        pointer_sets.append(pointer_set)
        for slot, semantic_id in zip(pointer_set['slots'], semantic_ids):
            verify_message_semantic(
                by_offset[int(slot['target_file_offset'], 16)], semantic_id)
        rows.append({
            'id': table_id,
            'source_table': pointer_set['source_table'],
            'target_count': len(semantic_ids),
            'semantic_ids': list(semantic_ids),
        })
    return rows


def add_ui_families(messages, by_offset, pointer_sets, profile, rom, decoder):
    stage_names, _ = add_pointer_set(
        messages, by_offset, profile, rom, decoder,
        profile['action_stage_name_table'], 7,
        'action_stage_name', 'action.stage_name')
    pointer_sets.append(stage_names)
    for slot, semantic_id in zip(stage_names['slots'], CITY_TERM_IDS):
        verify_message_semantic(
            by_offset[int(slot['target_file_offset'], 16)], semantic_id)
    title_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['title_text_start'], profile['title_text_end'],
        'title_and_mode_menu', 'title_menu')
    action_label_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['action_label_start'], profile['action_label_end'],
        'action_hud_label', 'action.hud')
    for start, semantic_id in zip(action_label_starts, ACTION_HUD_IDS):
        if semantic_id:
            verify_message_semantic(by_offset[start], semantic_id)
    sound_test_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['sound_test_start'], profile['sound_test_end'],
        'sound_test_menu', 'sound_test')
    return {
        'action_stage_name_pointer_slots': 7,
        'title_and_mode_menu_records': len(title_starts),
        'action_hud_label_records': len(action_label_starts),
        'sound_test_menu_records': len(sound_test_starts),
    }


def pc24_from_string(value):
    return int(value[1:3], 16) << 16 | int(value[4:], 16)


def fixed_composer_catalog(profile, rom, decoder, consumer_census):
    """Decode only proven fixed-composer roots with its native grammar."""
    sources = {}

    def add_source(address_text, candidate_id, owner, classification):
        row = sources.setdefault(address_text, {
            'candidate_semantic_ids': [],
            'owners': [],
            'classification': classification,
        })
        if candidate_id not in row['candidate_semantic_ids']:
            row['candidate_semantic_ids'].append(candidate_id)
        if owner not in row['owners']:
            row['owners'].append(owner)
        if row['classification'] != classification:
            raise ValueError(
                f'{address_text}: conflicting composer classifications')

    composer_sources = consumer_census['fixed_composer_sources']
    for table in composer_sources['pointer_tables']:
        for index, target in enumerate(table['target_pc24s']):
            add_source(
                target, f'menu.{table["id"]}.{index:02d}',
                f'pointer_table:{table["id"]}', 'fixed_composer_text')
    for row in composer_sources['direct_sources']:
        add_source(
            row['source_pc24'], f'menu.{row["id"]}',
            f'direct_source:{row["id"]}', 'fixed_composer_text')
    for row in composer_sources['indexed_direct_sources']:
        add_source(
            row['source_pc24'], f'menu.{row["id"]}',
            f'indexed_source:{row["id"]}', 'fixed_composer_text')
        for target in row['targets']:
            add_source(
                target['source_pc24'], f'menu.{target["id"]}',
                f'indexed_target:{row["id"]}', 'fixed_composer_text')
    for row in composer_sources['dynamic_reports']:
        add_source(
            row['source_pc24'], f'status.{row["id"]}',
            f'dynamic_report:{row["id"]}', 'typed_dynamic_text')
    numeric = composer_sources['numeric_only_source']
    if numeric is not None:
        add_source(
            numeric['source_pc24'], 'native.numeric_only',
            'numeric_only_descriptor', 'typed_numeric_only')
    for row in composer_sources['name_entry_sources']:
        add_source(
            row['source_pc24'], f'name_entry.{row["id"]}',
            f'name_entry:{row["id"]}', 'fixed_composer_text')
    for row in composer_sources.get('flow_sources', ()):
        add_source(
            row['source_pc24'], f'system.{row["id"]}',
            f'stateful_flow:{row["id"]}', row['classification'])

    composer_decoder = FixedComposerDecoder(decoder)
    segments = []
    start = profile['menu_start']
    end = profile['menu_end']
    for address_text, identity in sorted(
            sources.items(), key=lambda item: pc24_to_offset(
                pc24_from_string(item[0]))):
        position = pc24_to_offset(pc24_from_string(address_text))
        decoded = composer_decoder.decode_record(position, end)
        if not decoded['terminated']:
            raise ValueError(
                f'fixed composer record at {address_text} has no terminator')
        raw = rom[position:decoded['end']]
        operations = decoded['operations']
        if identity['classification'] == 'typed_non_language_indicator':
            operations = [{
                'op': 'insert_icon',
                'value': 'message_speed_selector',
                'part_index': 0,
                'part_count': 1,
                'confidence': 'mapped_semantic',
            }, operations[-1]]
        segment = {
            'id': native_id(profile, position),
            'candidate_semantic_ids': sorted(
                identity['candidate_semantic_ids']),
            'alignment_status': 'role_verified_address_identity_pending',
            'classification': identity['classification'],
            'confidence': 'consumer_source_and_grammar_verified',
            'owners': sorted(identity['owners']),
            'source': {
                'file_offset': f'0x{position:06X}',
                'snes': snes_string(position),
                'end_file_offset_exclusive':
                    f"0x{decoded['end']:06X}",
                'byte_count': len(raw),
                'raw_sha256': hashlib.sha256(raw).hexdigest(),
            },
            'terminated': True,
            'visible_units': visible_units(operations),
            'operations': operations,
        }
        if decoded['dictionary_tokens']:
            segment['source_dictionary_tokens'] = decoded[
                'dictionary_tokens']
        segments.append(segment)

    return {
        'status': 'exact_consumer_roots_censused_semantic_ids_pending',
        'warning': (
            'Only call-flow/table-proven fixed-composer roots are decoded. '
            'Interleaved pointer, coordinate, and code bytes are deliberately '
            'not presented as text; the enclosing mixed range still requires '
            'whole-ROM byte ownership before the extraction gate can close.'),
        'source_range': {
            'start_file_offset': f'0x{start:06X}',
            'end_file_offset_exclusive': f'0x{end:06X}',
            'start_snes': snes_string(start),
            'byte_count': end - start,
        },
        'proven_source_count': len(segments),
        'segments': segments,
    }


def operation_findings(records):
    """Summarize operations that still need semantic or Unicode ownership.

    Counts are deliberately separated by operation and confidence. A preserved
    native glyph is lossless extraction evidence, but it is not proof that the
    source is ready for a Unicode author pack.
    """
    counts = {}
    for record in records:
        for operation in record.get('operations', []):
            op = operation.get('op')
            confidence = operation.get('confidence')
            unresolved = ((confidence or '').startswith('unresolved') or
                          op in ('native_control', 'native_glyphs',
                                 'native_diacritic',
                                 'truncated_native_control',
                                 'truncated_composer_control',
                                 'truncated_composer_record') or
                          (op in ('format_number', 'insert_indexed_text') and
                           operation.get('value') is None))
            if not unresolved:
                continue
            key = (op or 'missing_operation', confidence or 'not_declared')
            counts[key] = counts.get(key, 0) + 1
    return [
        {'operation': operation, 'confidence': confidence, 'count': count}
        for (operation, confidence), count in sorted(counts.items())
    ]


def build_dynamic_text_census(profile, messages, menu, inventory):
    """Prove that each observed runtime substitution has a typed value."""
    records = list(messages) + list(menu['segments'])
    value_counts = {}
    operation_counts = {}
    unresolved = []
    for record in records:
        for operation in record.get('operations', []):
            op = operation.get('op')
            if op not in ('format_number', 'insert_indexed_text',
                          'insert_master_name'):
                continue
            operation_counts[op] = operation_counts.get(op, 0) + 1
            value = ('master_name' if op == 'insert_master_name' else
                     operation.get('value'))
            if value is None:
                unresolved.append({
                    'record_id': record['id'],
                    'operation': op,
                    'native_address': operation.get('native_address'),
                    'args_hex': operation.get('args_hex'),
                })
                continue
            value_counts[value] = value_counts.get(value, 0) + 1

    lookup_tables = inventory.get('dynamic_lookup_tables', [])
    lookup_semantics = {
        semantic_id for table in lookup_tables
        for semantic_id in table['semantic_ids']
    }
    message_semantics = {
        message.get('verified_semantic_id') for message in messages
        if message.get('verified_semantic_id')
    }
    missing_lookup_targets = sorted(lookup_semantics - message_semantics)
    complete = not unresolved and not missing_lookup_targets
    return {
        'status': ('all_observed_substitutions_typed' if complete else
                   'unresolved_dynamic_substitutions'),
        'complete': complete,
        'operation_counts': dict(sorted(operation_counts.items())),
        'typed_value_count': len(value_counts),
        'typed_value_use_counts': dict(sorted(value_counts.items())),
        'declared_number_address_count': len(profile.get(
            'number_semantics', {})),
        'declared_indexed_lookup_count': len(profile.get(
            'indexed_text_semantics', {})),
        'language_lookup_table_count': len(lookup_tables),
        'language_lookup_target_count': sum(
            table['target_count'] for table in lookup_tables),
        'unresolved_operation_count': len(unresolved),
        'unresolved_operations': unresolved,
        'missing_lookup_target_semantic_ids': missing_lookup_targets,
        'native_numeric_formats': {
            'ordinary': 'unsigned_decimal_from_16_bit_value',
            'score_84': 'packed_bcd_4_digits_plus_trailing_zero',
            'score_86': 'packed_bcd_6_digits_plus_trailing_zero',
        },
    }


def merge_source_intervals(intervals):
    """Return sorted, non-overlapping half-open intervals."""
    merged = []
    for start, end in sorted(intervals):
        if end < start:
            raise ValueError('source interval ends before it starts')
        if not merged or start > merged[-1][1]:
            merged.append([start, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return [tuple(interval) for interval in merged]


def interval_byte_count(intervals):
    return sum(end - start for start, end in merge_source_intervals(intervals))


def record_has_visible_language(record):
    return visible_units(record.get('operations', ())) > 0 or any(
        operation.get('op') in (
            'insert_master_name', 'insert_indexed_text', 'format_number')
        for operation in record.get('operations', ()))


def build_language_source_ownership(profile, rom, messages, pointer_sets,
                                    menu, consumer_census):
    """Classify every decoded source under an exhaustive consumer closure.

    Compressed dictionary bytes and Japanese tile codes make a whole-ROM
    "printable byte" scan actively misleading: ordinary code and graphics
    decode into plausible glyphs.  The sound proof runs in the other
    direction.  First census every path capable of producing text or writing
    its destination, then require every source reachable by those paths to
    resolve to a decoded record.  Bounded retail catalogues retain any
    unreferenced records as dormant/release resources rather than dropping
    them or inventing a live semantic route.
    """
    resolution = consumer_census.get('source_reference_resolution', {})
    references = consumer_census['source_reference_seeds']['references']
    live_record_ids = {
        reference['resolved_record_id'] for reference in references
        if reference.get('resolved_record_id') is not None
    }

    text_intervals = []
    class_counts = {}
    unclassified = []
    classified_records = []
    bounded_ui_categories = {
        'action_stage_name', 'action_hud_label', 'title_and_mode_menu',
        'sound_test_menu', 'town_name', 'enemy_name',
        'dynamic_lookup_text', 'offering_text', 'offering_text_native',
        'ending_text',
    }
    dormant_categories = {
        'angel_dialogue', 'angel_dialogue_native',
        'town_dialogue', 'town_dialogue_native',
        'post_offering_or_ending_native', 'dialogue_consumer_seed',
    }

    for record in list(messages) + list(menu['segments']):
        source = record['source']
        start = int(source['file_offset'], 16)
        end = int(source['end_file_offset_exclusive'], 16)
        text_intervals.append((start, end))
        category = record.get('category')
        if record['id'] in live_record_ids:
            ownership = 'live_consumer_text'
        elif not record_has_visible_language(record):
            ownership = 'verified_non_language_empty_record'
        elif record.get('classification') in (
                'typed_numeric_only', 'typed_non_language_indicator'):
            ownership = ('verified_non_language_numeric_descriptor'
                         if record.get('classification') ==
                         'typed_numeric_only' else
                         'verified_non_language_ui_indicator')
        elif category in bounded_ui_categories:
            ownership = 'bounded_consumer_catalog_text'
        elif category in dormant_categories:
            ownership = 'dormant_or_release_variant_text'
        elif record in menu['segments']:
            # Fixed-composer segments are admitted only from a proven table,
            # direct source, dynamic report, or name-entry source.
            ownership = 'bounded_consumer_catalog_text'
        else:
            ownership = 'unclassified'
            unclassified.append(record['id'])
        record['source_ownership'] = ownership
        class_counts[ownership] = class_counts.get(ownership, 0) + 1
        classified_records.append({
            'record_id': record['id'],
            'source_ownership': ownership,
            'file_offset': source['file_offset'],
            'end_file_offset_exclusive': source[
                'end_file_offset_exclusive'],
        })

    metadata_intervals = []
    metadata_rows = []

    def add_metadata(metadata_id, start, byte_count, classification):
        metadata_intervals.append((start, start + byte_count))
        metadata_rows.append({
            'id': metadata_id,
            'classification': classification,
            'file_offset': f'0x{start:06X}',
            'byte_count': byte_count,
        })

    for pointer_set in pointer_sets:
        table = pointer_set['source_table']
        add_metadata(
            pointer_set['id'], int(table['file_offset'], 16),
            table['pointer_count'] * 2, 'language_source_pointer_table')

    nested = consumer_census['nested_handler_sources']
    for matrix_index, matrix in enumerate(nested['matrices']):
        matrix_start = pc24_to_offset(pc24_from_string(
            matrix['row_table_pc24']))
        add_metadata(
            f'nested_handler.{matrix_index}.row_table', matrix_start,
            matrix['row_count'] * 2, 'language_source_pointer_table')
        for row in matrix['rows']:
            add_metadata(
                f'nested_handler.{matrix_index}.city.{row["city_slot"]}',
                pc24_to_offset(pc24_from_string(row['source_table_pc24'])),
                row['pointer_count'] * 2,
                'language_source_pointer_table')

    composer = consumer_census['fixed_composer_sources']
    for row in composer['pointer_tables']:
        add_metadata(
            f'fixed_composer.{row["id"]}',
            pc24_to_offset(pc24_from_string(row['source_table_pc24'])),
            row['pointer_count'] * 2, 'language_source_pointer_table')
    direct_table = composer.get('direct_source_table')
    if direct_table is not None:
        add_metadata(
            'fixed_composer.direct_sources',
            pc24_to_offset(pc24_from_string(
                direct_table['source_table_pc24'])),
            direct_table['pointer_count'] * 2,
            'language_source_pointer_table')
    for row in composer['indexed_direct_sources']:
        add_metadata(
            f'fixed_composer.indexed.{row["id"]}',
            pc24_to_offset(pc24_from_string(row['source_table_pc24'])),
            row['pointer_count'] * 2, 'language_source_pointer_table')
    for row in composer.get('flow_sources', ()):
        descriptor = row.get('descriptor_pc24')
        if descriptor is not None:
            add_metadata(
                f'fixed_composer.flow.{row["id"]}.destination',
                pc24_to_offset(pc24_from_string(descriptor)), 2,
                'fixed_composer_destination_descriptor')

    if profile['encoding'] == 'dictionary-12':
        add_metadata(
            'dictionary', profile['dictionary'],
            DICTIONARY_ENTRY_COUNT * DICTIONARY_ENTRY_BYTES,
            'language_decoder_dictionary')

    consumer_complete = consumer_census.get(
        'whole_game_consumer_discovery_complete', False)
    all_references_mapped = resolution.get(
        'all_current_seeds_mapped', False)
    complete = consumer_complete and all_references_mapped and not unclassified
    owned_intervals = text_intervals + metadata_intervals
    return {
        'status': ('complete_consumer_rooted_source_ownership'
                   if complete else 'incomplete_source_ownership'),
        'complete': complete,
        'method': 'exhaustive_text_destination_consumers_then_source_closure',
        'raw_printable_scan_rejected': True,
        'raw_printable_scan_reason': (
            'Dictionary tokens and direct tile codes make arbitrary ROM data '
            'decode as plausible text; byte-likeness cannot establish '
            'language ownership.'),
        'proof_obligations': {
            'whole_game_consumer_discovery_complete': consumer_complete,
            'all_consumer_source_references_mapped': all_references_mapped,
            'all_bounded_source_records_classified': not unclassified,
            'graphical_language_resources_deferred_to_separate_census': True,
        },
        'record_count': len(classified_records),
        'record_counts_by_ownership': dict(sorted(class_counts.items())),
        'unclassified_record_count': len(unclassified),
        'unclassified_record_ids': sorted(unclassified),
        'consumer_reference_count': len(references),
        'unique_consumer_source_count': resolution.get(
            'unique_source_count', 0),
        'mapped_unique_consumer_source_count': resolution.get(
            'mapped_unique_source_count', 0),
        'text_source_interval_count': len(merge_source_intervals(
            text_intervals)),
        'text_source_byte_count': interval_byte_count(text_intervals),
        'metadata_interval_count': len(merge_source_intervals(
            metadata_intervals)),
        'metadata_byte_count': interval_byte_count(metadata_intervals),
        'owned_rom_byte_count': interval_byte_count(owned_intervals),
        'outside_consumer_source_ownership_byte_count': (
            len(rom) - interval_byte_count(owned_intervals)),
        'outside_ownership_classification': (
            'not_reachable_as_language_source_by_any_censused_live_text_path; '
            'graphical assets are classified by graphical_text_census'),
        'metadata': metadata_rows,
        'records': classified_records,
    }


def pointer_slot_route_id(slot):
    candidate = slot['candidate_semantic_id']
    prefix, _, suffix = candidate.rpartition('.')
    index = int(suffix)
    if prefix == 'town_name':
        return ('town.name.table.sentinel' if index == 0 else
                f'town.name.{CITY_KEYS[index - 1]}')
    if prefix == 'enemy_name':
        return f'enemy.name.slot_{index:02d}'
    if prefix == 'action.stage_name':
        return f'action.stage_name.{CITY_TERM_IDS[index].split(".")[1]}'
    if prefix == 'offering':
        return f'dialogue.offering.slot_{index:02d}'
    if prefix == 'ending':
        return f'dialogue.ending.slot_{index:02d}'
    if prefix.startswith('lookup.'):
        return candidate[len('lookup.'):]
    return f'resource.pointer.{prefix}.slot_{index:02d}'


def fixed_composer_route_id(reference_kind, occurrence):
    pointer_prefix = 'fixed_composer_'
    pointer_suffix = '_pointer'
    if reference_kind.startswith(pointer_prefix) and \
            reference_kind.endswith(pointer_suffix):
        table_id = reference_kind[
            len(pointer_prefix):-len(pointer_suffix)]
        semantic_ids = COMPOSER_POINTER_ROUTE_IDS.get(table_id)
        if semantic_ids is None or occurrence >= len(semantic_ids):
            return None
        return semantic_ids[occurrence]
    direct_prefix = 'fixed_composer_direct_'
    if reference_kind.startswith(direct_prefix):
        return ('sim.menu.offering_action.' +
                reference_kind[len(direct_prefix):])
    indexed_prefix = 'fixed_composer_indexed_'
    if reference_kind.startswith(indexed_prefix):
        source_id = reference_kind[len(indexed_prefix):]
        return ('sim.menu.offering_action.selected' if
                source_id == 'offering_action' else
                f'sim.menu.offering_action.{source_id}')
    dynamic_prefix = 'fixed_composer_dynamic_'
    if reference_kind.startswith(dynamic_prefix):
        return 'status.report.' + reference_kind[len(dynamic_prefix):]
    name_prefix = 'fixed_composer_name_entry_'
    if reference_kind.startswith(name_prefix):
        return 'name_entry.' + reference_kind[len(name_prefix):]
    flow_prefix = 'fixed_composer_flow_'
    if reference_kind.startswith(flow_prefix):
        flow_id = reference_kind[len(flow_prefix):]
        return {
            'choice_labels': 'system.choice.yes_no',
            'message_speed_scale_labels':
                'system.message_speed.scale_labels',
        }.get(flow_id)
    return None


def build_semantic_route_catalog(profile, messages, pointer_sets, menu,
                                 consumer_census, decoder=None):
    """Build logical routes without treating physical record order as identity."""
    records = list(messages) + list(menu['segments'])
    records_by_id = {record['id']: record for record in records}
    routes = {}

    def add_route(route_id, record_id, source_offset, route_kind,
                  provenance, regional=False):
        if record_id not in records_by_id:
            raise ValueError(f'{profile["id"]}: route has unknown record')
        candidate = {
            'id': route_id,
            'source_record_id': record_id,
            'source_offset_within_record': source_offset,
            'source_category': records_by_id[record_id].get(
                'category', records_by_id[record_id].get(
                    'classification', 'unclassified')),
            'route_kind': route_kind,
            'availability': ('release_variant' if regional else
                             'cross_release_candidate'),
            'provenance': [provenance],
        }
        existing = routes.get(route_id)
        if existing is None:
            routes[route_id] = candidate
        elif (existing['source_record_id'],
              existing['source_offset_within_record']) != \
                (record_id, source_offset):
            raise ValueError(
                f'{profile["id"]}: route {route_id} has multiple sources')
        elif provenance not in existing['provenance']:
            existing['provenance'].append(provenance)

    # Pointer tables already preserve slot identity even when several slots
    # alias one physical record.
    offering_slots = []
    for pointer_set in pointer_sets:
        for slot in pointer_set['slots']:
            target_id = slot.get('target_id')
            if target_id is None:
                continue
            route_id = pointer_slot_route_id(slot)
            add_route(
                route_id, target_id, 0, 'pointer_table_slot',
                f'{pointer_set["id"]}.slot_{slot["slot"]:02d}',
                route_id.startswith('resource.'))
            if slot['candidate_semantic_id'].startswith('offering.'):
                offering_slots.append((slot['slot'], route_id))

    wrappers = consumer_census['dialogue_forwarding']['wrappers']
    wrapper_calls = {}
    for wrapper_index, wrapper in enumerate(wrappers):
        for call_index, call in enumerate(wrapper['call_sites']):
            wrapper_calls[call['call_site']] = (wrapper_index, call_index)

    nested_tables = {}
    for matrix in consumer_census['nested_handler_sources']['matrices']:
        for row in matrix['rows']:
            nested_tables[row['source_table_pc24']] = row['city_slot']

    references = consumer_census['source_reference_seeds']['references']
    reference_occurrences = {}
    wrapper_alternatives = {}
    wrapper_six_route = 0
    relay_route = 0
    unknown_references = []
    for reference_index, reference in enumerate(references):
        kind = reference['reference_kind']
        record_id = reference.get('resolved_record_id')
        if record_id is None:
            unknown_references.append(reference_index)
            continue
        source_offset = reference.get('source_offset_within_record', 0)
        provenance = (
            reference.get('via_call_site') or
            reference.get('via_source_table') or
            reference.get('via_consumer_entry'))
        route_id = None
        regional = False

        if kind == 'interpreter_nested_handler_pointer':
            table = reference['via_source_table']
            occurrence = reference_occurrences.get((kind, table), 0)
            reference_occurrences[(kind, table)] = occurrence + 1
            city_slot = nested_tables.get(table)
            if city_slot is not None and occurrence < HANDLER_SLOTS_PER_CITY:
                route_id = (
                    f'simulation.event.{CITY_KEYS[city_slot]}.'
                    f'slot_{occurrence:02d}')
        elif kind == 'interpreter_offering_pointer_table':
            occurrence = reference_occurrences.get((kind, provenance), 0)
            reference_occurrences[(kind, provenance)] = occurrence + 1
            if occurrence < len(offering_slots):
                route_id = offering_slots[occurrence][1]
        elif kind.startswith('interpreter_'):
            call = pc24_from_string(reference['via_call_site'])
            specifications = (JAPANESE_INTERACTIVE_ROUTE_IDS
                              if profile['id'] == 'jp' else
                              LATIN_INTERACTIVE_ROUTE_IDS)
            specification = specifications.get(call)
            occurrence = reference_occurrences.get((kind, provenance), 0)
            reference_occurrences[(kind, provenance)] = occurrence + 1
            if isinstance(specification, tuple):
                if occurrence < len(specification):
                    route_id = specification[occurrence]
            elif occurrence == 0:
                route_id = specification
            regional = bool(route_id and route_id.startswith('variant.'))
        elif kind.startswith('dialogue_wrapper_relay_'):
            if relay_route < len(CITY_KEYS):
                route_id = f'dialogue.event.relay.{CITY_KEYS[relay_route]}'
            relay_route += 1
        elif kind.startswith('dialogue_wrapper_'):
            call_site = reference['via_call_site']
            wrapper_position = wrapper_calls.get(call_site)
            if wrapper_position is not None:
                wrapper_index, call_index = wrapper_position
                alternative_key = (wrapper_index, call_index)
                alternative = wrapper_alternatives.get(alternative_key, 0)
                wrapper_alternatives[alternative_key] = alternative + 1
                if wrapper_index == 6:
                    route_id = (
                        f'dialogue.event.wrapper_06.route_'
                        f'{wrapper_six_route:02d}')
                    wrapper_six_route += 1
                else:
                    normalized_call = call_index
                    if wrapper_index == 0 and profile['id'] == 'jp':
                        if call_index == 2:
                            route_id = (
                                'variant.jp.dialogue.event.wrapper_00.'
                                'extra_call_02')
                            regional = True
                        elif call_index > 2:
                            normalized_call -= 1
                    if route_id is None:
                        route_id = (
                            f'dialogue.event.wrapper_{wrapper_index:02d}.'
                            f'call_{normalized_call:02d}.'
                            f'source_{alternative:02d}')
        elif kind.startswith('fixed_composer_'):
            occurrence_key = (kind, provenance)
            occurrence = reference_occurrences.get(occurrence_key, 0)
            reference_occurrences[occurrence_key] = occurrence + 1
            route_id = fixed_composer_route_id(kind, occurrence)

        if route_id is None:
            unknown_references.append(reference_index)
            continue
        add_route(route_id, record_id, source_offset, 'consumer_route',
                  f'{kind}:{provenance}', regional)

    # Bounded sequential UI resources are not represented by immediate source
    # references, but their enclosing composer/render flow is verified.
    ui_indices = {}
    for record in sorted(records, key=lambda item: int(
            item['source']['file_offset'], 16)):
        category = record.get('category')
        index = ui_indices.get(category, 0)
        ui_indices[category] = index + 1
        route_id = None
        regional = False
        if category == 'title_and_mode_menu':
            title_key = (profile['id'] if profile['id'] in ('us', 'jp')
                         else 'regional_mode_select')
            title_ids = TITLE_ROUTE_IDS[title_key]
            if index < len(title_ids):
                route_id = title_ids[index]
                regional = title_key == 'regional_mode_select' or \
                    route_id.endswith(('professional', 'special'))
        elif category == 'action_hud_label' and index < len(ACTION_HUD_IDS):
            route_id = ACTION_HUD_IDS[index]
        elif category == 'sound_test_menu' and index == 0:
            route_id = 'sound_test.menu.labels'
        if route_id is not None:
            add_route(route_id, record['id'], 0, 'bounded_ui_resource',
                      f'{category}.record_{index:02d}', regional)

    routed_records = {
        route['source_record_id'] for route in routes.values()
    }
    dormant_count = 0
    dormant_category_counts = {}
    unclassified_records = []
    for record in records:
        record_routes = sorted(
            route_id for route_id, route in routes.items()
            if route['source_record_id'] == record['id'])
        if record_routes:
            record['semantic_route_ids'] = record_routes
            record['alignment_status'] = 'logical_routes_verified'
        elif not record_has_visible_language(record):
            record['semantic_route_ids'] = []
            record['alignment_status'] = 'verified_non_language_resource'
        elif record.get('source_ownership', '').startswith(
                'verified_non_language_'):
            record['semantic_route_ids'] = []
            record['alignment_status'] = 'verified_non_language_resource'
        elif record.get('source_ownership') == \
                'dormant_or_release_variant_text':
            category = record.get('category', 'text')
            dormant_index = dormant_category_counts.get(category, 0)
            dormant_category_counts[category] = dormant_index + 1
            route_id = (
                f'variant.{profile["id"]}.dormant.'
                f'{category}.resource_{dormant_index:02d}')
            add_route(route_id, record['id'], 0, 'dormant_retail_resource',
                      'decoded_bounded_resource_without_live_reference', True)
            record['semantic_route_ids'] = [route_id]
            record['alignment_status'] = 'explicit_release_variant_resource'
            dormant_count += 1
        else:
            unclassified_records.append(record['id'])

    # A logical route can enter a shared physical record after an earlier
    # prefix. Preserve the exact invocation-visible operation stream so the
    # authoring exporter never has to guess by slicing another route's text.
    # Fixed-composer routes currently begin at their record start; the six
    # non-zero USA offsets (and regional counterparts) are interpreter inputs.
    menu_record_ids = {record['id'] for record in menu['segments']}
    for route in routes.values():
        record = records_by_id[route['source_record_id']]
        source_offset = route['source_offset_within_record']
        if source_offset:
            if decoder is None:
                continue
            if record['id'] in menu_record_ids:
                raise ValueError(
                    f'{profile["id"]}: fixed-composer route enters inside '
                    f'{record["id"]}')
            source_start = int(record['source']['file_offset'], 16)
            source_end = int(
                record['source']['end_file_offset_exclusive'], 16)
            decoded = decoder.decode_record(
                source_start + source_offset, source_end,
                stop_on_yield=True)
            if not decoded['terminated']:
                raise ValueError(
                    f'{profile["id"]}: route {route["id"]} has no '
                    'invocation boundary')
            operations = decoded['operations']
        else:
            operations = record['operations']
        serialized = json.dumps(
            operations, ensure_ascii=False, sort_keys=True,
            separators=(',', ':')).encode('utf-8')
        route['source_operations'] = operations
        route['source_operation_count'] = len(operations)
        route['source_operations_sha256'] = hashlib.sha256(
            serialized).hexdigest()

    complete = not unknown_references and not unclassified_records
    return {
        'status': ('all_logical_routes_locally_verified' if complete else
                   'unresolved_logical_routes'),
        'complete': complete,
        'identity_unit': 'logical_consumer_route_not_physical_record_ordinal',
        'route_count': len(routes),
        'routed_record_count': len(routed_records),
        'dormant_release_resource_count': dormant_count,
        'unresolved_consumer_reference_count': len(unknown_references),
        'unresolved_consumer_reference_indices': unknown_references,
        'unclassified_language_record_count': len(unclassified_records),
        'unclassified_language_record_ids': sorted(unclassified_records),
        'routes': [routes[route_id] for route_id in sorted(routes)],
    }


def apply_cross_release_semantic_alignment(extractions):
    """Join locally verified logical routes across all supported releases."""
    expected = ('us', 'eu-en', 'de', 'fr', 'jp')
    by_release = {profile['id']: pack for profile, pack in extractions}
    missing = [release_id for release_id in expected
               if release_id not in by_release]
    route_releases = {}
    for release_id, pack in by_release.items():
        catalog = pack['semantic_route_catalog']
        for route in catalog['routes']:
            route_releases.setdefault(route['id'], []).append(release_id)
    rows = []
    for route_id in sorted(route_releases):
        releases = sorted(
            route_releases[route_id], key=lambda item: expected.index(item))
        rows.append({
            'semantic_route_id': route_id,
            'available_releases': releases,
            'availability': ('all_supported_releases'
                             if tuple(releases) == expected else
                             'regional_or_release_variant'),
        })
    complete = not missing and all(
        pack['semantic_route_catalog']['complete']
        for pack in by_release.values())
    result = {
        'status': ('complete_logical_route_union' if complete else
                   'incomplete_logical_route_union'),
        'complete': complete,
        'identity_unit': 'logical_consumer_route_not_physical_record_ordinal',
        'expected_releases': list(expected),
        'present_releases': [release_id for release_id in expected
                             if release_id in by_release],
        'missing_releases': missing,
        'semantic_route_count': len(rows),
        'all_release_route_count': sum(
            row['availability'] == 'all_supported_releases' for row in rows),
        'regional_or_release_variant_route_count': sum(
            row['availability'] == 'regional_or_release_variant'
            for row in rows),
        'routes': rows,
    }
    for pack in by_release.values():
        pack['cross_release_semantic_alignment'] = {
            key: value for key, value in result.items() if key != 'routes'
        }
    return result


def build_coverage_report(profile, source, messages, pointer_sets, menu,
                          consumer_census=None, dynamic_census=None,
                          source_ownership=None, semantic_alignment=None,
                          graphical_census=None):
    """Build an honest whole-game extraction gate without retail wording.

    The first recovered extractor knows several bounded text families, but it
    does not yet prove that every consumer, dynamic composer, or graphical
    label in the ROM has been found. Keep those missing proof obligations
    explicit so a large decoded-message count cannot be mistaken for 100%
    language coverage again.
    """
    aligned_statuses = {
        'callsite_verified', 'logical_routes_verified',
        'explicit_release_variant_resource',
        'verified_non_language_resource',
    }
    unaligned_messages = sum(
        message.get('alignment_status') not in aligned_statuses
        for message in messages)
    unassigned_menu = sum(
        segment.get('alignment_status') not in aligned_statuses
        for segment in menu['segments'])
    structured_findings = operation_findings(messages)
    menu_findings = operation_findings(menu['segments'])
    blockers = []
    if not source_ownership or not source_ownership['complete']:
        blockers.append({
            'id': 'whole_rom_language_candidate_scan',
            'status': ('incomplete' if source_ownership else
                       'in_progress' if consumer_census else 'not_started'),
            'count': (source_ownership['unclassified_record_count']
                      if source_ownership else None),
            'detail': (
                'Consumer-rooted source ownership is incomplete; every live '
                'source and bounded dormant/variant record must be classified.'
                if source_ownership else
                'Known ranges are decoded, but consumer-rooted source '
                'ownership has not been built.'),
        })
    if not graphical_census or not graphical_census['complete']:
        blockers.append({
            'id': 'graphical_text_census',
            'status': ('incomplete' if graphical_census else
                       'in_progress' if consumer_census else 'not_started'),
            'count': (graphical_census['resource_count']
                      if graphical_census else None),
            'detail': (
                'Language-bearing graphical surfaces do not yet all have '
                'stable ownership and replacement classifications.'
                if graphical_census else
                'Lettering embedded in graphics and pre-rendered '
                'multi-tile labels is not exhaustively classified.'),
        })
    if not semantic_alignment or not semantic_alignment['complete']:
        blockers.append({
            'id': 'cross_release_semantic_alignment',
            'status': ('incomplete' if semantic_alignment else 'not_started'),
            'count': unaligned_messages + unassigned_menu,
            'detail': (
                'Logical consumer routes have not yet been joined across all '
                'five supported release profiles.'),
        })
    if not consumer_census or not consumer_census.get(
            'whole_game_consumer_discovery_complete'):
        blockers.append({
            'id': 'text_consumer_reference_census',
            'status': ('in_progress' if consumer_census else 'not_started'),
            'detail': (
                'Known consumer families are censused, but exact '
                'release-specific direct, DMA, and indirect destination '
                'closure is not available for this profile.'
                if consumer_census else
                'Pointer tables, direct references, fixed composers, and '
                'interpreter call sites are not exhaustively enumerated for '
                'this release.'),
        })
    if not dynamic_census or not dynamic_census['complete']:
        blockers.append({
            'id': 'dynamic_text_census',
            'status': ('incomplete' if dynamic_census else
                       'in_progress' if consumer_census else 'not_started'),
            'count': (dynamic_census['unresolved_operation_count']
                      if dynamic_census else None),
            'detail': (
                'Observed substitutions or their language-bearing lookup '
                'targets do not yet all have typed semantic ownership.'
                if dynamic_census else
                'Dynamic reports, generated labels, and typed value sources '
                'are not exhaustively classified.'),
        })
    unresolved_count = sum(item['count'] for item in
                           structured_findings + menu_findings)
    if unresolved_count:
        blockers.append({
            'id': 'unresolved_operations_or_glyphs',
            'status': 'incomplete',
            'count': unresolved_count,
            'detail': ('Native controls/glyphs remain lossless but are not '
                       'fully typed for Unicode authoring.'),
        })

    counts = {
        'structured_messages': len(messages),
        'logically_aligned_structured_messages': (
            len(messages) - unaligned_messages),
        'unaligned_structured_messages': unaligned_messages,
        'pointer_sets': len(pointer_sets),
        'menu_segments': len(menu['segments']),
        'semantically_assigned_menu_segments': (
            len(menu['segments']) - unassigned_menu),
        'unassigned_menu_segments': unassigned_menu,
        'unresolved_operation_instances': unresolved_count,
    }
    if consumer_census:
        consumers = consumer_census['consumers']
        counts.update({
            'known_text_consumers': len(consumers),
            'known_text_consumer_call_sites': sum(
                consumer['call_site_count'] for consumer in consumers),
            'unresolved_interactive_source_origins': (
                consumers[0]['unresolved_source_origin_call_count']),
            'nonadjacent_interactive_source_origins': (
                consumers[0]['nonadjacent_source_origin_call_count']),
            'branch_join_interactive_source_origins': (
                consumers[0]['branch_join_immediate_y_call_count']),
            'branch_join_dialogue_wrapper_calls': sum(
                call['source_origin'] == 'branch_join_immediate_y'
                for wrapper in consumer_census['dialogue_forwarding'][
                    'wrappers']
                for call in wrapper['call_sites']),
            'direct_bg3_buffer_write_sites': (
                consumer_census['bg3_buffer_writes'][
                    'direct_write_site_count']),
            'bg3_writes_outside_known_text_consumers': (
                consumer_census['bg3_buffer_writes'][
                    'outside_known_text_consumer_count']),
            'unclassified_base_bg3_write_sites': (
                consumer_census['bg3_buffer_writes'][
                    'outside_unclassified_count']),
            'base_bg3_graphical_candidates': (
                consumer_census['bg3_buffer_writes'][
                    'outside_graphical_candidate_count']),
            'base_bg3_graphical_text_sources': (
                consumer_census['bg3_buffer_writes'][
                    'outside_graphical_text_source_count']),
            'direct_long_bg3_write_candidates': (
                consumer_census['bg3_direct_long_writes'][
                    'raw_candidate_count']),
            'decoded_direct_long_bg3_writes': (
                consumer_census['bg3_direct_long_writes'][
                    'decoded_instruction_count']),
            'noncode_direct_long_bg3_patterns': (
                consumer_census['bg3_direct_long_writes'][
                    'noncode_pattern_count']),
            'unclassified_direct_long_bg3_writes': (
                consumer_census['bg3_direct_long_writes'][
                    'unclassified_count']),
            'direct_long_bg3_graphical_candidates': (
                consumer_census['bg3_direct_long_writes'][
                    'graphical_candidate_count']),
            'direct_long_bg3_graphical_text_sources': (
                consumer_census['bg3_direct_long_writes'][
                    'graphical_text_source_count']),
            'direct_long_bg3_graphical_text_regions': (
                consumer_census['bg3_direct_long_writes'][
                    'graphical_text_region_count']),
            'consumer_reference_seed_sources': (
                consumer_census['source_reference_resolution'][
                    'unique_source_count']),
            'unmapped_consumer_reference_seed_sources': (
                consumer_census['source_reference_resolution'][
                    'unmapped_unique_source_count']),
            'nested_handler_pointer_slots': (
                consumer_census['nested_handler_sources'][
                    'pointer_slot_count']),
            'nested_handler_unique_targets': (
                consumer_census['nested_handler_sources'][
                    'unique_target_count']),
            'consumer_seed_expansion_records': (
                consumer_census['source_seed_expansion'][
                    'added_record_count']),
            'fixed_composer_pointer_slots': (
                consumer_census['fixed_composer_sources'][
                    'pointer_slot_count']),
            'fixed_composer_unique_pointer_targets': (
                consumer_census['fixed_composer_sources'][
                    'unique_pointer_target_count']),
            'fixed_composer_verified_direct_sources': len(
                consumer_census['fixed_composer_sources'][
                    'direct_sources']) + len(
                consumer_census['fixed_composer_sources'][
                    'indexed_direct_sources']),
            'fixed_composer_indexed_direct_targets': sum(
                source['pointer_count'] for source in
                consumer_census['fixed_composer_sources'][
                    'indexed_direct_sources']),
            'fixed_composer_direct_source_candidates': len(
                consumer_census['fixed_composer_sources'][
                    'direct_source_candidates']),
            'fixed_composer_dynamic_reports': len(
                consumer_census['fixed_composer_sources'][
                    'dynamic_reports']),
            'fixed_composer_score_report_present': (
                consumer_census['fixed_composer_sources'][
                    'score_report_present']),
            'fixed_composer_numeric_only_sources': int(
                consumer_census['fixed_composer_sources'][
                    'numeric_only_source'] is not None),
            'fixed_composer_stateful_flow_sources': len(
                consumer_census['fixed_composer_sources'].get(
                    'flow_sources', ())),
            'fixed_composer_stateful_language_sources': sum(
                source.get('included_in_language_source_seeds', False)
                for source in consumer_census['fixed_composer_sources'].get(
                    'flow_sources', ())),
        })
    if dynamic_census:
        counts.update({
            'dynamic_operation_instances': sum(
                dynamic_census['operation_counts'].values()),
            'typed_dynamic_values': dynamic_census['typed_value_count'],
            'unresolved_dynamic_operations':
                dynamic_census['unresolved_operation_count'],
            'dynamic_language_lookup_tables':
                dynamic_census['language_lookup_table_count'],
            'dynamic_language_lookup_targets':
                dynamic_census['language_lookup_target_count'],
        })
        decoded_range = consumer_census['bg3_decoded_address_range_audit']
        direct_vram = consumer_census['direct_vram_port_writes']
        dma_launches = consumer_census['dma_launches']
        vram_descriptor = consumer_census['generic_vram_descriptor']
        indirect_writes = consumer_census['indirect_write_paths']
        dialog_font = consumer_census['dialog_font_asset']
        if decoded_range['status'] != 'not_profiled':
            counts.update({
                'decoded_bg3_address_range_references': decoded_range[
                    'decoded_reference_count'],
                'rejected_nonlong_bg3_address_candidates': decoded_range[
                    'rejected_nonlong_candidate_count'],
                'decoded_nonlong_bg3_writes': decoded_range[
                    'decoded_nonlong_bg3_write_count'],
                'direct_vram_port_write_sites': direct_vram[
                    'decoded_write_site_count'],
                'direct_vram_port_paths': len(direct_vram['paths']),
                'unclassified_direct_vram_paths': direct_vram[
                    'unclassified_path_count'],
                'decoded_dma_launch_sites': dma_launches[
                    'decoded_write_site_count'],
                'bg3_known_text_dma_transfers': dma_launches[
                    'bg3_known_text_transfer_count'],
                'unclassified_dma_launches': dma_launches[
                    'unclassified_count'],
                'generic_vram_descriptor_families': vram_descriptor[
                    'producer_or_dependency_family_count'],
                'unclassified_vram_descriptor_families': vram_descriptor[
                    'unclassified_family_count'],
                'indirect_write_families': indirect_writes['family_count'],
                'live_indirect_write_sites': indirect_writes[
                    'live_write_site_count'],
                'rejected_indirect_write_decodes': indirect_writes[
                    'rejected_decode_count'],
                'unclassified_indirect_write_sites': indirect_writes[
                    'unclassified_count'],
                'regional_dialog_font_sources': 1,
                'regional_dialog_font_script_references': dialog_font[
                    'script_reference_count'],
                'regional_dialog_font_tiles': dialog_font['tile_count'],
            })
    if source_ownership:
        counts.update({
            'classified_language_source_records': source_ownership[
                'record_count'],
            'unclassified_language_source_records': source_ownership[
                'unclassified_record_count'],
            'language_source_bytes': source_ownership[
                'text_source_byte_count'],
            'language_metadata_bytes': source_ownership[
                'metadata_byte_count'],
        })
    if graphical_census:
        counts.update({
            'classified_graphical_language_resources': graphical_census[
                'resource_count'],
            'regional_graphical_font_resources': graphical_census[
                'font_resource_count'],
            'graphical_tile_strip_or_tilemap_regions': graphical_census[
                'tile_strip_or_tilemap_region_count'],
            'graphical_full_surfaces': graphical_census[
                'full_surface_count'],
        })

    complete = not blockers
    return {
        'format': 'actraiser-language-extraction-coverage',
        'format_version': 1,
        'release_id': profile['id'],
        'locale': profile['locale'],
        'rom_sha256': source['rom_sha256'],
        'scope': 'whole_game_language_content',
        'complete': complete,
        'status': 'complete' if complete else 'incomplete',
        'counts': counts,
        'operation_findings': {
            'structured_messages': structured_findings,
            'menu_segments': menu_findings,
        },
        'blockers': blockers,
        'completion_rule': (
            'Every language-bearing candidate must be classified as semantic '
            'text, typed dynamic text, graphical text, or verified non-text; '
            'no unresolved record may remain.'),
    }


def build_extraction_index(extractions):
    """Build a deterministic cross-ROM summary without extracted wording."""
    order = {profile_id: index for index, profile_id in enumerate(
        ('us', 'eu-en', 'de', 'fr', 'jp'))}
    rows = []
    for profile, pack in extractions:
        coverage = pack['coverage']
        rows.append({
            'release_id': profile['id'],
            'release_label': profile['label'],
            'locale': profile['locale'],
            'rom_sha256': pack['source']['rom_sha256'],
            'extraction_file': f"{profile['locale']}.extraction.json",
            'coverage_file': f"{profile['locale']}.coverage.json",
            'coverage_complete': coverage['complete'],
            'coverage_status': coverage['status'],
            'blocker_count': len(coverage['blockers']),
            'counts': coverage['counts'],
        })
    rows.sort(key=lambda row: order.get(row['release_id'], len(order)))
    result = {
        'format': INDEX_FORMAT,
        'format_version': INDEX_FORMAT_VERSION,
        'scope': 'whole_game_language_content',
        'complete': all(row['coverage_complete'] for row in rows),
        'releases': rows,
    }
    alignments = {
        json.dumps(pack.get('cross_release_semantic_alignment'), sort_keys=True)
        for _, pack in extractions
        if pack.get('cross_release_semantic_alignment') is not None
    }
    if len(alignments) == 1:
        result['cross_release_semantic_alignment'] = json.loads(
            alignments.pop())
    if extractions and all(
            pack.get('semantic_route_catalog') is not None
            for _, pack in extractions):
        result['cross_release_semantic_alignment'] = \
            apply_cross_release_semantic_alignment(extractions)
    return result


def refresh_extraction_coverage(extractions):
    alignment = apply_cross_release_semantic_alignment(extractions)
    for profile, pack in extractions:
        pack['coverage'] = build_coverage_report(
            profile, pack['source'], pack['messages'], pack['pointer_sets'],
            pack['menu_source_catalog'], pack['consumer_census'],
            pack['dynamic_text_census'], pack['language_source_ownership'],
            pack['cross_release_semantic_alignment'],
            pack['graphical_text_census'])
        if pack['coverage']['complete']:
            pack['status'] = 'complete_extraction_ir'
            pack['runtime_compatibility'] = 'requires_author_pack_compilation'
        pack['capabilities']['semantic_alignment'] = (
            'logical_consumer_routes_cross_release_verified'
            if alignment['complete'] else 'local_logical_routes_only')
        pack['capabilities']['menu_semantic_ids'] = alignment['complete']
    return alignment


def extract_european(rom, profile, decoder):
    messages = []
    by_offset = {}
    pointer_sets = add_name_sets(
        messages, by_offset, profile, rom, decoder, 0x20000, 0x20043)
    lookup_inventory = add_dynamic_lookup_sets(
        messages, by_offset, pointer_sets, profile, rom, decoder)

    angel_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['angel_start'], profile.get('angel_end', profile['handler_table']),
        'angel_dialogue', 'dialogue.angel')
    town_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['town_start'], profile['offering_table'],
        'town_dialogue', 'dialogue.town')

    offerings, _ = add_pointer_set(
        messages, by_offset, profile, rom, decoder,
        profile['offering_table'], OFFERING_POINTER_COUNT,
        'offering_text', 'offering', allowed_end=profile['ending_table'])
    endings, _ = add_pointer_set(
        messages, by_offset, profile, rom, decoder,
        profile['ending_table'], ENDING_POINTER_COUNT,
        'ending_text', 'ending', allowed_end=profile['dictionary'])
    pointer_sets.extend((offerings, endings))

    ui_inventory = add_ui_families(
        messages, by_offset, pointer_sets, profile, rom, decoder)

    return messages, pointer_sets, {
        'angel_sequential_records': len(angel_starts),
        'town_sequential_records': len(town_starts),
        'handler_table': {
            'file_offset': f"0x{profile['handler_table']:06X}",
            'end_file_offset_exclusive': f"0x{profile['town_start']:06X}",
            'byte_count': profile['town_start'] - profile['handler_table'],
        },
        'dictionary': {
            'file_offset': f"0x{profile['dictionary']:06X}",
            'snes': snes_string(profile['dictionary']),
            'entries': DICTIONARY_ENTRY_COUNT,
            'entry_bytes': DICTIONARY_ENTRY_BYTES,
        },
        'localized_ui': ui_inventory,
        'dynamic_lookup_tables': lookup_inventory,
    }


def extract_japanese(rom, profile, decoder):
    messages = []
    by_offset = {}
    pointer_sets = add_name_sets(
        messages, by_offset, profile, rom, decoder,
        profile['town_name_table'], profile['enemy_name_table'])
    lookup_inventory = add_dynamic_lookup_sets(
        messages, by_offset, pointer_sets, profile, rom, decoder)

    angel_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['angel_start'], profile['handler_table'],
        'angel_dialogue_native', 'dialogue.angel')
    town_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        profile['town_start'], profile['offering_table'],
        'town_dialogue_native', 'dialogue.town')

    offerings, offering_ends = add_pointer_set(
        messages, by_offset, profile, rom, decoder,
        profile['offering_table'], OFFERING_POINTER_COUNT,
        'offering_text_native', 'offering',
        allowed_end=profile['post_text_end'])
    pointer_sets.append(offerings)

    # The Japanese bank has no equivalent eight-word ending table at the USA
    # location.  Preserve every later zero-terminated native script while its
    # call sites are assigned semantic IDs.
    post_start = max(offering_ends)
    post_starts = add_sequential_block(
        messages, by_offset, profile, rom, decoder,
        post_start, profile['post_text_end'],
        'post_offering_or_ending_native', 'post_text.unassigned')

    ui_inventory = add_ui_families(
        messages, by_offset, pointer_sets, profile, rom, decoder)

    return messages, pointer_sets, {
        'angel_sequential_records': len(angel_starts),
        'town_sequential_records': len(town_starts),
        'post_text_sequential_records': len(post_starts),
        'handler_table': {
            'file_offset': f"0x{profile['handler_table']:06X}",
            'end_file_offset_exclusive': f"0x{profile['town_start']:06X}",
            'byte_count': profile['town_start'] - profile['handler_table'],
        },
        'unicode_mapping': {
            'status': 'all_observed_source_glyphs_mapped',
            'preservation': (
                'Hiragana, katakana, Japanese punctuation, selectable '
                'spacing marks, and following DE/DF diacritics decode to '
                'Unicode. Verified cursor/selector tiles are typed icons; '
                'any future unassigned tile remains a lossless '
                'native_glyphs operation.'),
        },
        'localized_ui': ui_inventory,
        'dynamic_lookup_tables': lookup_inventory,
    }


def inspect_rom(path):
    rom = path.read_bytes()
    digest = hashlib.sha256(rom).hexdigest()
    profile = ROM_PROFILES.get(digest)
    if profile is None:
        raise ValueError(
            f'{path}: unsupported ROM SHA-256 {digest}; an exact supported '
            'clean ActRaiser ROM is required')
    if len(rom) != ROM_SIZE:
        raise ValueError(f'{path}: expected 1 MiB headerless ROM')
    dictionary_consumers = native_dictionary_consumers(profile, rom)
    decoder = Decoder(rom, profile)
    if profile['encoding'] == 'direct-glyph':
        messages, pointer_sets, inventory = extract_japanese(rom, profile, decoder)
    else:
        messages, pointer_sets, inventory = extract_european(rom, profile, decoder)

    consumer_census = build_consumer_census(profile, rom)
    menu = fixed_composer_catalog(
        profile, rom, decoder, consumer_census)
    text_candidates = sum(
        segment['classification'] in (
            'fixed_composer_text', 'typed_dynamic_text')
        for segment in menu['segments'])
    source = {
        'release_id': profile['id'],
        'release_label': profile['label'],
        'rom_file': path.name,
        'rom_title': internal_title(rom),
        'rom_size': len(rom),
        'rom_crc32': f'{zlib.crc32(rom) & 0xFFFFFFFF:08x}',
        'rom_sha256': digest,
        'encoding': profile['encoding'],
    }
    expand_unmapped_dialogue_seeds(
        consumer_census, messages, menu, profile, rom, decoder)
    dynamic_census = build_dynamic_text_census(
        profile, messages, menu, inventory)
    source_ownership = build_language_source_ownership(
        profile, rom, messages, pointer_sets, menu, consumer_census)
    semantic_routes = build_semantic_route_catalog(
        profile, messages, pointer_sets, menu, consumer_census, decoder)
    graphical_census = build_graphical_text_census(
        profile, rom, consumer_census)
    coverage = build_coverage_report(
        profile, source, messages, pointer_sets, menu, consumer_census,
        dynamic_census, source_ownership,
        graphical_census=graphical_census)
    pack = {
        'format': EXTRACTION_FORMAT,
        'format_version': EXTRACTION_FORMAT_VERSION,
        'status': 'incomplete_extraction_ir',
        'runtime_compatibility': 'requires_semantic_alignment_and_author_draft',
        'locale': profile['locale'],
        'source': source,
        'native_dialogue_layout': native_dialogue_layout(profile, rom),
        'native_dictionary_consumers': dictionary_consumers,
        'capabilities': {
            'dialogue_unicode': True,
            'native_glyphs_losslessly_preserved': True,
            'typed_control_arguments': True,
            'semantic_alignment': 'positional_and_source_address_only',
            'menu_semantic_ids': False,
        },
        'warnings': [
            'ROM-derived retail text is for local use; do not commit or redistribute.',
            ('Physical record IDs and candidate IDs are provenance only; '
             'author-facing identity comes from the logical semantic route catalog.'),
            'Convert reviewed identities to author-format .artext; the runtime intentionally does not load native extraction IR.',
        ],
        'inventory': inventory,
        'consumer_census': consumer_census,
        'dynamic_text_census': dynamic_census,
        'language_source_ownership': source_ownership,
        'graphical_text_census': graphical_census,
        'semantic_route_catalog': semantic_routes,
        'pointer_sets': pointer_sets,
        'messages': sorted(messages, key=lambda message: int(
            message['source']['file_offset'], 16)),
        'menu_source_catalog': menu,
        'coverage': coverage,
        'summary': {
            'unique_structured_messages': len(messages),
            'pointer_sets': len(pointer_sets),
            'menu_segments': len(menu['segments']),
            'menu_text_candidates': text_candidates,
        },
    }
    return profile, pack


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('roms', nargs='*', type=Path,
                        help='localized ROMs; defaults to known filenames that exist')
    parser.add_argument('--out-dir', type=Path, required=True,
                        help=('private output directory for ROM-derived '
                              'extraction and coverage artifacts'))
    parser.add_argument(
        '--require-complete', action='store_true',
        help=('return a failing status unless every release passes the '
              'whole-game language coverage gate'))
    args = parser.parse_args()

    paths = args.roms or [Path(name) for name in DEFAULT_ROMS
                          if Path(name).is_file()]
    if not paths:
        parser.error('no ROM paths supplied and no localized ROM filenames found')

    args.out_dir.mkdir(parents=True, exist_ok=True)
    seen = set()
    extractions = []
    try:
        for path in paths:
            profile, pack = inspect_rom(path)
            if profile['id'] in seen:
                raise ValueError(f'duplicate release profile {profile["id"]}')
            seen.add(profile['id'])
            extractions.append((profile, pack))
        refresh_extraction_coverage(extractions)
        for profile, pack in extractions:
            output = args.out_dir / f"{profile['locale']}.extraction.json"
            output.write_text(
                json.dumps(pack, ensure_ascii=False, indent=2) + '\n',
                encoding='utf-8')
            summary = pack['summary']
            print(f"{profile['label']}: {summary['unique_structured_messages']} "
                  f"structured messages, {summary['menu_text_candidates']}/"
                  f"{summary['menu_segments']} menu segments classified as text")
            print(f'  wrote {output}')
            coverage_output = args.out_dir / f"{profile['locale']}.coverage.json"
            coverage_output.write_text(
                json.dumps(pack['coverage'], ensure_ascii=False, indent=2) + '\n',
                encoding='utf-8')
            print(f"  coverage {pack['coverage']['status']}: "
                  f"{len(pack['coverage']['blockers'])} blocker classes")
            print(f'  wrote {coverage_output}')
    except (IndexError, OSError, ValueError) as error:
        print(f'language_pack_extract: {error}', file=sys.stderr)
        return 1

    index = build_extraction_index(extractions)
    index_output = args.out_dir / 'extraction-index.json'
    index_output.write_text(
        json.dumps(index, ensure_ascii=False, indent=2) + '\n',
        encoding='utf-8')
    print(f'  wrote {index_output}')
    print('ROM-derived output is local-only; do not commit or redistribute it.')
    print('Extraction IR is evidence input, not a runtime-loadable language pack.')
    if args.require_complete and not index['complete']:
        print('language_pack_extract: whole-game language coverage is incomplete',
              file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
