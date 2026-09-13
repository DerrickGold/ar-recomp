"""Default-on connected SIM release probe; uses private saves and settings.

Timing uses a visible borderless 1280x800 window and normal presentation cadence.
Separate pixel verification fixes one present per tick through headless-video.
"""
import argparse
import base64
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(binary, output):
    root = Path(__file__).resolve().parents[3]
    fixture = json.loads((root / 'tests/fixtures/sim3d/checkpoints.json').read_text())['checkpoints']['D7-voxel-town']
    env = json.loads(Path(__file__).with_name('environment.json').read_text())
    for key in tuple(env):
        if key.startswith('AR_SHOT_') or key in ('AR_INPUT_REPLAY', 'AR_SAVE_NATIVE_PATH', 'AR_SETTINGS_PATH'):
            del env[key]
    env.update(AR_SIM3D_PITCH='-750', AR_SIM3D_DISTANCE='450', AR_WINDOW_MODE='Borderless',
               AR_WINDOW_SCALE='3', AR_AUDIO_VOLUME='0', AR_SHOW_FPS='Off', AR_INTERP_ENABLE='1')
    output.mkdir(parents=True, exist_ok=False)
    for source, target in ((binary, 'game'), (root / fixture['replay'], 'replay.rec'),
                           (root / fixture['settings'], 'settings.ini'), (Path(__file__), 'probe.py')):
        shutil.copy2(source, output / target)
    seed = base64.b64decode((root / fixture['sram_base64']).read_bytes())
    assert len(seed) == 8192 and hashlib.sha256(seed).hexdigest() == fixture['sram_sha256']
    (output / 'seed.srm').write_bytes(seed)
    manifest = {'environment': env, 'rom_sha256': sha(root / 'ar.sfc'),
                'hashes': {p.name: sha(p) for p in output.iterdir() if p.is_file()}}
    (output / 'inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(output)


def memory():
    available = next(int(line.split()[1]) * 1024 for line in Path('/proc/meminfo').read_text().splitlines()
                     if line.startswith('MemAvailable:'))
    gpu = sum(int(Path(p).read_text()) for pattern in (
        '/sys/class/drm/card[0-9]*/device/mem_info_vram_used',
        '/sys/class/drm/card[0-9]*/device/mem_info_gtt_used') for p in glob.glob(pattern))
    return available, gpu


def platform():
    patterns = ('/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor',
                '/sys/class/drm/card[0-9]*/device/power_dpm_force_performance_level',
                '/sys/class/hwmon/hwmon*/temp1_input', '/sys/class/power_supply/BAT*/status')
    return {'uname': list(os.uname()), 'sensors': {
        p: Path(p).read_text().strip() for pattern in patterns for p in glob.glob(pattern)}}


def summary(log):
    samples, cursor = [], 0
    for block in log.split('[pipeline-perf]')[1:]:
        header = block.splitlines()[0]
        if not header.startswith(' scene=Town 3D ') or ' map=00/04 ' not in header:
            continue
        match = re.search(r'output=(\d+x\d+) frames=(\d+) fps=([\d.]+) cadence-ms=([\d.]+) p95=([\d.]+)', header)
        assert match and match[1] == '1280x800', header
        frames = int(match[2])
        start, cursor = cursor, cursor + frames
        stages = {m[1]: float(m[2]) for m in re.finditer(
            r'\[pipeline-stage\] (.*?) mean-ms=([\d.]+) peak-call-ms=[\d.]+ calls=\d+', block)}
        assert all(k in stages for k in ('PPU + capture', 'presentation', 'SIM underlay'))
        stages['render CPU'] = sum(stages.get(k, 0) for k in (
            'PPU + capture', 'world map build', 'SIM metadata', 'town canvas',
            'frame snapshot', 'upload', 'presentation'))
        counts = {}
        for line in block.splitlines():
            tag = re.match(r'\[(pipeline-work|pipeline-traffic)\] ', line)
            if tag:
                counts.update({f'{tag[1]}/{k}': float(v) for k, v in re.findall(r'([\w/-]+)=([\d.]+)', line)})
        assert counts.get('pipeline-work/fallback') == 0 and counts.get('pipeline-work/failed') == 0
        samples.append(dict(start=start, end=cursor, frames=frames, cadence_ms=float(match[4]),
                            p95_ms=float(match[5]), stages=stages, counts=counts))
    samples = samples[1:][-10:]
    assert len(samples) >= 5, f'Only {len(samples)} settled windows'
    frames = sum(s['frames'] for s in samples)
    intervals = sum(s['frames'] - 1 for s in samples)
    duration = sum((s['frames'] - 1) * s['cadence_ms'] for s in samples)
    result = {'fps': intervals * 1000 / duration, 'cadence_ms': duration / intervals,
              'p95_window_median_ms': statistics.median(s['p95_ms'] for s in samples),
              'frames': frames, 'samples': samples}
    for field in ('stages', 'counts'):
        keys = set().union(*(s[field] for s in samples))
        result[field] = {k: sum(s['frames'] * s[field].get(k, 0) for s in samples) / frames for k in sorted(keys)}
    return result


def run(args):
    root = Path(__file__).resolve().parent
    manifest = json.loads((root / 'inputs.json').read_text())
    rom = Path('/home/deck/argame/ar.sfc')
    assert sha(rom) == manifest['rom_sha256']
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'schema': 'sim-underlay-deck-v1', 'platform': platform(), 'inputs': manifest,
              'verification': args.verify, 'runs': []}
    order = args.order or (['explicit', 'default'] if args.verify else ['flat', 'default', 'default', 'flat'])
    for index, variant in enumerate(order):
        assert all(sha(root / p) == h for p, h in manifest['hashes'].items())
        # Only observe other game processes; never stop another task's run.
        for p in Path('/proc').glob('[0-9]*/comm'):
            try:
                assert p.read_text().strip() not in ('ActRaiserRecomp', 'game'), 'Another game is active'
            except FileNotFoundError:
                pass
        trial = args.output / f'{index}-{variant}'
        trial.mkdir()
        shutil.copy2(root / 'seed.srm', trial / 'seed.srm')
        shutil.copy2(root / 'settings.ini', trial / 'settings.ini')
        env = {k: v for k, v in os.environ.items() if not k.startswith(('AR_', 'SNESRECOMP_'))}
        env.update(manifest['environment'])
        env.update(LD_LIBRARY_PATH='/home/deck/argame', XDG_RUNTIME_DIR='/run/user/1000',
            WAYLAND_DISPLAY='wayland-0', SDL_VIDEODRIVER='wayland',
            AR_INPUT_REPLAY=str(root / 'replay.rec'), AR_SAVE_NATIVE_PATH=str(trial / 'seed.srm'),
            AR_SETTINGS_PATH=str(trial / 'settings.ini'), AR_QUIT_FRAMES=str(args.frames),
            AR_HEADLESS='1' if args.verify else '0', AR_HEADLESS_VIDEO='1' if args.verify else '0', AR_ENABLE_RUN_DIR='1',
            AR_PIPELINE_PERF='1')
        env.pop('AR_SIM3D_GLOBE_CACHE', None)
        env.pop('AR_SIM3D_GLOBE_UNDERLAY', None)
        # Default trial deliberately has no prototype opt-in or cache switch.
        if variant != 'default':
            env['AR_SIM3D_GLOBE_UNDERLAY'] = '0' if variant == 'flat' else '1'
        if args.verify:
            env.update(AR_SHOT_REQUIRE_COMPOSITE='1', AR_SHOT_FROM='1000', AR_SHOT_TO='1400', AR_SHOT_EVERY='100')
        minimum, initial_gpu = memory()
        assert minimum > 6 * 1024**3
        growth, reason, start = 0, None, time.monotonic()
        before = platform()
        with (trial / 'game.log').open('w') as log:
            process = subprocess.Popen([str(root / 'game'), str(rom), '--config', str(root / 'settings.ini')],
                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                while process.poll() is None:
                    available, gpu = memory()
                    minimum, growth = min(minimum, available), max(growth, gpu - initial_gpu)
                    if available < 4 * 1024**3 or growth > 2 * 1024**3:
                        reason = 'memory guard'
                    if time.monotonic() - start > 120:
                        reason = 'timeout'
                    if reason:
                        break
                    time.sleep(.2)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
        record = dict(variant=variant, environment={k: v for k, v in env.items() if k.startswith('AR_')},
                      seconds=time.monotonic()-start, minimum_available=minimum, maximum_gpu_growth=growth,
                      abort_reason=reason, returncode=process.returncode, before=before, after=platform())
        (trial / 'guard.json').write_text(json.dumps(record, indent=2)+'\n')
        log = (trial / 'game.log').read_text()
        assert reason is None and process.returncode == 0, record
        assert not re.search(r'VK_ERROR_[A-Z_]+|Wayland display connection closed|\[fatal-session\]|GPU .* rejected', log)
        assert 'ordered GPU interop enabled' in log and 'enhanced (ready, view=enhanced' in log, 'Missing enhanced GPU path'
        cadence = re.findall(r'\[present-cadence\] tick-presents=(\d+) re-presents=(\d+).*?no-present-no-sleep=(\d+)', log)[-1]
        assert cadence[2] == '0'
        record.update(tick_presents=int(cadence[0]), represents=int(cadence[1]))
        if args.verify:
            assert cadence[:2] == (str(args.frames), '0')
        if variant != 'flat':
            assert '[sim-globe-underlay] source town=4' in log
            assert '[sim-globe-cache]' in log
            assert '[sim-globe-underlay] unavailable' not in log
        run_dir = root / re.search(r'\[run-dir\] (runs/\d+-\d+(?:-\d+)?)(?:\s|$)', log)[1]
        assert int(re.search(r'^frame=(\d+) ', (run_dir / 'dump_state.txt').read_text(), re.M)[1]) == args.frames
        record.update(final_wram_sha256=sha(run_dir / 'dump_wram.bin'), run_dir=str(run_dir))
        if args.verify:
            record['images'] = {p.name: sha(p) for p in run_dir.glob('shot_*.ppm')}
            assert record['images'].keys() == {f'shot_{n}.ppm' for n in range(1000, 1401, 100)}
            assert 'capture=failed' not in log and 'capture=native-framebuffer' not in log
        else:
            record.update(summary(log))
        report['runs'].append(record)
        (args.output / 'results.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f'{index+1}/{len(order)} {variant}: ' + ('captures complete' if args.verify else
            f'{record["fps"]:.2f} workload FPS; {record["stages"]["render CPU"]:.3f} ms render CPU; '
            f'{record["stages"]["SIM underlay"]:.3f} ms underlay'), flush=True)
    assert len({r['final_wram_sha256'] for r in report['runs']}) == 1
    if args.verify:
        assert all(r['images'] == report['runs'][0]['images'] for r in report['runs'])
    report['identical_final_wram'] = True
    (args.output / 'results.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', type=Path, help='Linux binary to package on the host')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=2400)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--order', nargs='+', choices=('flat', 'explicit', 'default'))
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.prepare:
        prepare(args.prepare.resolve(), args.output)
    else:
        run(args)
