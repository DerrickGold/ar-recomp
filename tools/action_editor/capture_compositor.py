#!/usr/bin/env python3
"""Capture an isolated real action scene without touching player settings/saves."""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from build_preview import ROOT


def capture(room, output, seed=None, replay=None, warp_at=500, diorama_at=900, quit_frames=1800):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    fixtures = ROOT / 'tests/fixtures/benchmark'
    if not (output / 'game-assets').exists():
        (output / 'game-assets').symlink_to(ROOT / 'game-assets', target_is_directory=True)
    shutil.copyfile(ROOT / 'diorama-layers.ini', output / 'diorama-layers.ini')
    (output / 'seed.srm').write_bytes(base64.b64decode((fixtures / 'action-routes-seed.srm.b64').read_bytes()))
    (output / 'settings.ini').write_text((fixtures / 'action-render-settings.ini').read_text() + '''
window_scale = 1
diorama_camera_mode = Dynamic Cam
diorama_tilt_x_mrad = 0
diorama_tilt_y_mrad = 0
diorama_distance_x100 = 325
diorama_vertical_extend = 64
diorama_skybox = Skybox only
diorama_depth_shade = 0
diorama_hud_flat = Off
gpu_fx_dof = On
gpu_fx_edgeaa = On
gpu_fx_rim = On
gpu_interp_enabled = Off
action_effect_lighting = Off
action_effect_particles = Off
action_environmental_effects = Off
crt_enabled = Off
''')
    (output / 'config.ini').write_text('# isolated shared compositor capture\n')
    inputs = [0] * 2001
    for start, length, buttons in json.loads((fixtures / 'aitos-r4-natural-inputs.json').read_text())['pulses']:
        if start <= 420:
            for f in range(start, start + length):
                inputs[f] = buttons
    (output / 'input.rec').write_bytes(b''.join(struct.pack('<II', f, v) for f, v in enumerate(inputs)))
    if seed:
        shutil.copyfile(seed, output / 'seed.srm')
    if replay:
        shutil.copyfile(replay, output / 'input.rec')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('AR_', 'SNESRECOMP_'))}
    env.update(AR_USER_DATA_DIR=str(output), AR_SAVE_NATIVE_PATH=str(output / 'seed.srm'),
               AR_SETTINGS_PATH=str(output / 'settings.ini'), AR_INPUT_REPLAY=str(output / 'input.rec'),
               AR_HEADLESS='1', AR_HEADLESS_VIDEO='1', AR_ENABLE_RUN_DIR='1', AR_REPLAY_NOSTOP='1',
               AR_QUIT_FRAMES=str(quit_frames), AR_REFRESH_MODE='Unlimited', AR_WS_HEADLESS='1',
               AR_EXTENDED_ASPECT_RATIO='16:10', AR_ASPECT_PAR='Square pixels', AR_DISPLAY_MODE='2',
               AR_WARP=room, AR_WARP_AT=str(warp_at), AR_DIORAMA_AT=str(diorama_at), AR_SHOT_REQUIRE_COMPOSITE='1',
               AR_SHOT_FROM=str(diorama_at + 100), AR_SHOT_TO=str(diorama_at + 100), AR_SHOT_EVERY='1', AR_INF_HP='1',
               AR_DIORAMA_SNAPSHOT=str(output / 'scene.ardi'), AR_DIORAMA_SNAPSHOT_AFTER='101',
               SDL_AUDIODRIVER='dummy')
    with (output / 'capture.log').open('w') as log:
        subprocess.run([str(ROOT / 'build/ActRaiserRecomp'), str(ROOT / 'ar.sfc'),
                        '--config', str(output / 'config.ini')], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    text = (output / 'capture.log').read_text()
    if '[diorama-snapshot] wrote ' not in text or '[fatal-session]' in text:
        raise RuntimeError(f'Capture failed; see {output / "capture.log"}')
    print(output / 'scene.ardi')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--room', default='0101')
    parser.add_argument('--output', type=Path, default=ROOT / 'runs/action-editor-captured-scene/fillmore')
    parser.add_argument('--seed', type=Path, help='Optional native battery save copied into the isolated run')
    parser.add_argument('--replay', type=Path, help='Optional native input replay')
    parser.add_argument('--warp-at', type=int, default=500)
    parser.add_argument('--diorama-at', type=int, default=900)
    parser.add_argument('--quit-frames', type=int, default=1800)
    args = parser.parse_args()
    capture(args.room, args.output, args.seed, args.replay, args.warp_at,
            args.diorama_at, args.quit_frames)
