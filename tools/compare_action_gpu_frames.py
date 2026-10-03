#!/usr/bin/env python3
"""Capture deterministic scrolling action-room composites for CPU/GPU parity.

Requires a staged game directory (ar.sfc, config.ini, seed.srm, input.rec,
assets/, game-assets/, diorama-layers.ini). Never writes its inputs. Captures
are correctness evidence, NOT performance measurements. Compare with --compare;
numpy accelerates image analysis; Pillow adds PNG previews. Keep raw evidence until differences have
been reviewed. Use the same executable/backend and settings for both variants;
--allow-binary-change explicitly permits a comparison between builds. Free Cam
isolates rendering by default; use --camera-mode 'Dynamic Cam' to check framing.
The comparison binding stays enabled to exercise the separate authentic pass
at camera clamps, even though the walkthrough never opens comparison mode.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from compare_pipeline_performance import capture_evidence, run_evidence, validate_run_completion


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(a):
    root, work, binary = a.assets.resolve(), a.output.resolve(), a.binary.resolve()
    work.mkdir(parents=True, exist_ok=False)
    inputs = {name: sha(root / name) for name in
              ('ar.sfc', 'config.ini', 'seed.srm', 'input.rec', 'diorama-layers.ini')}
    for name in ('assets', 'game-assets'):
        (work / name).symlink_to(root / name, target_is_directory=True)
    for name in ('seed.srm', 'diorama-layers.ini'):
        shutil.copy2(root / name, work / name)
    # Shared boot recording followed by a deterministic right/jump/attack route.
    records = dict(struct.iter_unpack('<II', (root / 'input.rec').read_bytes()))
    for frame in range(900, a.frames + 1):
        records[frame] = 0 if a.stationary else 128 | (1 if frame % 100 < 22 else 0) | (2 if frame % 40 < 10 else 0)
    (work / 'input.rec').write_bytes(b''.join(
        struct.pack('<II', f, records.get(f, 0)) for f in range(a.frames + 1)))
    (work / 'settings.ini').write_text('''save_backend = native-srm
save_edit_armed = Off
save_autobackup = Off
diorama_mode = Off
window_mode = Windowed
window_scale = 1
diorama_camera_mode = Free Cam
diorama_tilt_x_mrad = 0
diorama_tilt_y_mrad = 0
diorama_distance_x100 = 325
diorama_hud_flat = On
gpu_fx_dof = On
gpu_fx_edgeaa = On
gpu_fx_rim = On
crt_enabled = On
diorama_vertical_extend = 64
diorama_skybox = Skybox only
action_effect_lighting = On
action_effect_particles = On
action_environmental_effects = On
bind_key_render_compare = Key 43 Tab
''' .replace('diorama_camera_mode = Free Cam', f'diorama_camera_mode = {a.camera_mode}')
    .replace('diorama_skybox = Skybox only', f'diorama_skybox = {a.skybox}')
    .replace('diorama_tilt_x_mrad = 0', f'diorama_tilt_x_mrad = {a.tilt_x}')
    .replace('diorama_tilt_y_mrad = 0', f'diorama_tilt_y_mrad = {a.tilt_y}')
    .replace('diorama_distance_x100 = 325', f'diorama_distance_x100 = {a.distance}')
    + f'gpu_interp_enabled = {"Off" if a.phase == "source" else "On"}\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('AR_', 'SNESRECOMP_'))}
    env.update(SDL_AUDIODRIVER='dummy', AR_USER_DATA_DIR=str(work),
               AR_SETTINGS_PATH=str(work / 'settings.ini'), AR_SAVE_NATIVE_PATH=str(work / 'seed.srm'),
               AR_INPUT_REPLAY=str(work / 'input.rec'), AR_ENABLE_RUN_DIR='1', AR_REPLAY_NOSTOP='1',
               AR_QUIT_FRAMES=str(a.frames), AR_PERFORMANCE_OVERLAY='Off', AR_REFRESH_MODE='Unlimited',
               AR_FRAME_LIMIT_FPS='0', AR_WINDOW_MODE='Windowed', AR_WS_HEADLESS='1',
               AR_EXTENDED_ASPECT_RATIO='16:10', AR_ASPECT_PAR='Square pixels', AR_DISPLAY_MODE='2',
               AR_WARP=a.room, AR_WARP_AT='500', AR_DIORAMA_AT='900', AR_INF_HP='1',
               AR_NO_KNOCKBACK='0' if a.allow_hits else '1', AR_ACTION_BG_HLE='1', AR_RENDER_WORKERS='3', AR_AUDIO_VOLUME='0',
               AR_HEADLESS='1', AR_HEADLESS_VIDEO='1', AR_MUSIC_REPLACEMENTS='0', AR_FRAME_STREAM='0',
               AR_GPU_FRAME_HANDOFF=a.handoff,
               AR_GPU_EFFECT_PROJECTION=a.effect_projection,
               AR_GPU_BG_DECODE=a.bg_decode,
               AR_GPU_PREPARE_WORKER=str(int(a.prepare_worker)),
               AR_GPU_BG_CAPTURE='owned' if a.variant == 'gpu' else '0',
               AR_GPU_BG_MOTION='owned' if a.variant == 'gpu' else '0',
               AR_FRAME_CAPTURE_TICK_CLOCK='1', AR_SHOT_FROM=str(a.start), AR_SHOT_TO=str(a.end),
               AR_SHOT_EVERY=str(a.every), AR_SHOT_REQUIRE_COMPOSITE='1')
    if a.phase != 'source':
        env['AR_SHOT_PHASE'] = a.phase
    if sys.platform.startswith('linux'):
        env.update(XDG_RUNTIME_DIR='/run/user/1000', WAYLAND_DISPLAY='wayland-0',
                   SDL_VIDEODRIVER='wayland', LD_LIBRARY_PATH=str(root))
    command = [str(binary), str(root / 'ar.sfc'), '--config', str(root / 'config.ini')]
    provenance = dict(binary_sha256=sha(binary), inputs=inputs, command=command,
                      env={k:v for k,v in env.items() if k.startswith(('AR_', 'SDL_')) or
                           k in ('XDG_RUNTIME_DIR', 'WAYLAND_DISPLAY', 'LD_LIBRARY_PATH')},
                      replay_sha256=sha(work / 'input.rec'), settings_sha256=sha(work / 'settings.ini'),
                      variant=a.variant, effect_projection=a.effect_projection, handoff=a.handoff, prepare_worker=a.prepare_worker, bg_decode=a.bg_decode,
                      phase=a.phase, start=a.start, end=a.end, every=a.every)
    (work / 'provenance.json').write_text(json.dumps(provenance, indent=2))
    with (work / 'test.log').open('w') as log:
        result = subprocess.run(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)
    validate_capture(a, work, provenance, result.returncode)


def validate_capture(a, work, provenance, returncode=0):
    """Validate immutable evidence independently of executing the game."""
    binary = Path(provenance['command'][0])
    root = Path(provenance['command'][1]).parent
    inputs = provenance['inputs']
    log = (work / 'test.log').read_text()
    if returncode or re.search(r'\[fatal-session\]|\[missing-mx-variant\]|\[dispatch-oob\]|VK_ERROR_|\[gpu-bg-decode\] native resolve failed', log):
        raise RuntimeError(f'Replay failed: {work / "test.log"}')
    validate_run_completion(log, a.frames)
    if a.variant == 'gpu':
        ownership = re.findall(r'\[gpu-bg-capture\] frames=\d+ rejected-sources=\d+ .* mask=([0-9a-f]+) owned=([0-9a-f]+)', log)
        # Some PPU sources still publish decoded pixels (color math or native
        # post-scanout edits). Confirm actual tile ownership without claiming
        # every source is eligible, and retain each observed mask as evidence.
        if not ownership or not all(int(owned, 16) and not (int(owned, 16) & ~int(mask, 16)) for mask, owned in ownership):
            raise RuntimeError('GPU background ownership was not confirmed')
    if a.variant == 'gpu' and a.bg_decode == 'native' and '[gpu-bg-decode] native storage-buffer compute active' not in log:
        raise RuntimeError('Native background decode was not exercised')
    if a.handoff != '0' and a.phase != 'source' and '[gpu-frame-handoff] native ' not in log:
        raise RuntimeError('Native handoff was not exercised')
    if a.effect_projection == 'resident':
        counts = re.findall(r'\[gpu-effect-projection\] resident-captures=(\d+) metadata-downloads=(\d+) bytes=(\d+) source-draws=(\d+) primitives=(\d+)', log)
        if not counts or not any(int(c[0]) > 0 and int(c[3]) > 0 and int(c[4]) > 0 for c in counts):
            raise RuntimeError('Resident effect projection was not exercised')
        if any(int(c[1]) or int(c[2]) for c in counts):
            raise RuntimeError('Resident replay unexpectedly downloaded motion metadata')
    if a.prepare_worker and '[gpu-prepare-worker] native packing/analysis worker active' not in log:
        raise RuntimeError('Native preparation worker was not exercised')
    evidence = run_evidence(log, work)
    images = capture_evidence(Path(evidence['run_dir']), a.start, a.end, a.every)
    states = {}
    pattern = (r'\[shot-state\] gf=(\d+) room=(\w+) bg1=(-?\d+),(-?\d+) '
               r'bg2=(-?\d+),(-?\d+) phase=([-\d.]+) generated=([0-9a-f]+) gpu=([0-9a-f]+)')
    for m in re.finditer(pattern, log):
        states[m[1]] = dict(room=m[2], bg1=[int(m[3]), int(m[4])], bg2=[int(m[5]), int(m[6])],
                            phase=float(m[7]), generated=int(m[8], 16), gpu=int(m[9], 16))
    if len(states) != len(images) or any(s['room'] != a.room for s in states.values()):
        raise RuntimeError('Missing camera evidence or wrong room')
    ranges = {bg: [max(s[bg][i] for s in states.values()) - min(s[bg][i] for s in states.values())
                   for i in range(2)] for bg in ('bg1', 'bg2')}
    if a.phase != 'source' and not any(s['generated'] for s in states.values()):
        raise RuntimeError('Interpolation was never exercised')
    if a.variant == 'gpu' and a.phase != 'source' and not any(s['gpu'] for s in states.values()):
        raise RuntimeError('GPU interpolation was never exercised')
    if sha(binary) != provenance['binary_sha256'] or any(sha(root / k) != v for k, v in inputs.items()):
        raise RuntimeError('Inputs changed during run')
    report = dict(**provenance, **evidence, images=images, states=states, camera_ranges=ranges,
                  background_ownership=ownership if a.variant == 'gpu' else [])
    (work / 'capture.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(output=str(work), frames=len(images), camera_ranges=ranges,
                          generated_frames=sum(bool(s['generated']) for s in states.values()),
                          gpu_frames=sum(bool(s['gpu']) for s in states.values())), indent=2), flush=True)


def compare(a):
    try:
        import numpy as np
    except ImportError:
        np = None
    try:
        from PIL import Image
    except ImportError:
        Image = None
    cpu, gpu = [json.loads((p / 'capture.json').read_text()) for p in a.compare]
    if cpu['binary_sha256'] != gpu['binary_sha256'] and not a.allow_binary_change:
        raise ValueError('Unmatched binary_sha256; use --allow-binary-change for a build regression comparison')
    for key in ('inputs', 'replay_sha256', 'settings_sha256',
                'phase', 'start', 'end', 'every', 'final_wram_sha256'):
        if cpu[key] != gpu[key]:
            raise ValueError(f'Unmatched {key}; comparison invalid')
    if cpu['images'].keys() != gpu['images'].keys():
        raise ValueError('Unmatched frame sets')
    work = a.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    rows, worst = [], None
    for name in sorted(cpu['images'], key=lambda n: int(n[5:-4])):
        gf = name[5:-4]
        for key in ('room', 'bg1', 'bg2', 'phase'):
            if cpu['states'][gf][key] != gpu['states'][gf][key]:
                raise ValueError(f'Frame {gf} differs in {key}')
        resident_comparison = (cpu.get('effect_projection', 'reference') !=
                               gpu.get('effect_projection', 'reference'))
        if resident_comparison:
            reference, resident = ((gpu, cpu) if cpu.get('effect_projection') == 'resident' else (cpu, gpu))
            for key in ('generated', 'gpu'):
                if reference['states'][gf][key] & ~resident['states'][gf][key]:
                    raise ValueError(f'Frame {gf}: resident candidate mask excludes a reference plane')
        elif cpu['variant'] == gpu['variant'] and cpu['states'][gf] != gpu['states'][gf]:
            raise ValueError(f'Frame {gf}: generated plane masks differ')
        paths = [Path(r['run_dir']) / name for r in (cpu, gpu)]
        images = [p.read_bytes().split(b'\n', 3) for p in paths]
        if images[0][:3] != images[1][:3]:
            raise ValueError(f'Frame {gf}: output dimensions differ')
        c, g = [i[3] for i in images]
        pixels = len(c) // 3
        changed = over_2 = maximum = total = 0
        if c != g:
            if np is not None:
                d = np.abs(np.frombuffer(c, dtype='uint8').astype('int16') -
                           np.frombuffer(g, dtype='uint8').astype('int16')).reshape(-1, 3)
                changed, over_2 = int(np.any(d, axis=1).sum()), int(np.any(d > 2, axis=1).sum())
                maximum, total = int(d.max()), int(d.sum())
            else:
                for i in range(0, len(c), 3):
                    delta = [abs(c[i+j] - g[i+j]) for j in range(3)]
                    peak = max(delta)
                    changed += peak > 0
                    over_2 += peak > 2
                    maximum = max(maximum, peak)
                    total += sum(delta)
        row = dict(gf=int(gf), changed_pixels=changed,
                   pixels=pixels, max_delta=maximum, mean_delta=total / len(c),
                   pixels_over_2=over_2,
                   cpu_generated=cpu['states'][gf]['generated'], gpu_generated=gpu['states'][gf]['generated'])
        rows.append(row)
        score = (row['pixels_over_2'], row['mean_delta'])
        if worst is None or score > worst[0]:
            worst = (score, row, paths)
    if any(worst[0]) and Image is not None and np is not None:
        for tag, path in zip(('cpu', 'gpu'), worst[2]):
            Image.open(path).save(work / f'worst-{tag}.png')
        c, g = [np.asarray(Image.open(p), dtype=np.int16) for p in worst[2]]
        Image.fromarray(np.clip(np.abs(c-g)*16, 0, 255).astype('uint8')).save(work / 'worst-difference-x16.png')
    report = dict(cpu=str(a.compare[0].resolve()), gpu=str(a.compare[1].resolve()),
                  binary_sha256=[cpu['binary_sha256'], gpu['binary_sha256']],
                  phase=cpu['phase'], frames=len(rows),
                  candidate_mask_frames=sum(r['cpu_generated'] != r['gpu_generated'] for r in rows), exact_frames=sum(r['changed_pixels'] == 0 for r in rows),
                  frames_over_2=sum(r['pixels_over_2'] > 0 for r in rows),
                  camera_ranges=cpu['camera_ranges'], worst_frame=worst[1], frames_detail=rows)
    (work / 'comparison.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k != 'frames_detail'}, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path)
    p.add_argument('--validate-existing', type=Path, help='Recheck a completed capture using its recorded settings and input hashes')
    p.add_argument('--assets', type=Path)
    p.add_argument('--output', type=Path)
    p.add_argument('--variant', choices=('cpu', 'gpu'), default='cpu')
    p.add_argument('--room', default='0101')
    p.add_argument('--stationary', action='store_true', help='Remain at the room entrance after the boot recording')
    p.add_argument('--camera-mode', choices=('Free Cam', 'Dynamic Cam'), default='Free Cam',
                   help='Free Cam isolates rendering; Dynamic Cam exercises the gameplay framing clamps')
    p.add_argument('--skybox', choices=('Skybox only', 'Plane + skybox', 'Off'), default='Skybox only')
    p.add_argument('--tilt-x', type=int, default=0)
    p.add_argument('--tilt-y', type=int, default=0)
    p.add_argument('--distance', type=int, default=325)
    p.add_argument('--effect-projection', choices=('reference', 'resident'), default='reference')
    p.add_argument('--handoff', choices=('0', 'unpack', 'all'), default='0')
    p.add_argument('--prepare-worker', action='store_true')
    p.add_argument('--bg-decode', choices=('fragment', 'native'), default='fragment')
    p.add_argument('--phase', choices=('source', '0.25', '0.5', '0.75'), default='source')
    p.add_argument('--allow-hits', action='store_true',
                   help='Keep infinite HP but show normal player colors/hit reactions')
    p.add_argument('--frames', type=int, default=2450)
    p.add_argument('--start', type=int, default=1000)
    p.add_argument('--end', type=int, default=1599)
    p.add_argument('--every', type=int, default=1)
    p.add_argument('--compare', type=Path, nargs=2, metavar=('CPU_RUN', 'GPU_RUN'))
    p.add_argument('--allow-binary-change', action='store_true',
                   help='Compare builds; still require identical assets, replay, settings, camera and gameplay state')
    a = p.parse_args()
    if a.validate_existing:
        work = a.validate_existing.resolve()
        provenance = json.loads((work / 'provenance.json').read_text())
        for key in ('variant', 'effect_projection', 'handoff', 'prepare_worker', 'bg_decode', 'phase', 'start', 'end', 'every'):
            setattr(a, key, provenance[key])
        a.room = provenance['env']['AR_WARP']
        a.frames = int(provenance['env']['AR_QUIT_FRAMES'])
        if (sha(work / 'input.rec') != provenance['replay_sha256'] or
                sha(work / 'settings.ini') != provenance['settings_sha256']):
            raise RuntimeError('Capture inputs changed')
        validate_capture(a, work, provenance)
        return
    if not a.output:
        p.error('--output is required for capture and comparison')
    if a.compare:
        compare(a)
    elif a.binary and a.assets:
        if a.prepare_worker and (a.handoff != 'all' or a.variant != 'gpu' or a.phase == 'source'):
            p.error('Preparation worker requires GPU interpolation and native all handoff')
        if not (0 <= a.start <= a.end <= 65535 and a.every > 0 and a.frames > a.end):
            p.error('Invalid capture frame range')
        capture(a)
    else:
        p.error('Capture requires --binary and --assets')


if __name__ == '__main__':
    main()
