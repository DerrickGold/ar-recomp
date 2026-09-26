#!/usr/bin/env python3
"""Check that private headers are included only by the files they serve.

C has no scope between one translation unit and the whole program: once a
split file's helpers lose `static`, any file could declare and call them. The
split families therefore share their helpers through a private `*_internal.h`,
and this check keeps each such header inside its family, so a folder like
actraiser/enhancements/ stays a real boundary rather than a convention.

Each rule names a private header and the source paths (glob patterns,
relative to the repository root) allowed to include it. Any other tracked C
source or header under src/, tests/, benchmarks/ or tools/ that includes it
fails the check. `--self-test` proves an outside include is reported.
"""

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RULES = {
    'present/present_internal.h': [
        'src/present/*.c', 'tests/present_frame_order_test.c'],
    'sim/sim3d/present_sim3d_internal.h': [
        'src/sim/sim3d/*.c', 'tests/present_world_nav_gpu_test.c'],
    'settings_overlay/settings_overlay_internal.h': [
        'src/settings_overlay/*', 'tests/settings_overlay_test.c'],
    'actraiser/enhancements/actraiser_enhancements_internal.h': [
        'src/actraiser/enhancements/*'],
    'actraiser/actraiser_rtl_internal.h': [
        'src/actraiser/actraiser_rtl.c', 'src/actraiser/actraiser_cheats.c',
        'src/actraiser/enhancements/*'],
    'sim/world_nav/present_world_nav_internal.h': [
        'src/sim/world_nav/present_world_nav*.c', 'src/sim/world_nav/present_sim_globe.c'],
    'action/action_effect_render_internal.h': [
        'src/action/action_effect_render.c', 'src/action/action_scene_effect_render.c',
        'src/action/action_scene_lightning_render.c'],
    'platform/sdl/sim3d_depth_pass_sdl_internal.h': [
        'src/platform/sdl/sim3d_depth_pass*_sdl.c'],
}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)


def violations(files, rules=RULES):
    """files: {repo-relative path: text}. Returns [(path, header)]."""
    by_name = {Path(header).name: header for header in rules}
    found = []
    for path, text in files.items():
        for target in INCLUDE.findall(text):
            header = by_name.get(Path(target).name)
            if header is None:
                continue
            if not any(fnmatch.fnmatch(path, pattern) for pattern in rules[header]):
                found.append((path, header))
    return found


def tracked_sources():
    listed = subprocess.run(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard',
         'src', 'tests', 'benchmarks', 'tools'],
        cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    files = {}
    for path in listed:
        if path.endswith(('.c', '.h', '.inc', '.cpp')) and (ROOT / path).is_file():
            files[path] = (ROOT / path).read_text(errors='replace')
    return files


def self_test():
    cases = [
        ('actraiser/enhancements/actraiser_enhancements_internal.h',
         'src/actraiser/enhancements/actraiser_frame_draw.c'),
        ('present/present_internal.h', 'src/present/present_frame.c'),
        ('sim/sim3d/present_sim3d_internal.h', 'src/sim/sim3d/present_sim3d.c'),
        ('settings_overlay/settings_overlay_internal.h',
         'src/settings_overlay/save_slots/save_slot_menu.c'),
    ]
    for header, owner in cases:
        include = f'#include "{header}"\n'
        found = violations({'src/main.c': include, owner: include})
        if found != [('src/main.c', header)]:
            print(f'self-test failed for {header}: {found}', file=sys.stderr)
            return 1
    print('Private header self-test: outside includes rejected; family includes accepted')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    missing = [header for header in RULES if not (ROOT / 'src' / header).is_file()]
    if missing:
        print('Private header rules name missing files (moved or renamed?):', file=sys.stderr)
        for header in missing:
            print(f'  src/{header}', file=sys.stderr)
        return 1
    found = violations(tracked_sources())
    if found:
        print('Private headers included outside their family:', file=sys.stderr)
        for path, header in found:
            print(f'  {path} includes {header}', file=sys.stderr)
        print('Declare what outsiders need in a public header instead.', file=sys.stderr)
        return 1
    print(f'Private headers: {len(RULES)} checked, each included only by its family')
    return 0


if __name__ == '__main__':
    sys.exit(main())
