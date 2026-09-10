#!/usr/bin/env python3
"""Report language-pack character gaps using the game's actual font backend."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from language_pack_v1 import MAX_FONT_BYTES, parse_artext, parse_manifest


ROOT = Path(__file__).resolve().parents[1]
BUILTIN_FONT = ROOT / 'game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf'
DEFAULT_PROBE = ROOT / 'build' / (
    'actraiser_font_coverage.exe' if sys.platform == 'win32'
    else 'actraiser_font_coverage')


def pack_resource(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f'resource escapes pack directory: {relative!r}')
    return path


def check_pack_fonts(pack, probe=DEFAULT_PROBE, builtin_font=BUILTIN_FONT,
                     samples=()):
    """Return JSON-ready diagnostics; no game/renderer/ROM is launched.

    Deliberately separate from semantic validation: this also checks reference
    packs and offers a reusable entry point for the forthcoming builder UI.
    Only literal text and explicit samples are resolved here, not live values.
    """
    root = Path(pack).resolve()
    manifest = parse_manifest(root / 'pack.ini')
    fonts = []
    for reference in [manifest['fonts']['primary'], *manifest['fonts']['fallback']]:
        if reference == 'builtin:actraiser-sans':
            path = Path(builtin_font).resolve()
        elif reference.startswith('builtin:'):
            raise ValueError(f'unknown built-in font: {reference!r}')
        else:
            path = pack_resource(root, reference)
        if not path.is_file() or not 0 < path.stat().st_size <= MAX_FONT_BYTES:
            raise ValueError(f'font missing, empty, or over size limit: {path}')
        fonts.append({'reference': reference, 'path': str(path),
                      'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})

    locations = {}
    dynamic = set()

    def collect(text, source, message_id, line):
        for character in set(text):
            if 0xd800 <= ord(character) <= 0xdfff or character == '\0':
                raise ValueError('text samples must contain valid non-NUL Unicode')
            locations.setdefault(ord(character), set()).add((source, message_id, line))

    for relative in manifest['scripts']:
        for message in parse_artext(pack_resource(root, relative)):
            for operation in message['operations']:
                if operation['op'] == 'text':
                    collect(operation['value'], relative, message['id'],
                            operation['source_line'])
                elif operation['op'] == 'placeholder':
                    if not operation['name'].startswith('icon.'):
                        dynamic.add(operation['name'])
    for index, sample in enumerate(samples, 1):
        collect(sample, f'<sample:{index}>', '', 1)

    probe = Path(probe).resolve()
    if not probe.is_file():
        raise ValueError(f'font probe not found: {probe}; build the '
                         'actraiser_font_coverage target or pass --probe')
    scalars = sorted(locations)
    response = subprocess.run(
        [str(probe), *(font['path'] for font in fonts)],
        input=''.join(f'{scalar:04X}\n' for scalar in scalars),
        text=True, encoding='utf-8', capture_output=True, timeout=30, check=False)
    if response.returncode:
        raise ValueError('font backend failed: ' + response.stderr.strip()[:1024])
    provided = {}
    for line in response.stdout.splitlines():
        fields = line.split('\t')
        if len(fields) != 2 or fields[1] not in ('0', '1'):
            raise ValueError('invalid font probe response')
        scalar = int(fields[0], 16)
        if scalar in provided:
            raise ValueError('duplicate scalar in font probe response')
        provided[scalar] = fields[1] == '1'
    if set(provided) != set(scalars):
        raise ValueError('incomplete font probe response')
    missing = [
        {'codepoint': f'U+{scalar:04X}', 'character': chr(scalar),
         'locations': [{'source': source, 'message_id': message, 'line': line}
                       for source, message, line in sorted(locations[scalar])]}
        for scalar in scalars if not provided[scalar]
    ]
    return {
        'format': 'actraiser-font-coverage', 'version': 1,
        'package_id': manifest['pack']['id'], 'locale': manifest['pack']['locale'],
        'fonts': fonts, 'distinct_scalars': len(scalars),
        'scalar_coverage_complete': not missing, 'missing': missing,
        'dynamic_placeholders_not_resolved': sorted(dynamic),
        'scope': 'authored literal text and supplied samples',
        'limitations': ('Scalar coverage does not certify shaping, ligatures, '
                       'emoji sequences, or layout. Layout controls, typed '
                       'objects and Unicode default-ignorables do not require '
                       'standalone glyphs. Preview complex-script text and '
                       'supply --sample for names and other dynamic values.'),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=DEFAULT_PROBE)
    parser.add_argument('--builtin-font', type=Path, default=BUILTIN_FONT)
    parser.add_argument('--sample', action='append', default=[],
                        help='additional dynamic text, such as a player name')
    parser.add_argument('--out', type=Path, help='save the JSON report here')
    args = parser.parse_args(argv)
    try:
        report = check_pack_fonts(args.pack, args.probe, args.builtin_font, args.sample)
        encoded = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
        if args.out:
            args.out.write_text(encoded, encoding='utf-8')
        else:
            print(encoded, end='')
        return 0 if report['scalar_coverage_complete'] else 1
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f'font coverage: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
