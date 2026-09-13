#!/usr/bin/env python3
"""Private visible-Deck cadence probe; not the one-tick-per-present harness."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import statistics

import compare_pipeline_performance as common

ROOT = Path(__file__).resolve().parents[1]


def summarize(log):
    samples = []
    for block in log.split('[pipeline-perf]')[1:]:
        header = block.splitlines()[0]
        if not header.startswith(' scene=Action 3D ') or ' map=01/01 ' not in header:
            continue
        match = re.search(r'output=(\d+x\d+) frames=(\d+) fps=([\d.]+) cadence-ms=([\d.]+) p95=([\d.]+)', header)
        if not match:
            raise ValueError('Missing cadence evidence')
        if match[1] != '1280x800':
            raise ValueError(f'Unexpected fullscreen output: {match[1]}')
        stages = {m[1]: {'mean_ms': float(m[2]), 'calls': int(m[3])}
                  for m in re.finditer(r'\[pipeline-stage\] (.*?) mean-ms=([\d.]+) peak-call-ms=[\d.]+ calls=(\d+)', block)}
        work = re.search(r'\[pipeline-work\] ticks=([\d.]+) repre=([\d.]+).*?fallback=([\d.]+) failed=([\d.]+)', block)
        if not work or float(work[3]) or float(work[4]):
            raise ValueError('Missing work counters or fallback/failure')
        samples.append({'presents': int(match[2]), 'fps': float(match[3]),
                        'cadence_ms': float(match[4]), 'p95_ms': float(match[5]),
                        'ticks_per_present': float(work[1]),
                        'represents_per_present': float(work[2]), 'stages': stages})
    samples = samples[1:][-10:]
    if len(samples) < 8:
        raise ValueError('Need eight settled visible scene windows')
    presents = sum(s['presents'] for s in samples)
    intervals = sum(s['presents'] - 1 for s in samples)
    seconds = sum((s['presents'] - 1) * s['cadence_ms'] / 1000 for s in samples)
    stage_names = set().union(*(s['stages'] for s in samples))
    stages = {}
    for name in stage_names:
        elapsed = sum(s['presents'] * s['stages'].get(name, {}).get('mean_ms', 0) for s in samples)
        calls = sum(s['stages'].get(name, {}).get('calls', 0) for s in samples)
        stages[name] = {'ms_per_present': elapsed / presents,
                        'ms_per_call': elapsed / calls if calls else None,
                        'calls': calls}
    return {'windows': len(samples), 'presents': presents,
            'interval_weighted_fps': intervals / seconds,
            'median_window_fps': statistics.median(s['fps'] for s in samples),
            'min_window_fps': min(s['fps'] for s in samples),
            'max_window_fps': max(s['fps'] for s in samples),
            'median_window_p95_ms': statistics.median(s['p95_ms'] for s in samples),
            'ticks_per_present': sum(s['presents'] * s['ticks_per_present'] for s in samples) / presents,
            'stages': stages, 'samples': samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--quit-frames', type=int, default=2000)
    parser.add_argument('--interpolation', required=True, choices=('0', '1'))
    parser.add_argument('--refresh', choices=('Unlimited', 'Vsync'), default='Unlimited')
    parser.add_argument('--order', nargs='+', choices=('control', 'candidate'), default=['control', 'candidate', 'candidate', 'control'])
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    manifest = ROOT / 'tests/fixtures/benchmark/fillmore-skybox-checkpoint.json'
    fixture = json.loads(manifest.read_text())['checkpoints']['fillmore']
    settings = ROOT / fixture['settings']
    seed_file = ROOT / fixture['sram']
    seed = seed_file.read_bytes()
    if len(seed) != 8192 or common.hashlib.sha256(seed).hexdigest() != fixture['sram_sha256']:
        raise ValueError('Seed mismatch')
    replay = ROOT / fixture['replay']
    seed_copy = output / 'seed.srm'
    settings_copy = output / 'settings.ini'
    binaries = {v: ROOT / v for v in set(args.order)}
    inputs = [*binaries.values(), ROOT / 'ar.sfc', ROOT / 'config.ini', ROOT / 'diorama-layers.ini',
              manifest, settings, seed_file, replay, Path(__file__), Path(common.__file__)]
    hashes = {str(p): common.digest(p) for p in inputs}
    env = {k: v for k, v in os.environ.items() if not k.startswith(('AR_', 'SNESRECOMP_'))}
    env.update(fixture['env'])
    env.update(AR_HEADLESS='0', AR_HEADLESS_VIDEO='0', AR_INPUT_REPLAY=str(replay),
               AR_SAVE_NATIVE_PATH=str(seed_copy), AR_SETTINGS_PATH=str(settings_copy),
               AR_QUIT_FRAMES=str(args.quit_frames), AR_ENABLE_RUN_DIR='1', AR_PIPELINE_PERF='1',
               AR_PERFORMANCE_OVERLAY='Off', AR_SHOW_FPS='On', AR_AUDIO_VOLUME='0',
               AR_WINDOW_MODE='Borderless', AR_REFRESH_MODE=args.refresh,
               AR_INTERP_ENABLE=args.interpolation, AR_RENDER_WORKERS='3')
    report = {'schema': 'actraiser-visible-deck-probe-v1',
              'platform': common.deck_platform(), 'input_sha256': hashes,
              'environment': {k: v for k, v in env.items() if k.startswith('AR_')},
              'runs': []}
    print(f'Evidence: {output}', flush=True)
    for i, variant in enumerate(args.order):
        if any(common.digest(Path(p)) != h for p, h in hashes.items()):
            raise ValueError('Inputs changed')
        seed_copy.write_bytes(seed)
        settings_copy.write_bytes(settings.read_bytes())
        log_path = output / f'{i}-{variant}.log'
        with log_path.open('w') as log:
            guard = common.run_guarded([str(binaries[variant]), str(ROOT / 'ar.sfc'),
                                       '--config', str(ROOT / 'config.ini')],
                                      cwd=ROOT, env=env, log=log, timeout=90,
                                      guard_path=output / f'{i}-{variant}-guard.json')
        if any(common.digest(Path(p)) != h for p, h in hashes.items()):
            raise ValueError('Inputs changed during trial')
        log = log_path.read_text()
        if re.search(r'\bVK_ERROR_[A-Z_]+\b|Wayland display connection closed|\[fatal-session\]', log):
            raise ValueError('Graphics/runtime failure')
        evidence = common.run_evidence(log)
        state = (Path(evidence['run_dir']) / 'dump_state.txt').read_text()
        frame = re.search(r'^frame=(\d+) ', state, re.M)
        if not frame or int(frame[1]) != args.quit_frames:
            raise ValueError('Did not complete the requested emulation ticks')
        cadence = re.search(r'\[present-cadence\] tick-presents=(\d+) re-presents=(\d+).*?no-present-no-sleep=(\d+)', log)
        if not cadence or int(cadence[3]) != 0:
            raise ValueError('Missing/invalid normal present completion evidence')
        result = summarize(log)
        result.update(evidence, variant=variant, log=str(log_path), guard=guard,
                      emulation_ticks=int(frame[1]), tick_presents=int(cadence[1]),
                      represents=int(cadence[2]))
        report['runs'].append(result)
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'{i+1}/{len(args.order)} {variant}: {result["interval_weighted_fps"]:.2f} host FPS; '
              f'{result["stages"]["PPU scanout"]["ms_per_call"]:.3f} ms/scanout', flush=True)
    common.verify_runs(report['runs'])
    report['identical_final_wram'] = True
    report['summary'] = {}
    for variant in set(args.order):
        runs = [r for r in report['runs'] if r['variant'] == variant]
        report['summary'][variant] = {
            'median_fps': statistics.median(r['interval_weighted_fps'] for r in runs),
            'min_fps': min(r['interval_weighted_fps'] for r in runs),
            'max_fps': max(r['interval_weighted_fps'] for r in runs),
            'stages': {name: {basis: statistics.median(r['stages'][name][basis] for r in runs)
                              for basis in ('ms_per_present', 'ms_per_call')}
                       for name in ('PPU + capture', 'PPU scanout', 'action scanout', 'upload', 'presentation')}}
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
