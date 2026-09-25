#!/usr/bin/env python3
"""Check snesbuild.ini, the source list that shipped builds compile from.

Release bundles and the Builder compile the game hermetically from
snesbuild.ini, not from CMake. The game target that proves the list links
needs a ROM, so a source that is moved, renamed or added without updating the
manifest would otherwise surface only when someone runs a hermetic build.
This check needs no ROM and no generated code:

  * every `source =` file and every `include =` directory must exist,
  * no source may be listed twice (it would compile and link twice), and
  * every authored C source under src/ must be listed. src/gen is generated
    and globbed separately by both builds, so it is not listed.

Lines are read the way snesbuild's manifest loader reads them: trimmed, with
blank lines and lines starting with '#' or ';' ignored.
"""

import argparse
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATED_DIRS = ('src/gen', 'src/generated')


def read_manifest(path):
    entries = {'source': [], 'include': []}
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(('#', ';')):
            continue
        key, separator, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if separator and key in entries and value:
            entries[key].append((number, value))
    return entries


def authored_sources(root):
    sources = set()
    for path in (root / 'src').rglob('*.c'):
        relative = path.relative_to(root).as_posix()
        if not relative.startswith(tuple(d + '/' for d in GENERATED_DIRS)):
            sources.add(relative)
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT,
                        help='project root holding snesbuild.ini')
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = root / 'snesbuild.ini'
    if not manifest.is_file():
        sys.exit(f'check_source_manifest: {manifest} is missing')

    entries = read_manifest(manifest)
    problems = []
    seen = {}
    for number, value in entries['source']:
        if not (root / value).is_file():
            problems.append(f'snesbuild.ini:{number}: source {value} does not exist')
        if value in seen:
            problems.append(f'snesbuild.ini:{number}: source {value} is already '
                            f'listed on line {seen[value]}')
        seen.setdefault(value, number)
    for number, value in entries['include']:
        if not (root / value).is_dir():
            problems.append(f'snesbuild.ini:{number}: include {value} is not a directory')
    if not entries['source']:
        problems.append('snesbuild.ini declares no `source =` entries')

    listed = {value for _, value in entries['source']}
    for source in sorted(authored_sources(root) - listed):
        problems.append(f'{source} is not listed in snesbuild.ini; '
                        'shipped builds would not compile it')

    if problems:
        print('Source manifest check failed:', file=sys.stderr)
        for problem in problems:
            print(f'  {problem}', file=sys.stderr)
        sys.exit(1)
    print(f'Source manifest: {len(listed)} sources and '
          f'{len(entries["include"])} include directories present; '
          'every authored src/ source is listed')


if __name__ == '__main__':
    main()
