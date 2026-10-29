#!/usr/bin/env python3
"""Repeat complete-room GPU sweeps; capture runs deliberately have no timings.

Runs locally beside staged room assets. WSL can launch native Windows binaries
with --wsl. --wake-deck wakes/checks the desktop display before EVERY execution.
No save files, settings, emulation, swapchain presentation or gameplay are used.
"""
from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def telemetry() -> dict[str, str]:
    values = {}
    for pattern in (
        '/sys/class/drm/card*/device/hwmon/hwmon*/temp1_input',
        '/sys/class/drm/card*/device/hwmon/hwmon*/power1_average',
        '/sys/class/drm/card*/device/gpu_busy_percent',
        '/sys/class/drm/card*/device/pp_dpm_sclk',
        '/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq',
        '/sys/class/backlight/*/bl_power',
        '/sys/class/backlight/*/actual_brightness',
    ):
        for name in glob.glob(pattern):
            try:
                values[name] = Path(name).read_text().strip()
            except OSError:
                pass
    return values


def display_awake(values: dict[str, str]) -> bool:
    try:
        brightness = [int(v) for k, v in values.items() if k.endswith('/actual_brightness')]
        power = [int(v) for k, v in values.items() if k.endswith('/bl_power')]
    except ValueError:
        return False
    return bool(brightness and power) and all(v > 0 for v in brightness) and all(v == 0 for v in power)


def load_plan(path: Path) -> list[dict]:
    plan = json.loads(path.read_text())
    if not isinstance(plan, list) or not plan:
        raise ValueError('Route plan must be a nonempty list')
    names = set()
    for route in plan:
        if not isinstance(route, dict):
            raise ValueError('Each route must be an object')
        name, room = route.get('name'), route.get('room')
        # Names become directory components; rooms name staged scene files.
        if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', name):
            raise ValueError(f'Invalid route name: {name!r}')
        if name in names:
            raise ValueError(f'Duplicate route name: {name}')
        names.add(name)
        if not isinstance(room, str) or not re.fullmatch(r'[0-9a-fA-F]{4}', room):
            raise ValueError(f'Invalid room for {name}: {room!r}')
        for key in ('start', 'end', 'player'):
            point = route.get(key)
            if (not isinstance(point, list) or len(point) != 2 or
                    any(type(v) is not int or not -(2**31) <= v < 2**31 for v in point)):
                raise ValueError(f'{name}.{key} must contain two signed 32-bit integers')
        if route['player'][0] >= 0:
            for start, end, player in zip(route['start'], route['end'], route['player']):
                if not -(2**31) <= player + end - start < 2**31:
                    raise ValueError(f'Player route exceeds integer coordinates: {name}')
    return plan


def summarize(path: Path, frames: int) -> dict:
    with path.open() as file:
        rows = list(csv.DictReader(file))
    try:
        if frames < 1 or len(rows) != frames or [int(r['frame']) for r in rows] != list(range(frames)):
            raise ValueError('Missing or unordered frames')
        columns = {key: [float(r[key]) for r in rows] for key in
                   ('prepare_ms', 'compose_submit_ms', 'gpu_wait_ms', 'complete_ms')}
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f'Invalid measurement in {path}: {error}') from error
    columns['compose_complete_ms'] = [c + w for c, w in
                                      zip(columns['compose_submit_ms'], columns['gpu_wait_ms'])]
    result = {}
    for key, values in columns.items():
        if not all(math.isfinite(v) and v >= 0 for v in values):
            raise ValueError(f'Invalid {key}: {path}')
        ordered = sorted(values)
        result[key] = dict(mean=statistics.mean(values), median=statistics.median(values),
                           p95=ordered[math.ceil(len(values)*.95)-1],
                           p99=ordered[math.ceil(len(values)*.99)-1])
    # Six-decimal CSV rounding permits a few nanoseconds of error. Reject
    # mismatched totals rather than publishing plausible but corrupt evidence.
    for prepare, compose, complete in zip(columns['prepare_ms'], columns['compose_complete_ms'],
                                         columns['complete_ms']):
        if abs(prepare + compose - complete) > .00001:
            raise ValueError(f'Inconsistent completion time: {path}')
    return result


def run_process(command: list[str], env: dict[str, str], output: Path, timeout: float) -> None:
    samples, started = [], time.monotonic()
    try:
        with (output/'run.log').open('w') as log:
            with subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT) as process:
                try:
                    while process.poll() is None:
                        samples.append(dict(elapsed=time.monotonic()-started, sensors=telemetry()))
                        if time.monotonic()-started > timeout:
                            raise TimeoutError(f'Benchmark timed out: {output}')
                        time.sleep(.5)
                except BaseException:
                    process.kill()
                    process.wait()
                    raise
        if process.returncode:
            raise RuntimeError(f'Benchmark exited with {process.returncode}: {output}/run.log')
    finally:
        # Preserve failure/timeout evidence as well as successful measurements.
        (output/'telemetry.json').write_text(json.dumps(samples, indent=2)+'\n')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, help='Optional paired control executable')
    parser.add_argument('--rooms', type=Path, required=True, help='Directory from export_gpu_rooms.mjs')
    parser.add_argument('--layers', type=Path, required=True)
    parser.add_argument('--effects', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory; existing data is never overwritten')
    parser.add_argument('--plan', type=Path, default=Path(__file__).with_name('benchmark_routes.json'))
    parser.add_argument('--only', default='', help='Comma-separated route names')
    parser.add_argument('--drivers', nargs='+', choices=['metal', 'vulkan', 'direct3d12'], required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--size', type=int, nargs=2, default=[1280, 800])
    parser.add_argument('--mode', choices=['both', 'on', 'off'], default='both')
    parser.add_argument('--final-battle', action='store_true', help='Enable the optional native-nebula material in 07:08')
    parser.add_argument('--wake-deck', action='store_true')
    parser.add_argument('--wsl', action='store_true')
    parser.add_argument('--capture', action='store_true', help='PNG sequence, no performance measurements; use a smaller --size')
    args = parser.parse_args()
    if args.repeats < 1 or not 2 <= args.frames <= 10000 or any(not 1 <= n <= 2048 for n in args.size):
        parser.error('Invalid repeat count, frames or dimensions')
    if args.wsl and args.wake_deck:
        parser.error('--wake-deck is for the Deck desktop, not WSL')
    try:
        plan = load_plan(args.plan)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.only:
        names = set(args.only.split(','))
        if names - {r['name'] for r in plan}:
            parser.error('Unknown route name')
        plan = [r for r in plan if r['name'] in names]
    binaries = [('candidate', args.binary.resolve())]
    if args.baseline:
        binaries.insert(0, ('control', args.baseline.resolve()))
    rooms, layers, effects, output = (p.resolve() for p in (args.rooms, args.layers, args.effects, args.output))
    provenance = dict(plan=plan, layers_sha256=sha(layers), effects_sha256=sha(effects),
                      binaries={label: sha(path) for label, path in binaries},
                      assets={p.name: sha(p) for p in rooms.iterdir() if p.suffix in ('.arscene', '.bin')},
                      warmup_frames=args.frames, dimensions=args.size, capture=args.capture,
                      final_battle=args.final_battle)
    for route in plan:
        if not (rooms/(route['room']+'.arscene')).is_file():
            parser.error(f'Missing scene for {route["name"]}: {route["room"]}.arscene')
        if route['player'][0] >= 0 and not (rooms/'player.bin').is_file():
            parser.error('Player stand-ins require rooms/player.bin')
    output.mkdir(parents=True, exist_ok=False)
    (output/'inputs.json').write_text(json.dumps(provenance, indent=2)+'\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('AR_', 'SDL_', 'SNESRECOMP_', 'LD_PRELOAD'))}
    env.update(AR_BENCH_WIDTH=str(args.size[0]), AR_BENCH_HEIGHT=str(args.size[1]),
               AR_BENCH_WARMUP=str(args.frames), AR_BENCH_STEP='2', AR_BENCH_CLOCK='0',
               AR_BENCH_FINAL_BATTLE=str(int(args.final_battle)))
    if args.wake_deck:
        env.update(DISPLAY=':0', WAYLAND_DISPLAY='wayland-0', XDG_RUNTIME_DIR=f'/run/user/{os.getuid()}',
                   DBUS_SESSION_BUS_ADDRESS=f'unix:path=/run/user/{os.getuid()}/bus', SDL_VIDEO_DRIVER='wayland')
    def native(path: Path) -> str:
        return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip() if args.wsl else str(path)
    summaries = []
    for repeat in range(args.repeats):
        # Alternate backend, control/candidate and off/on order to reduce drift.
        order = -1 if repeat % 2 else 1
        for route in plan:
            for driver in args.drivers[::order]:
                for variant, binary in binaries[::order]:
                    for mode in (['off', 'on'][::order] if args.mode == 'both' else [args.mode]):
                        name = f"{route['name']}-{driver}-{variant}-{mode}-{repeat}"
                        out = output/name
                        out.mkdir()
                        run_env = dict(env, SDL_GPU_DRIVER=driver, AR_BENCH_EFFECTS=str(int(mode == 'on')))
                        if args.wake_deck:
                            display = []
                            for command in (['kscreen-doctor', '--dpms', 'on'], ['kscreen-doctor', '-o']):
                                result = subprocess.run(command, env=run_env, capture_output=True, text=True, check=True)
                                display.append(result.stdout+result.stderr)
                            (out/'display.txt').write_text('\n'.join(display))
                            if not display_awake(telemetry()):
                                raise RuntimeError('Deck backlight is off; no valid performance run')
                        if args.capture:
                            (out/'frames').mkdir()
                            run_env['AR_BENCH_CAPTURE'] = native(out/'frames')
                        if args.wsl:
                            run_env['WSLENV'] = ':'.join(k for k in run_env if k.startswith(('AR_', 'SDL_')))
                        command = [str(binary), native(out/'sweep.csv'), native(layers), native(effects),
                                   native(rooms/(route['room']+'.arscene')),
                                   *map(str, route['start']+route['end']+route['player']+[args.frames])]
                        print(name, flush=True)
                        run_process(command, run_env, out, max(180, args.frames*.5))
                        log = (out/'run.log').read_text()
                        if '[gpu-compile] late' in log.split('[benchmark] timed-begin')[-1]:
                            raise RuntimeError(f'Late shader compilation: {out}')
                        samples = json.loads((out/'telemetry.json').read_text())
                        if args.wake_deck and any(not display_awake(s['sensors']) for s in samples):
                            raise RuntimeError(f'Deck display slept during {name}; discard this run')
                        if not args.capture:
                            if f'backend={driver} ' not in log or '[benchmark] timed-begin' not in log:
                                raise RuntimeError(f'Requested backend/timing not confirmed: {out}')
                            summaries.append(dict(name=name, route=route['name'], driver=driver,
                                                  variant=variant, mode=mode, repeat=repeat,
                                                  timings=summarize(out/'sweep.csv', args.frames)))
                            (output/'summary.json').write_text(json.dumps(summaries, indent=2)+'\n')


if __name__ == '__main__':
    main()
