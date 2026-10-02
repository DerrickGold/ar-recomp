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
    'render/localized_text_resources_internal.h': [
        'src/render/localized_text_resources.c', 'src/render/localized_text_presenter.c'],
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
    'action/action_environment_capture_internal.h': [
        'src/action/action_effects.c', 'src/action/action_*_effect_capture.c',
        'src/action/action_environment_scene.c', 'src/action/action_map_effect_scene.c',
        'src/action/action_ray_field_capture.c',
        'src/action/action_moon_field_capture.c',
        'src/action/action_marsh_field_capture.c',
        'src/action/action_castle_field_capture.c',
        'src/action/action_glow_field_capture.c',
        'src/action/action_water_field_capture.c',
        'src/action/action_atmosphere_field_capture.c'],
    'action/action_effect_render_internal.h': [
        'src/action/action_effect_render.c', 'src/action/action_scene_effect_render.c',
        'src/action/action_scenery_shadow.c',
        'src/action/action_authored_effect_render.c', 'src/action/action_authored_fields.c',
        'src/action/action_cave_effect_render.c', 'src/action/action_bloodpool_effect_render.c',
        'src/action/action_bloodpool_detail_render.c',
        'src/action/action_castle_effect_render.c',
        'src/action/action_ray_field_render.c', 'src/action/action_environment_geometry.c',
        'src/action/action_scene_lightning_render.c'],
    'platform/sdl/sim3d_depth_pass_sdl_internal.h': [
        'src/platform/sdl/sim3d_depth_pass*_sdl.c'],
    'actraiser/actraiser_action_room_hle_internal.h': [
        'src/actraiser/actraiser_action_room_loader.c',
        'src/actraiser/actraiser_action_room_graphics.c',
        'src/actraiser/actraiser_action_video_config.c'],
    'actraiser/actraiser_cpu_hle_internal.h': [
        'src/actraiser/*', 'tests/actraiser_*_test.c'],
    'platform/sdl/render_sdl_internal.h': [
        'src/platform/sdl/*', 'tests/render_*_test.c',
        'tests/settings_overlay_test.c', 'tests/present_world_nav_gpu_test.c',
        'tests/sim3d_depth_pass_gpu_test.c', 'tests/diorama_frame_generation_test.c',
        'tools/sim_voxel_model_sheet.c', 'tools/benchmark_model_projection.c',
        'tools/action_editor/replay_compositor.c'],
    'regional/session/regional_session_internal.h': [
        'src/regional/session/*', 'tests/regional_characterization_test.c'],
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


def unregistered_headers(files, rules=RULES):
    """A newly introduced private header must declare its family explicitly."""
    return sorted(path for path in files if path.startswith('src/') and
                  path.endswith('_internal.h') and path[4:] not in rules)


def self_test():
    cases = [
        ('actraiser/enhancements/actraiser_enhancements_internal.h',
         'src/actraiser/enhancements/actraiser_frame_draw.c'),
        ('present/present_internal.h', 'src/present/present_frame.c'),
        ('sim/sim3d/present_sim3d_internal.h', 'src/sim/sim3d/present_sim3d.c'),
        ('settings_overlay/settings_overlay_internal.h',
         'src/settings_overlay/save_slots/save_slot_menu.c'),
        ('actraiser/actraiser_action_room_hle_internal.h',
         'src/actraiser/actraiser_action_room_loader.c'),
        ('actraiser/actraiser_cpu_hle_internal.h', 'src/actraiser/actraiser_miracle.c'),
        ('platform/sdl/render_sdl_internal.h', 'src/platform/sdl/render_sdl.c'),
        ('regional/session/regional_session_internal.h',
         'src/regional/session/regional_session_codec.c'),
    ]
    for header, owner in cases:
        include = f'#include "{header}"\n'
        found = violations({'src/main.c': include, owner: include})
        if found != [('src/main.c', header)]:
            print(f'self-test failed for {header}: {found}', file=sys.stderr)
            return 1
    unknown = {'src/new/feature_internal.h': '', 'src/new/public.h': ''}
    if unregistered_headers(unknown) != ['src/new/feature_internal.h']:
        print('self-test failed: unregistered private header escaped the gate', file=sys.stderr)
        return 1
    print('Private header self-test: outside includes and unregistered families rejected')
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
    files = tracked_sources()
    unregistered = unregistered_headers(files)
    if unregistered:
        print('Private headers need an explicit family rule:', file=sys.stderr)
        for path in unregistered:
            print(f'  {path}', file=sys.stderr)
        return 1
    found = violations(files)
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
