#!/usr/bin/env python3
"""Build and validate ActRaiser v1 human-editable language packs.

Retail extraction JSON and generated official-language source packs are local,
ROM-derived material. Do not commit or redistribute them. The schema, catalog
shape, parser, validator, and independently authored templates are safe to
distribute.
"""

import argparse
import collections
import hashlib
import json
import re
import unicodedata
import shlex
import sys
from pathlib import Path, PurePosixPath


PACK_FORMAT = 'actraiser-language-pack'
PACK_VERSION = 1
CATALOG_FORMAT = 'actraiser-language-semantic-catalog'
CATALOG_VERSION = 1
PROGRESS_FORMAT = 'actraiser-language-progress'
PROGRESS_VERSION = 1

MAX_MANIFEST_BYTES = 256 * 1024
MAX_SCRIPT_BYTES = 16 * 1024 * 1024
MAX_FONT_BYTES = 64 * 1024 * 1024
MAX_SOURCES = 64
MAX_FALLBACK_FONTS = 8
MAX_MESSAGES = 16384
MAX_OPERATIONS_PER_MESSAGE = 4096
MAX_AUTHORED_PAGES = 64
MAX_MESSAGE_UTF8_BYTES = 256 * 1024
MAX_WAIT_FRAMES_PER_OPERATION = 600
MAX_WAIT_FRAMES_PER_MESSAGE = 3600
MAX_EVENT_ARGUMENT_BYTES = 1024
MAX_PACKAGE_ID_BYTES = 96
MAX_LOCALE_BYTES = 32
MAX_NAME_BYTES = 192
MAX_AUTHOR_BYTES = 192
MAX_LICENSE_BYTES = 128

IDENTIFIER_RE = re.compile(r'^[A-Za-z][A-Za-z0-9_.-]*$')
LOCALE_RE = re.compile(
    r'^[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*$')

MANIFEST_KEYS = {
    'pack': {
        'format', 'version', 'id', 'locale', 'name', 'autonym', 'author',
        'license', 'direction', 'target', 'source_profile', 'fallback',
        'coverage', 'description',
    },
    'fonts': {'primary', 'fallback'},
    'scripts': {'source'},
}
REPEATED_MANIFEST_KEYS = {('fonts', 'fallback'), ('scripts', 'source')}
REQUIRED_PACK_KEYS = {
    'format', 'version', 'id', 'locale', 'name', 'autonym', 'author',
    'license', 'direction', 'target', 'source_profile', 'fallback', 'coverage',
}

AUTHOR_OPERATION_KINDS = {
    'text', 'placeholder', 'line', 'paragraph', 'page', 'wait', 'anchor',
    'event', 'empty', 'end',
}
SOURCE_TEXT_KINDS = {'text', 'line_break', 'page_break', 'end'}
SOURCE_PLACEHOLDER_KINDS = {
    'insert_master_name', 'format_number', 'insert_indexed_text', 'insert_icon',
}
SOURCE_ANCHOR_KINDS = {
    'reset_text_cursor', 'toggle_text_state', 'delay', 'yield',
}
REFLOW_SOURCE_CATEGORIES = {
    'angel_dialogue', 'angel_dialogue_native',
    'town_dialogue', 'town_dialogue_native',
    'offering_text', 'offering_text_native',
    'ending_text', 'post_offering_or_ending_native',
    'dialogue_consumer_seed',
}


class LanguagePackError(ValueError):
    pass


def fail(message, path=None, line=None):
    location = ''
    if path is not None:
        location = str(path)
        if line is not None:
            location += f':{line}'
        location += ': '
    raise LanguagePackError(location + message)


def utf8_length(value):
    return len(value.encode('utf-8'))


def require_identifier(value, description, path=None, line=None):
    if not IDENTIFIER_RE.fullmatch(value):
        fail(f'invalid {description} {value!r}', path, line)
    return value


def portable_relative_path(value, path=None, line=None):
    if not value or '\\' in value or value.startswith('/') or \
            value.endswith('/') or '//' in value:
        fail(f'path must be portable and relative: {value!r}', path, line)
    candidate = PurePosixPath(value)
    if any(part in ('', '.', '..') for part in candidate.parts):
        fail(f'path contains an unsafe component: {value!r}', path, line)
    if candidate.parts and candidate.parts[0].endswith(':'):
        fail(f'path must not contain a drive prefix: {value!r}', path, line)
    return value


def parse_manifest_text(text, path='<manifest>'):
    if utf8_length(text) > MAX_MANIFEST_BYTES:
        fail('manifest exceeds size limit', path)
    if '\0' in text:
        fail('manifest contains a NUL byte', path)
    sections = collections.defaultdict(lambda: collections.defaultdict(list))
    current = None
    seen_sections = set()
    for line_number, physical in enumerate(text.splitlines(), 1):
        line = physical.strip()
        if not line or line.startswith(('#', ';')):
            continue
        if line.startswith('[') and line.endswith(']'):
            current = line[1:-1].strip()
            if current not in MANIFEST_KEYS:
                fail(f'unknown manifest section [{current}]', path, line_number)
            if current in seen_sections:
                fail(f'duplicate manifest section [{current}]', path,
                     line_number)
            seen_sections.add(current)
            continue
        if current is None:
            fail('key appears before a section', path, line_number)
        if '=' not in line:
            fail('expected key = value', path, line_number)
        key, value = (part.strip() for part in line.split('=', 1))
        if key not in MANIFEST_KEYS[current]:
            fail(f'unknown key {key!r} in [{current}]', path, line_number)
        if not value:
            fail(f'{key!r} must not be empty', path, line_number)
        values = sections[current][key]
        if values and (current, key) not in REPEATED_MANIFEST_KEYS:
            fail(f'duplicate key {key!r}', path, line_number)
        values.append((value, line_number))

    missing_sections = sorted(set(MANIFEST_KEYS) - set(sections))
    if missing_sections:
        fail('missing sections: ' + ', '.join(missing_sections), path)
    missing_keys = sorted(REQUIRED_PACK_KEYS - set(sections['pack']))
    if missing_keys:
        fail('missing [pack] keys: ' + ', '.join(missing_keys), path)
    if 'primary' not in sections['fonts']:
        fail('missing [fonts] primary', path)
    if 'source' not in sections['scripts']:
        fail('missing [scripts] source', path)

    def one(section, key):
        return sections[section][key][0][0]

    pack = {key: one('pack', key) for key in sections['pack']}
    if pack['format'] != PACK_FORMAT or pack['version'] != str(PACK_VERSION):
        fail('unsupported language-pack format or version', path)
    require_identifier(pack['id'], 'package id', path)
    if utf8_length(pack['id']) > MAX_PACKAGE_ID_BYTES:
        fail('package id is too long', path)
    if not LOCALE_RE.fullmatch(pack['locale']) or \
            utf8_length(pack['locale']) > MAX_LOCALE_BYTES:
        fail(f'invalid BCP-47 locale {pack["locale"]!r}', path)
    for key in ('name', 'autonym'):
        if utf8_length(pack[key]) > MAX_NAME_BYTES:
            fail(f'{key} is too long', path)
    if utf8_length(pack['author']) > MAX_AUTHOR_BYTES:
        fail('author is too long', path)
    if utf8_length(pack['license']) > MAX_LICENSE_BYTES:
        fail('license is too long', path)
    if pack['direction'] not in ('ltr', 'rtl', 'auto'):
        fail('direction must be ltr, rtl, or auto', path)
    if pack['target'] not in ('us-runtime', 'reference-only'):
        fail('target must be us-runtime or reference-only', path)
    if pack['source_profile'] not in ('us', 'eu-en', 'de', 'fr', 'jp'):
        fail('source_profile is not a supported retail profile', path)
    if pack['target'] == 'us-runtime' and pack['source_profile'] != 'us':
        fail('us-runtime packs must use the U.S. semantic control contract',
             path)
    if pack['fallback'] != 'native-us':
        fail('v1 fallback must be native-us', path)
    if pack['coverage'] not in ('partial', 'complete'):
        fail('coverage must be partial or complete', path)

    primary = one('fonts', 'primary')
    fallbacks = [value for value, _ in sections['fonts'].get('fallback', [])]
    if len(fallbacks) > MAX_FALLBACK_FONTS:
        fail('too many fallback fonts', path)
    for reference in [primary] + fallbacks:
        if reference.startswith('builtin:'):
            require_identifier(reference[8:], 'built-in font id', path)
        else:
            portable_relative_path(reference, path)
    sources = [value for value, _ in sections['scripts']['source']]
    if len(sources) > MAX_SOURCES:
        fail('too many script sources', path)
    for source in sources:
        portable_relative_path(source, path)
        if not source.endswith('.artext'):
            fail(f'script source must end in .artext: {source!r}', path)
    if len(set(sources)) != len(sources):
        fail('duplicate script source', path)
    return {
        'pack': pack,
        'fonts': {'primary': primary, 'fallback': fallbacks},
        'scripts': sources,
    }


def parse_manifest(path):
    raw = Path(path).read_bytes()
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError as error:
        fail(f'manifest is not UTF-8: {error}', path)
    return parse_manifest_text(text, path)


def append_text(operations, value, source_line):
    if not value:
        return
    if operations and operations[-1]['op'] == 'text' and \
            operations[-1]['source_line'] == source_line:
        operations[-1]['value'] += value
    else:
        operations.append({
            'op': 'text', 'value': value, 'source_line': source_line})


def append_inline(operations, value, path, line):
    literal = []

    def flush_literal():
        if literal:
            append_text(operations, ''.join(literal), line)
            literal.clear()

    index = 0
    while index < len(value):
        character = value[index]
        if character == '{':
            if index + 1 < len(value) and value[index + 1] == '{':
                literal.append('{')
                index += 2
                continue
            end = value.find('}', index + 1)
            if end < 0:
                fail('unclosed placeholder', path, line)
            name = value[index + 1:end]
            digits = 0
            if ':' in name:
                name, spec = name.split(':', 1)
                if not re.fullmatch(r'0[1-9]', spec):
                    fail('number format must be 01 through 09', path, line)
                digits = int(spec[1])
            require_identifier(name, 'placeholder', path, line)
            flush_literal()
            operations.append({
                'op': 'placeholder', 'name': name, 'source_line': line,
                **({'minimum_digits': digits} if digits else {})})
            index = end + 1
            continue
        if character == '}':
            if index + 1 < len(value) and value[index + 1] == '}':
                literal.append('}')
                index += 2
                continue
            fail("unmatched '}' (write '}}' for a literal)", path, line)
        literal.append(character)
        index += 1
    flush_literal()


def parse_command(message, line, path, line_number):
    try:
        parts = shlex.split(line)
    except ValueError as error:
        fail(f'invalid command: {error}', path, line_number)
    command = parts[0]
    arguments = parts[1:]
    operations = message['operations']
    if command in ('@line', '@paragraph', '@page', '@empty', '@end'):
        if arguments:
            fail(f'{command} takes no arguments', path, line_number)
        kind = command[1:]
        operations.append({'op': kind, 'source_line': line_number})
        if kind == 'end':
            message['ended'] = True
        return
    if command == '@anchor':
        if len(arguments) != 1:
            fail('@anchor requires one stable anchor id', path, line_number)
        require_identifier(arguments[0], 'anchor id', path, line_number)
        operations.append({
            'op': 'anchor', 'id': arguments[0], 'source_line': line_number})
        return
    if command == '@wait':
        if len(arguments) != 1 or not arguments[0].isdigit():
            fail('@wait requires a decimal frame count', path, line_number)
        operations.append({
            'op': 'wait', 'frames': int(arguments[0]),
            'source_line': line_number})
        return
    if command == '@event':
        if not arguments:
            fail('@event requires an event id', path, line_number)
        require_identifier(arguments[0], 'event id', path, line_number)
        raw_arguments = ' '.join(arguments[1:])
        if utf8_length(raw_arguments) > MAX_EVENT_ARGUMENT_BYTES:
            fail('@event arguments are too long', path, line_number)
        operations.append({
            'op': 'event', 'id': arguments[0], 'arguments': raw_arguments,
            'source_line': line_number})
        return
    if command == '@alias':
        if len(arguments) != 1:
            fail('@alias requires one semantic message id', path, line_number)
        require_identifier(arguments[0], 'alias target', path, line_number)
        if message['operations'] or message.get('alias'):
            fail('@alias must be the only message content', path, line_number)
        message['alias'] = arguments[0]
        message['ended'] = True
        return
    fail(f'unknown command {command!r}', path, line_number)


def finalize_message(message, path):
    if message is None:
        return None
    operations = message['operations']
    while operations and operations[-1]['op'] == 'paragraph':
        operations.pop()
    if message.get('alias'):
        if operations:
            fail('@alias message has other operations', path,
                 message['source_line'])
        return message
    empty_count = sum(operation['op'] == 'empty' for operation in operations)
    if empty_count:
        meaningful = [operation for operation in operations
                      if operation['op'] != 'end']
        disallowed = [operation for operation in meaningful
                      if operation['op'] not in ('empty', 'anchor')]
        if empty_count != 1 or disallowed:
            fail('@empty permits only required @anchor commands', path,
                 (disallowed[0] if disallowed else meaningful[0])[
                     'source_line'])
    elif not operations:
        fail('message has no content; use @empty intentionally', path,
             message['source_line'])
    if not operations or operations[-1]['op'] != 'end':
        operations.append({'op': 'end', 'source_line': message['source_line']})
    return message


def parse_artext_text(text, path='<script>'):
    if utf8_length(text) > MAX_SCRIPT_BYTES:
        fail('script exceeds size limit', path)
    if '\0' in text:
        fail('script contains a NUL byte', path)
    messages = []
    seen = set()
    current = None
    previous_text_line = False
    for line_number, physical in enumerate(text.splitlines(), 1):
        stripped = physical.strip()
        if stripped.startswith('::'):
            if current is not None:
                messages.append(finalize_message(current, path))
            semantic_id = stripped[2:].strip()
            require_identifier(semantic_id, 'semantic message id', path,
                               line_number)
            if semantic_id in seen:
                fail(f'duplicate message {semantic_id!r}', path, line_number)
            if len(messages) >= MAX_MESSAGES:
                fail('script has too many messages', path, line_number)
            seen.add(semantic_id)
            current = {
                'id': semantic_id, 'source_line': line_number,
                'operations': [], 'ended': False,
            }
            previous_text_line = False
            continue
        if current is None:
            if not stripped or stripped.startswith(('#', ';')):
                continue
            fail('content appears before the first :: message', path,
                 line_number)
        if current['ended']:
            if not stripped or stripped.startswith(('#', ';')):
                continue
            fail('content appears after @end or @alias', path, line_number)
        if not stripped:
            previous_text_line = False
            operations = current['operations']
            if operations and operations[-1]['op'] not in (
                    'line', 'paragraph', 'page'):
                operations.append({'op': 'paragraph',
                                   'source_line': line_number})
            continue
        if stripped.startswith(('#', ';')):
            continue
        line = physical
        if line.startswith('@@'):
            line = line[1:]
        elif line.startswith('\\#') or line.startswith('\\;'):
            line = line[1:]
        elif stripped.startswith('@'):
            parse_command(current, stripped, path, line_number)
            previous_text_line = False
            continue
        if previous_text_line:
            append_text(current['operations'], ' ', line_number)
        append_inline(current['operations'], line, path, line_number)
        previous_text_line = True
    if current is not None:
        messages.append(finalize_message(current, path))
    if not messages:
        fail('script contains no messages', path)
    return messages


def parse_artext(path):
    raw = Path(path).read_bytes()
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError as error:
        fail(f'script is not UTF-8: {error}', path)
    return parse_artext_text(text, path)


def escape_text(value):
    value = value.replace('{', '{{').replace('}', '}}')
    if value.startswith('@'):
        value = '@' + value
    elif value.startswith(('#', ';')):
        value = '\\' + value
    return value


def emit_artext(messages):
    lines = []
    for message_index, message in enumerate(messages):
        if message_index:
            lines.append('')
        lines.append(f':: {message["id"]}')
        if message.get('alias'):
            lines.append(f'@alias {message["alias"]}')
            continue
        inline = []

        def flush_inline():
            if inline:
                lines.append(''.join(inline))
                inline.clear()

        for operation in message['operations']:
            kind = operation['op']
            if kind == 'text':
                inline.append(escape_text(operation['value']))
            elif kind == 'placeholder':
                spec = (':0' + str(operation['minimum_digits'])) \
                    if operation.get('minimum_digits') else ''
                inline.append('{' + operation['name'] + spec + '}')
            elif kind == 'line':
                flush_inline()
                lines.append('@line')
            elif kind == 'paragraph':
                flush_inline()
                if lines and lines[-1] != '':
                    lines.append('')
            elif kind == 'page':
                flush_inline()
                lines.append('@page')
            elif kind == 'wait':
                flush_inline()
                lines.append(f'@wait {operation["frames"]}')
            elif kind == 'anchor':
                flush_inline()
                lines.append(f'@anchor {operation["id"]}')
            elif kind == 'event':
                flush_inline()
                suffix = (' ' + operation['arguments']
                          if operation.get('arguments') else '')
                lines.append(f'@event {operation["id"]}{suffix}')
            elif kind == 'empty':
                flush_inline()
                lines.append('@empty')
            elif kind == 'end':
                flush_inline()
                lines.append('@end')
            else:
                raise LanguagePackError(
                    f'cannot emit unknown operation {kind!r}')
        flush_inline()
    return '\n'.join(lines) + '\n'


def operation_without_source_line(operation):
    return {key: value for key, value in operation.items()
            if key != 'source_line'}


def canonical_messages(messages):
    canonical = []
    for message in messages:
        operations = []
        for source in message['operations']:
            operation = operation_without_source_line(source)
            if operation['op'] == 'text' and operations and \
                    operations[-1]['op'] == 'text':
                operations[-1]['value'] += operation['value']
            else:
                operations.append(operation)
        canonical.append({
            'id': message['id'],
            **({'alias': message['alias']} if message.get('alias') else {}),
            'operations': operations,
        })
    return canonical


def placeholder_from_source(operation):
    kind = operation['op']
    if kind == 'insert_master_name':
        return 'master_name'
    if kind in ('format_number', 'insert_indexed_text'):
        return operation['value']
    if kind == 'insert_icon':
        return 'icon.' + operation['value']
    raise LanguagePackError(f'not a placeholder source operation: {kind}')


def placeholder_type(name):
    if name.startswith('icon.'):
        return 'icon'
    if name.startswith('score_') or \
            name.endswith(('_population', '_level', '_items', '_score')) or \
            name in {
                'lair_count', 'master_level', 'master_hp', 'master_sp',
                'master_max_sp', 'master_lives_display',
                'master_magic_points', 'next_level_population',
                'total_population', 'total_score', 'required_master_level',
                'sound_music_id', 'sound_effect_id',
            }:
        return 'number'
    if name.endswith('_growth_state'):
        return 'localized_term'
    return 'localized_text'


def source_anchor(operation, ordinal):
    kind = operation['op']
    anchor = {'id': f'{kind}.{ordinal:02d}', 'kind': kind}
    if kind == 'delay':
        anchor['frames'] = operation['frames']
    if kind == 'toggle_text_state':
        anchor['state'] = operation['native_control']
    return anchor


def source_contract(operations):
    placeholders = []
    anchors = []
    for operation in operations:
        kind = operation['op']
        if kind in SOURCE_PLACEHOLDER_KINDS:
            placeholders.append(placeholder_from_source(operation))
        elif kind in SOURCE_ANCHOR_KINDS:
            anchors.append(source_anchor(operation, len(anchors)))
        elif kind not in SOURCE_TEXT_KINDS:
            raise LanguagePackError(
                f'unrepresentable extracted operation {kind!r}')
    return {
        'placeholders_used': placeholders,
        'required_anchors': anchors,
        'native_authored_page_count': 1 + sum(
            operation['op'] == 'page_break' for operation in operations),
        'native_operation_count': len(operations),
    }


def build_semantic_catalog(extractions):
    release_order = ('us', 'eu-en', 'de', 'fr', 'jp')
    by_release = {}
    for extraction in extractions:
        if extraction.get('format') != 'actraiser-language-extraction' or \
                not extraction.get('coverage', {}).get('complete'):
            raise LanguagePackError(
                'semantic catalogs require complete extraction IR')
        release_id = extraction['source']['release_id']
        if release_id in by_release:
            raise LanguagePackError(f'duplicate extraction {release_id}')
        by_release[release_id] = extraction
    missing = [release for release in release_order if release not in by_release]
    if missing:
        raise LanguagePackError(
            'semantic catalog requires all five releases: ' +
            ', '.join(missing))

    routes = collections.defaultdict(dict)
    for release_id, extraction in by_release.items():
        for route in extraction['semantic_route_catalog']['routes']:
            if 'source_operations' not in route:
                raise LanguagePackError(
                    'extraction predates invocation operation streams')
            routes[route['id']][release_id] = route

    result_routes = []
    placeholder_registry = {}
    for route_id in sorted(routes):
        releases = routes[route_id]
        contracts = {}
        allowed_placeholders = set()
        categories = {}
        for release_id in release_order:
            route = releases.get(release_id)
            if route is None:
                continue
            contract = source_contract(route['source_operations'])
            contracts[release_id] = contract
            allowed_placeholders.update(contract['placeholders_used'])
            categories[release_id] = route['source_category']
        for name in allowed_placeholders:
            placeholder_registry[name] = placeholder_type(name)
        canonical_release = 'us' if 'us' in releases else next(
            release for release in release_order if release in releases)
        result_routes.append({
            'id': route_id,
            'availability': ('all_supported_releases'
                             if len(releases) == len(release_order) else
                             'regional_or_release_variant'),
            'available_releases': [release for release in release_order
                                   if release in releases],
            'canonical_contract_profile': canonical_release,
            'allowed_placeholders': sorted(allowed_placeholders),
            'source_categories': categories,
            'contracts': contracts,
        })
    return {
        'format': CATALOG_FORMAT,
        'version': CATALOG_VERSION,
        'identity': 'logical_consumer_route',
        'release_order': list(release_order),
        'route_count': len(result_routes),
        'all_release_route_count': sum(
            route['availability'] == 'all_supported_releases'
            for route in result_routes),
        'regional_or_release_variant_route_count': sum(
            route['availability'] == 'regional_or_release_variant'
            for route in result_routes),
        'placeholder_count': len(placeholder_registry),
        'placeholders': dict(sorted(placeholder_registry.items())),
        'limits': {
            'operations_per_message': MAX_OPERATIONS_PER_MESSAGE,
            'authored_pages_per_message': MAX_AUTHORED_PAGES,
            'message_utf8_bytes': MAX_MESSAGE_UTF8_BYTES,
            'wait_frames_per_operation': MAX_WAIT_FRAMES_PER_OPERATION,
            'wait_frames_per_message': MAX_WAIT_FRAMES_PER_MESSAGE,
        },
        'routes': result_routes,
    }


def load_extraction(path):
    with Path(path).open(encoding='utf-8') as source:
        return json.load(source)


def load_catalog(path):
    with Path(path).open(encoding='utf-8') as source:
        catalog = json.load(source)
    if catalog.get('format') != CATALOG_FORMAT or \
            catalog.get('version') != CATALOG_VERSION:
        raise LanguagePackError('unsupported semantic catalog')
    return catalog


def fixed_table_operations(operations, semantic_id):
    """Export explicit editable cells; never infer columns from translated text.

    Retail report rows have verified roles. Only this source-conversion step
    interprets their padding. Runtime authors can freely use spaces in cells.
    """
    if semantic_id == 'system.message_speed.scale_labels':
        result = [dict(operation) for operation in operations]
        if result and result[0]['op'] == 'text':
            digits = result[0]['value'].strip()
            if digits.isascii() and digits.isdigit():
                result[0]['value'] = ' | '.join(digits)
        fields = []
        for operation in result:
            direction = operation['op'] == 'placeholder' and \
                operation['name'] == 'icon.ui.speed_direction'
            if direction:
                fields.append({'op': 'text', 'value': ' | '})
            fields.append(operation)
            if direction:
                fields.append({'op': 'text', 'value': ' | '})
        return fields
    if not semantic_id.startswith('status.report.'):
        return operations
    result, row = [], []
    row_index = 0
    for operation in operations:
        if operation['op'] not in ('line', 'end'):
            row.append(operation)
            continue
        master = semantic_id == 'status.report.master_report'
        score = semantic_id == 'status.report.score_report'
        split = (not master and row_index not in (2, 5)) or \
            (master and row_index in (3, 5, 7, 9))
        if row_index == 0 and not any(item['op'] == 'placeholder' for item in row):
            split = False  # e.g. the French two-line population title.
        if row_index == 1 and not master:
            # A continued title is a multiword label plus its total, not
            # separate columns for each word of that label.
            row = [dict(item, value=' '.join(item['value'].split()) + '  ')
                   if item['op'] == 'text' and item['value'].strip() else item
                   for item in row]
            separator = r'\s{2,}'
        else:
            separator = r'\s{2,}' if row_index == 0 or \
                (score and row_index == 3) else r'\s+'
        cells = [[]]
        for item in row:
            if not split or item['op'] != 'text':
                cells[-1].append(item)
                continue
            parts = re.split('(' + separator + ')', item['value'])
            for part in parts:
                if not part:
                    continue
                if re.fullmatch(separator, part):
                    if cells[-1]:
                        cells.append([])
                else:
                    cells[-1].append({'op': 'text', 'value': part})
        cells = [cell for cell in cells if cell]
        for index, cell in enumerate(cells):
            if index:
                result.append({'op': 'text', 'value': ' | '})
            result.extend(cell)
        result.append(operation)
        row, row_index = [], row_index + 1
    return result


# These categories use the native small dialogue window. Ending scenes have
# different geometry; retain their explicit breaks until separately profiled.
NATIVE_DIALOGUE_REFLOW_CATEGORIES = REFLOW_SOURCE_CATEGORIES - {
    'ending_text', 'post_offering_or_ending_native',
}


def native_line_breaks(route, layout):
    """Return source indices of hard breaks; uncertainty preserves the break.

    A line is soft only if the next complete native word could not have fit.
    Measure native cells, not UTF-8 bytes, HD font advances or word count.
    Unknown dynamic widths preserve the break: never infer author intent from
    the name or population in a particular save.
    """
    source = route['source_operations']
    breaks = {i for i, op in enumerate(source) if op['op'] == 'line_break'}
    if (not layout or not layout.get('space_delimited_words', False) or
            route['source_category'] not in NATIVE_DIALOGUE_REFLOW_CATEGORIES):
        return breaks
    columns = layout['columns']

    def units(operations):
        result = []
        for op in operations:
            kind = op['op']
            if kind == 'text':
                # Dictionary expansion carries padding; it is not visible
                # word width. A combining dakuten shares its native cell.
                for char in unicodedata.normalize('NFC', op['value']):
                    if char.isspace():
                        if result and result[-1] != ' ':
                            result.append(' ')
                    elif not unicodedata.combining(char):
                        result.append(1)
            elif kind == 'format_number' and 0 < op.get('width', 0) < 10:
                result.append(op['width'])
            elif kind == 'insert_icon':
                result.append(1)
            elif kind in SOURCE_PLACEHOLDER_KINDS:
                result.append(None)  # Variable lookup/BCD width is unknown.
        while result and result[-1] == ' ':
            result.pop()
        return result

    def cell_width(items):
        if None in items:
            return None
        return sum(1 if item == ' ' else item for item in items)

    boundaries = {'line_break', 'page_break', 'reset_text_cursor', 'yield', 'end'}
    start = 0
    for index, op in enumerate(source):
        if op['op'] == 'line_break':
            end = index + 1
            while end < len(source) and source[end]['op'] not in boundaries:
                end += 1
            current = units(source[start:index])
            following = units(source[index + 1:end])
            next_word = following[:following.index(' ')] if ' ' in following else following
            left, right = cell_width(current), cell_width(next_word)
            if current and next_word and left is not None and right is not None:
                # Exact fits are intentional breaks; only a complete word
                # that overflows the native width is eligible for reflow.
                if left + 1 + right > columns:
                    breaks.remove(index)
        if op['op'] in boundaries:
            start = index + 1
    return breaks


def source_operations_to_author(route, native_layout=None):
    operations = []
    reflow = route['source_category'] in REFLOW_SOURCE_CATEGORIES
    hard_breaks = native_line_breaks(route, native_layout) if reflow else set()
    anchors = 0
    for source_index, source in enumerate(route['source_operations']):
        kind = source['op']
        if kind == 'text':
            value = source['value']
            if reflow:
                # Preserve explicit word boundaries on both sides of values
                # and punctuation. A placeholder is not itself a word break.
                value = re.sub(r'\s+', ' ', value)
            if value:
                operations.append({'op': 'text', 'value': value})
        elif kind == 'line_break':
            if reflow and source_index not in hard_breaks:
                if operations and operations[-1]['op'] in (
                        'text', 'placeholder'):
                    operations.append({'op': 'text', 'value': ' '})
            else:
                operations.append({'op': 'line'})
        elif kind == 'page_break':
            operations.append({'op': 'page'})
        elif kind in SOURCE_PLACEHOLDER_KINDS:
            if kind == 'insert_icon' and source.get('part_index', 0):
                # One logical object, even when the native atlas uses two
                # adjacent tiles. IR retains every part for extraction QA.
                continue
            placeholder = {
                'op': 'placeholder', 'name': placeholder_from_source(source)}
            if kind == 'format_number' and source.get('width'):
                width = source['width']
                # Decimal fields retain leading zeros. The packed-BCD score
                # renderer ($02:BFF6) skips leading zero nibbles and advances
                # the cursor instead; its trailing zero is already included
                # in the numeric value. Cell geometry supplies that alignment.
                if not width & 0x80:
                    placeholder['minimum_digits'] = width
            operations.append(placeholder)
        elif kind in SOURCE_ANCHOR_KINDS:
            operations.append({
                'op': 'anchor', 'id': source_anchor(source, anchors)['id']})
            anchors += 1
        elif kind == 'end':
            operations.append({'op': 'end'})
        else:
            raise LanguagePackError(
                f'unrepresentable extracted operation {kind!r}')
    # Keep hard line/paragraph boundaries. Only width-induced wraps and retail
    # padding are presentation rather than content on variable-width surfaces.
    if reflow:
        normalized = []
        for operation in operations:
            if operation['op'] == 'text':
                if normalized and normalized[-1]['op'] == 'text':
                    normalized[-1]['value'] += operation['value']
                elif operation['value']:
                    normalized.append(dict(operation))
            else:
                normalized.append(operation)
        for operation in normalized:
            if operation['op'] == 'text':
                operation['value'] = re.sub(r'\s+', ' ', operation['value'])
        visible = [operation for operation in normalized
                   if operation['op'] in ('text', 'placeholder')]
        # A separator after a leading placeholder (or before a trailing one)
        # is content. Only trim at the actual visible paragraph boundary.
        if visible and visible[0]['op'] == 'text':
            visible[0]['value'] = visible[0]['value'].lstrip()
        if visible and visible[-1]['op'] == 'text':
            visible[-1]['value'] = visible[-1]['value'].rstrip()
        operations = [operation for operation in normalized
                      if operation['op'] != 'text' or operation['value']]
    return fixed_table_operations(operations, route['id'])


def build_source_messages(extraction):
    routes = extraction['semantic_route_catalog']['routes']
    canonical_by_hash = {}
    messages = []
    for route in sorted(routes, key=lambda item: item['id']):
        operations = source_operations_to_author(
            route, extraction.get('native_dialogue_layout'))
        # Source-identical routes can have different layout/line-break rules.
        # Alias only the actual author operations, not their pre-layout IR.
        content_hash = json.dumps(operations, ensure_ascii=False, sort_keys=True)
        canonical = canonical_by_hash.get(content_hash)
        if canonical is not None:
            messages.append({
                'id': route['id'], 'alias': canonical, 'operations': []})
            continue
        canonical_by_hash[content_hash] = route['id']
        visible = any(operation['op'] in ('text', 'placeholder')
                      for operation in operations)
        if not visible:
            operations.insert(0, {'op': 'empty'})
        messages.append({'id': route['id'], 'operations': operations})
    return messages


def manifest_text(metadata, source='text/source.artext'):
    lines = [
        '[pack]',
        f'format = {PACK_FORMAT}',
        f'version = {PACK_VERSION}',
        f'id = {metadata["id"]}',
        f'locale = {metadata["locale"]}',
        f'name = {metadata["name"]}',
        f'autonym = {metadata["autonym"]}',
        f'author = {metadata["author"]}',
        f'license = {metadata["license"]}',
        f'direction = {metadata["direction"]}',
        f'target = {metadata["target"]}',
        f'source_profile = {metadata["source_profile"]}',
        'fallback = native-us',
        f'coverage = {metadata.get("coverage", "complete")}',
        '',
        '[fonts]',
        f'primary = {metadata.get("primary_font", "builtin:actraiser-sans")}',
    ]
    for fallback in metadata.get('fallback_fonts', []):
        lines.append(f'fallback = {fallback}')
    lines.extend(['', '[scripts]', f'source = {source}', ''])
    return '\n'.join(lines)


def progress_text(message_ids, statuses=None):
    statuses = statuses or {}
    lines = [
        f'# format={PROGRESS_FORMAT} version={PROGRESS_VERSION}',
        '# semantic-id<TAB>not_started|wip|done',
    ]
    for semantic_id in sorted(message_ids):
        lines.append(f'{semantic_id}\t{statuses.get(semantic_id, "not_started")}')
    return '\n'.join(lines) + '\n'


def parse_progress_text(text, known_ids=None, path='<progress>'):
    statuses = {}
    for line_number, physical in enumerate(text.splitlines(), 1):
        if not physical or physical.startswith('#'):
            continue
        parts = physical.split('\t')
        if len(parts) != 2:
            fail('expected semantic-id<TAB>status', path, line_number)
        semantic_id, status = parts
        require_identifier(semantic_id, 'progress message id', path,
                           line_number)
        if semantic_id in statuses:
            fail(f'duplicate progress row {semantic_id!r}', path, line_number)
        if status not in ('not_started', 'wip', 'done'):
            fail(f'invalid progress status {status!r}', path, line_number)
        if known_ids is not None and semantic_id not in known_ids:
            fail(f'unknown progress message {semantic_id!r}', path,
                 line_number)
        statuses[semantic_id] = status
    return statuses


def validate_messages(messages, catalog, source_profile, coverage='partial',
                      allowed_events=()):
    route_by_id = {route['id']: route for route in catalog['routes']}
    message_by_id = {}
    for message in messages:
        semantic_id = message['id']
        if semantic_id in message_by_id:
            raise LanguagePackError(f'duplicate message {semantic_id!r}')
        if semantic_id not in route_by_id:
            raise LanguagePackError(f'unknown semantic message {semantic_id!r}')
        message_by_id[semantic_id] = message
    if coverage == 'complete':
        required_ids = {
            route_id for route_id, route in route_by_id.items()
            if source_profile in route['contracts']
        }
        missing = sorted(required_ids - set(message_by_id))
        if missing:
            raise LanguagePackError(
                f'complete pack is missing {len(missing)} messages; first is '
                f'{missing[0]}')

    allowed_events = set(allowed_events)
    for semantic_id, message in message_by_id.items():
        route = route_by_id[semantic_id]
        contract_profile = (source_profile if source_profile in route['contracts']
                            else route['canonical_contract_profile'])
        contract = route['contracts'][contract_profile]
        if message.get('alias'):
            target = message['alias']
            if target not in route_by_id:
                raise LanguagePackError(
                    f'{semantic_id}: unknown alias target {target!r}')
            continue
        operations = message['operations']
        if len(operations) > MAX_OPERATIONS_PER_MESSAGE:
            raise LanguagePackError(
                f'{semantic_id}: too many operations')
        text_bytes = sum(utf8_length(operation.get('value', ''))
                         for operation in operations)
        if text_bytes > MAX_MESSAGE_UTF8_BYTES:
            raise LanguagePackError(f'{semantic_id}: text is too large')
        pages = 1 + sum(operation['op'] == 'page'
                        for operation in operations)
        if pages > MAX_AUTHORED_PAGES:
            raise LanguagePackError(f'{semantic_id}: too many authored pages')
        wait_total = 0
        for operation in operations:
            if operation.get('minimum_digits') and (
                    operation['op'] != 'placeholder' or
                    catalog['placeholders'].get(operation['name']) != 'number' or
                    not 1 <= operation['minimum_digits'] <= 9):
                raise LanguagePackError(
                    f'{semantic_id}: number format requires a numeric placeholder')
            if operation['op'] not in AUTHOR_OPERATION_KINDS:
                raise LanguagePackError(
                    f'{semantic_id}: unknown operation {operation["op"]!r}')
            if operation['op'] == 'placeholder' and \
                    operation['name'] not in route['allowed_placeholders']:
                raise LanguagePackError(
                    f'{semantic_id}: placeholder '
                    f'{{{operation["name"]}}} is unavailable on this route')
            if operation['op'] == 'wait':
                frames = operation['frames']
                if not 1 <= frames <= MAX_WAIT_FRAMES_PER_OPERATION:
                    raise LanguagePackError(
                        f'{semantic_id}: @wait must be 1-'
                        f'{MAX_WAIT_FRAMES_PER_OPERATION} frames')
                wait_total += frames
            if operation['op'] == 'event' and \
                    operation['id'] not in allowed_events:
                raise LanguagePackError(
                    f'{semantic_id}: event {operation["id"]!r} is not '
                    'allow-listed')
        if wait_total > MAX_WAIT_FRAMES_PER_MESSAGE:
            raise LanguagePackError(
                f'{semantic_id}: cumulative authored wait is too long')
        expected_anchors = [anchor['id']
                            for anchor in contract['required_anchors']]
        actual_anchors = [operation['id'] for operation in operations
                          if operation['op'] == 'anchor']
        if actual_anchors != expected_anchors:
            raise LanguagePackError(
                f'{semantic_id}: locked anchors changed; expected '
                f'{expected_anchors}, found {actual_anchors}')

    for semantic_id, message in message_by_id.items():
        seen = {semantic_id}
        cursor = message
        while cursor.get('alias'):
            target = cursor['alias']
            if target in seen:
                raise LanguagePackError(f'{semantic_id}: alias cycle detected')
            seen.add(target)
            cursor = message_by_id.get(target)
            if cursor is None:
                raise LanguagePackError(
                    f'{semantic_id}: alias target {target!r} is not included '
                    'in this pack')
        if message.get('alias'):
            route = route_by_id[semantic_id]
            contract_profile = (
                source_profile if source_profile in route['contracts'] else
                route['canonical_contract_profile'])
            expected_anchors = [
                anchor['id'] for anchor in
                route['contracts'][contract_profile]['required_anchors']]
            actual_anchors = [
                operation['id'] for operation in cursor['operations']
                if operation['op'] == 'anchor']
            if actual_anchors != expected_anchors:
                raise LanguagePackError(
                    f'{semantic_id}: alias target has incompatible locked '
                    'anchors')
            unavailable = sorted({
                operation['name'] for operation in cursor['operations']
                if operation['op'] == 'placeholder' and
                operation['name'] not in route['allowed_placeholders']
            })
            if unavailable:
                raise LanguagePackError(
                    f'{semantic_id}: alias target uses unavailable '
                    f'placeholder {unavailable[0]!r}')
    return {
        'message_count': len(messages),
        'alias_count': sum(bool(message.get('alias')) for message in messages),
        'operation_count': sum(len(message['operations'])
                               for message in messages),
    }


def validate_pack(pack_directory, catalog, allowed_events=()):
    root = Path(pack_directory)
    resolved_root = root.resolve()
    manifest_path = root / 'pack.ini'
    manifest = parse_manifest(manifest_path)
    messages = []
    content_hash = hashlib.sha256()

    def hash_part(kind, name, data):
        for part in (kind.encode('utf-8'), name.encode('utf-8'), data):
            content_hash.update(len(part).to_bytes(8, 'big'))
            content_hash.update(part)

    hash_part('manifest', 'pack.ini', manifest_path.read_bytes())
    for relative in manifest['scripts']:
        script_path = root.joinpath(*PurePosixPath(relative).parts)
        try:
            script_path.resolve().relative_to(resolved_root)
        except ValueError:
            fail(f'script escapes pack directory: {relative!r}', manifest_path)
        if not script_path.is_file():
            fail(f'missing script {relative!r}', manifest_path)
        raw = script_path.read_bytes()
        hash_part('script', relative, raw)
        messages.extend(parse_artext(script_path))
    for reference in ([manifest['fonts']['primary']] +
                      manifest['fonts']['fallback']):
        if reference.startswith('builtin:'):
            hash_part('builtin-font', reference, b'')
            continue
        font_path = root.joinpath(*PurePosixPath(reference).parts)
        try:
            font_path.resolve().relative_to(resolved_root)
        except ValueError:
            fail(f'font escapes pack directory: {reference!r}', manifest_path)
        if not font_path.is_file():
            fail(f'missing font {reference!r}', manifest_path)
        font_bytes = font_path.read_bytes()
        if not font_bytes or len(font_bytes) > MAX_FONT_BYTES:
            fail(f'font is empty or exceeds size limit: {reference!r}',
                 manifest_path)
        hash_part('font', reference, font_bytes)
    stats = validate_messages(
        messages, catalog, manifest['pack']['source_profile'],
        manifest['pack']['coverage'], allowed_events)
    progress_path = root / 'translation-progress.tsv'
    progress = {}
    if progress_path.is_file():
        try:
            progress_source = progress_path.read_text(encoding='utf-8')
        except UnicodeDecodeError as error:
            fail(f'progress file is not UTF-8: {error}', progress_path)
        progress = parse_progress_text(
            progress_source, {message['id'] for message in messages},
            progress_path)
    return {
        'valid': True,
        'package_id': manifest['pack']['id'],
        'locale': manifest['pack']['locale'],
        'content_sha256': content_hash.hexdigest(),
        'progress_rows': len(progress),
        **stats,
    }


def write_source_pack(extraction, output, metadata):
    output = Path(output)
    source_dir = output / 'text'
    source_dir.mkdir(parents=True, exist_ok=True)
    messages = build_source_messages(extraction)
    manifest = manifest_text(metadata)
    parse_manifest_text(manifest)
    (output / 'pack.ini').write_text(manifest, encoding='utf-8')
    (source_dir / 'source.artext').write_text(
        emit_artext(messages), encoding='utf-8')
    (output / 'translation-progress.tsv').write_text(
        progress_text((message['id'] for message in messages)),
        encoding='utf-8')
    return {'message_count': len(messages)}


def write_json(path, value):
    Path(path).write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n',
        encoding='utf-8')


def command_catalog(args):
    catalog = build_semantic_catalog(
        [load_extraction(path) for path in args.extraction])
    write_json(args.out, catalog)
    print(f'wrote {args.out}: {catalog["route_count"]} semantic routes, '
          f'{catalog["placeholder_count"]} placeholders')
    return 0


def command_source(args):
    extraction = load_extraction(args.extraction)
    release_id = extraction['source']['release_id']
    metadata = {
        'id': args.id,
        'locale': args.locale or extraction['locale'],
        'name': args.name,
        'autonym': args.autonym,
        'author': args.author,
        'license': args.license,
        'direction': args.direction,
        'target': args.target,
        'source_profile': release_id,
        'coverage': 'complete',
    }
    # Validate metadata before creating any output.
    parse_manifest_text(manifest_text(metadata))
    result = write_source_pack(extraction, args.out_dir, metadata)
    print(f'wrote {args.out_dir}: {result["message_count"]} messages')
    print('Generated official-ROM wording is local-only; do not redistribute it.')
    return 0


def command_validate(args):
    catalog = load_catalog(args.catalog)
    result = validate_pack(args.pack, catalog)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest='command', required=True)
    catalog = subparsers.add_parser(
        'catalog', help='build a five-ROM semantic contract catalog')
    catalog.add_argument('--extraction', action='append', required=True,
                         help='complete extraction JSON; pass all five')
    catalog.add_argument('--out', type=Path, required=True)
    catalog.set_defaults(function=command_catalog)

    source = subparsers.add_parser(
        'source', help='create a local author source from extraction IR')
    source.add_argument('--extraction', type=Path, required=True)
    source.add_argument('--out-dir', type=Path, required=True)
    source.add_argument('--id', required=True)
    source.add_argument('--name', required=True)
    source.add_argument('--autonym', required=True)
    source.add_argument('--author', required=True)
    source.add_argument('--license', required=True)
    source.add_argument('--locale')
    source.add_argument('--direction', choices=('ltr', 'rtl', 'auto'),
                        default='auto')
    source.add_argument('--target', choices=('us-runtime', 'reference-only'),
                        default='reference-only')
    source.set_defaults(function=command_source)

    validate = subparsers.add_parser(
        'validate', help='validate a v1 language-pack directory')
    validate.add_argument('--pack', type=Path, required=True)
    validate.add_argument(
        '--catalog', type=Path,
        default=Path(__file__).resolve().parent /
        'data/localization/semantic-catalog-v1.json')
    validate.set_defaults(function=command_validate)
    args = parser.parse_args()
    try:
        return args.function(args)
    except (LanguagePackError, OSError, json.JSONDecodeError) as error:
        print(f'language_pack_v1: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
