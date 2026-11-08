#!/usr/bin/env python3
"""Bounded, resumable software room captures; never declares visual acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def room_plan_hash(room):
    """Match JSON.stringify used by the capture script, including integer floats."""
    def normalize(value):
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, list):
            return [normalize(item) for item in value]
        if isinstance(value, dict):
            return {key: normalize(item) for key, item in value.items()}
        return value
    encoded = json.dumps(normalize(room), ensure_ascii=False, separators=(',', ':'))
    return hashlib.sha256(encoded.encode()).hexdigest()


def complete_capture(result, directory, room, profile, editor_sha):
    windows = [w for w in room['windows'] if not w.get('profiles') or profile in w['profiles']]
    if not (result.get('passed') and result.get('html_sha256') == editor_sha
            and result.get('profiles') == [profile]
            and result.get('room') == room['id']
            and result.get('plan_sha256') == room_plan_hash(room)
            and len(result.get('views', [])) == len(windows)):
        return False
    for window in windows:
        view = next((v for v in result['views'] if v['id'] == window['id']), None)
        if not view or view['bounds'] != window['bounds']:
            return False
        angles = window.get('angles', ['left', 'right'])
        if [v['angle'] for v in view['views']] != angles:
            return False
        if any(abs(v['pitch'] - (0 if v['angle'] == 'front' else window.get('pitch', .25))) > 1e-6 for v in view['views']):
            return False
        if window.get('bg2') and not all('bg2_' + kind in view['native'] for kind in ('native', 'effective')):
            return False
    artifacts = list(result.get('screenshots', [])) + list(result.get('contacts', []))
    for view in result['views']:
        artifacts.extend(view.get('native', {}).values())
        for angle in view['views']:
            artifacts.extend([angle['image'], angle['observed']])
    return bool(artifacts) and all((directory / name).is_file() and (directory / name).stat().st_size for name in artifacts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rooms', help='Comma-separated room ids; default all')
    parser.add_argument('--profiles', default='0')
    parser.add_argument('--keep-going', action='store_true', help='Retain failures and capture unaffected rooms; final exit remains nonzero')
    args = parser.parse_args()
    assert os.getpriority(os.PRIO_PROCESS, 0) == 15, 'Run at nice15'
    plan = json.loads(args.plan.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    rooms = {room['id']: room for room in plan['rooms']}
    order = args.rooms.split(',') if args.rooms else list(rooms)
    html_sha = hashlib.sha256(args.editor.read_bytes()).hexdigest()
    records, start = [], time.monotonic()

    def receipt():
        data = {'editor': str(args.editor.resolve()), 'editor_sha256': html_sha,
                'plan': str(args.plan.resolve()), 'plan_sha256': hashlib.sha256(args.plan.read_bytes()).hexdigest(),
                'elapsed_seconds': time.monotonic() - start, 'actual_nice': 15,
                'records': records, 'visual_review': 'pending'}
        (args.output / 'capture-receipt.json').write_text(json.dumps(data, indent=2) + '\n')

    for room in order:
        assert room in rooms
        for profile in map(int, args.profiles.split(',')):
            out = args.output / room / f't{profile}'
            out.mkdir(parents=True, exist_ok=True)
            old = out / 'result.json'
            expected = sum(not w.get('profiles') or profile in w['profiles'] for w in rooms[room]['windows'])
            if not expected:
                continue
            if old.exists():
                result = json.loads(old.read_text())
                if complete_capture(result, out, rooms[room], profile, html_sha):
                    records.append({'room': room, 'profile': profile, 'status': 'reused_current_complete_capture', 'result': str(old)})
                    receipt()
                    continue
                # Keep the previous failure/evidence metadata before a retry can
                # replace it; captures are never retroactively relabeled passes.
                previous = out / 'previous-attempts' / str(time.time_ns())
                previous.mkdir(parents=True)
                for name in ('result.json', 'capture.log', 'failure.png'):
                    source = out / name
                    if source.is_file():
                        shutil.copy2(source, previous / name)
            env = dict(os.environ, AUDIT_PLAN=str(args.plan.resolve()), AUDIT_PROFILES=str(profile))
            env.pop('AUDIT_WINDOW_IDS', None)
            with (out / 'capture.log').open('w') as log:
                process = subprocess.run(['node', str(Path(__file__).with_name('capture-room.mjs')),
                                          str(args.editor.resolve()), str(out.resolve()), room],
                                         env=env, stdout=log, stderr=subprocess.STDOUT)
            records.append({'room': room, 'profile': profile, 'status': 'captured' if process.returncode == 0 else 'failed',
                            'returncode': process.returncode, 'result': str(old)})
            receipt()
            print(room, profile, process.returncode, round(time.monotonic() - start, 1), flush=True)
            if process.returncode and not args.keep_going:
                return process.returncode
    receipt()
    return 1 if any(record.get('returncode', 0) for record in records) else 0


if __name__ == '__main__':
    raise SystemExit(main())
