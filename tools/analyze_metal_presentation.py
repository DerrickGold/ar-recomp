#!/usr/bin/env python3
"""Correlate profile_metal.m callbacks with the game's CPU pacing trace.

Metal command execution intervals can overlap and contain dependency stalls.
Their union is a coverage measurement, NOT a hardware engine-busy counter.
Drawable timestamps are OS-reported presentation, not photon measurements.
"""
import argparse
import bisect
import csv
import json
from pathlib import Path
import statistics

from analyze_frame_pacing import analyze, source_cadence, stats


def read(path):
    with Path(path).open() as source:
        return [{k: v if k == 'kind' else int(v) for k, v in row.items()}
                for row in csv.DictReader(source)]


def union_duration(intervals):
    end = total = 0
    for a, b in sorted(intervals):
        if b < a:
            raise ValueError('Reversed GPU interval')
        total += max(0, b - max(a, end))
        end = max(end, b)
    return total


def output_resources(events, frames, commands, gpu, offset):
    """Associate the terminal copy with actual Metal backing textures.

    An SDL container can cycle its backing; CPU pointer equality is insufficient.
    The writer's timestamps cover its entire command, not just the output pass.
    Encoding before a previous read completes is legal; Metal orders hazards.
    """
    encoders, pending, passes, pending_present, presents, writes = {}, {}, {}, {}, {}, {}
    surfaces = {e['id']: e['value0'] for e in events if e['kind'] == 'surface'}
    for e in events:
        kind = e['kind']
        if kind == 'target':
            p = dict(texture=e['value0'], size=e['value1'], start=e['start_ns'], samples=[])
            encoders[e['end_ns']] = p
            pending.setdefault(e['id'], []).append(p)
        elif kind == 'sample' and e['id'] in encoders:
            encoders[e['id']]['samples'].append((e['value1'], e['value0']))
        elif kind == 'present':
            pending_present[e['id']] = e['value0']
        elif kind == 'commit':
            ps = passes[e['id']] = pending.pop(e['value1'], [])
            presents[e['id']] = pending_present.pop(e['value1'], None)
            for p in ps:
                p['command'] = e['id']
                writes.setdefault(p['texture'], []).append(p)
    for values in writes.values():
        values.sort(key=lambda p: p['start'])
    times = {texture: [p['start'] for p in values] for texture, values in writes.items()}
    rows, previous = [], {}
    for f in frames:
        ps = passes.get(f['command'], [])
        surface = surfaces.get(f['drawable'])
        if (len(ps) != 1 or not surface or ps[0]['texture'] != surface or
                presents.get(f['command']) != surface):
            raise ValueError('Cannot identify terminal drawable render pass')
        p = ps[0]
        sources = {texture for slot, texture in p['samples'] if slot == 0}
        if len(sources) != 1:
            raise ValueError('Ambiguous terminal output sampler')
        source = sources.pop()
        index = bisect.bisect_left(times.get(source, []), p['start']) - 1
        if index < 0:
            raise ValueError('Missing output writer')
        writer = writes[source][index]
        result = gpu.get(writer['command'])
        if (not result or result['end_ns'] != 4 or not result['value0'] or
                result['value1'] < result['value0']):
            raise ValueError('Missing/failed output writer GPU execution')
        if commands[writer['command']]['start_ns'] >= commands[f['command']]['start_ns']:
            raise ValueError('Output writer submitted after terminal copy')
        row = dict(tick=f['tick'], texture=source, writer=writer['command'], terminal=f['command'],
                   writer_command_gpu_ms=(result['value1']-result['value0'])/1e6,
                   writer_queue_ms=(result['value0']+offset-commands[writer['command']]['start_ns'])/1e6,
                   writer_end_to_terminal_start_ms=(f['gpu_start_ns']-result['value1']-offset)/1e6)
        if source in previous:
            row['encode_before_previous_read_end_ms'] = (previous[source]['gpu_end_ns']-writer['start'])/1e6
        previous[source] = f
        rows.append(row)
    keys = ('writer_command_gpu_ms', 'writer_queue_ms', 'writer_end_to_terminal_start_ms',
            'encode_before_previous_read_end_ms')
    return dict(textures=len({r['texture'] for r in rows}), frames=rows,
                metrics={key: stats([r[key] for r in rows if key in r]) for key in keys})


def correlate(events, pacing, start=1600, end=2500):
    # Older probes sampled again after SDL_Quit reset its tick epoch. Ignore
    # teardown samples after the final commit, regardless of their offset.
    last_commit = max((i for i,e in enumerate(events) if e['kind']=='commit'), default=-1)
    clocks = [e for i,e in enumerate(events) if i <= last_commit and
              e['kind'] == 'clock' and e['start_ns'] > 0]
    if not clocks:
        raise ValueError('Missing Metal/SDL clock correlation')
    if any(e['end_ns'] < e['start_ns'] for e in clocks):
        raise ValueError('Reversed Metal/SDL clock bracket')
    offsets = [(e['start_ns'] + e['end_ns']) / 2 - e['value0'] for e in clocks]
    offset = statistics.median(offsets)
    uncertainty = max(e['end_ns'] - e['start_ns'] for e in clocks) / 2
    spread = max(offsets) - min(offsets)
    if spread > 100_000:
        raise ValueError('Metal/SDL clock mapping moved by more than 0.1 ms')
    commands = {e['id']: e for e in events if e['kind'] == 'commit'}
    gpu = {e['id']: e for e in events if e['kind'] == 'gpu'}
    display = {e['id']: e for e in events if e['kind'] == 'display'}
    acquires = sorted((e for e in events if e['kind'] == 'acquire'), key=lambda e: e['start_ns'])
    acquire_times = [e['start_ns'] for e in acquires]
    commits = sorted(commands.values(), key=lambda e: e['start_ns'])
    commit_times = [e['start_ns'] for e in commits]
    presents = [r for r in pacing if r['presented'] and start <= r['tick'] <= end]
    if len(presents) < 2:
        raise ValueError('Insufficient selected presents')
    frames = []
    for row in presents:
        lo = bisect.bisect_left(acquire_times, row['submit_start_ns'])
        hi = bisect.bisect_right(acquire_times, row['complete_ns'])
        if hi - lo != 1:
            raise ValueError('Expected exactly one drawable acquisition per present')
        acq = acquires[lo]
        if (acq['value0'] != 1 or acq['end_ns'] < acq['start_ns'] or
                acq['end_ns'] > row['complete_ns']):
            raise ValueError('Missing drawable or inconsistent acquisition timing')
        if row['backend_swapchain_ns'] < acq['end_ns'] - acq['start_ns']:
            raise ValueError('Drawable acquisition exceeds its CPU swapchain scope')
        a = bisect.bisect_left(commit_times, acq['end_ns'])
        b = bisect.bisect_right(commit_times, row['complete_ns'])
        terminal = [c for c in commits[a:b] if c['value0'] == acq['value1']]
        if len(terminal) != 1:
            raise ValueError('Cannot uniquely identify owner-thread terminal submission')
        command = terminal[0]
        result = gpu.get(command['id'])
        if not result or result['end_ns'] != 4 or not result['value0'] or result['value1'] < result['value0']:
            raise ValueError('Missing/failed terminal GPU execution')
        shown = display.get(acq['id'])
        f = dict(tick=row['tick'], drawable=acq['id'], command=command['id'],
                 submit_ns=command['start_ns'], complete_ns=row['complete_ns'],
                 gpu_start_ns=result['value0'] + offset, gpu_end_ns=result['value1'] + offset,
                 drawable_wait_ms=(acq['end_ns']-acq['start_ns'])/1e6,
                 swapchain_wait_ms=row['backend_swapchain_ns']/1e6,
                 # SDL may wait for its frame-in-flight fence before nextDrawable.
                 swapchain_outside_drawable_ms=(row['backend_swapchain_ns']-
                                               acq['end_ns']+acq['start_ns'])/1e6,
                 queue_to_terminal_gpu_ms=(result['value0']+offset-command['start_ns'])/1e6,
                 terminal_gpu_ms=(result['value1']-result['value0'])/1e6,
                 api_return_to_gpu_end_ms=(result['value1']+offset-row['complete_ns'])/1e6,
                 display_callback=shown is not None,
                 display_ns=shown['value0']+offset if shown and shown['value0'] else None)
        if f['display_ns'] is not None:
            f['gpu_end_to_display_ms'] = (f['display_ns']-f['gpu_end_ns'])/1e6
            f['api_return_to_display_ms'] = (f['display_ns']-row['complete_ns'])/1e6
            if row.get('source_ns', 0):
                f['source_age_at_display_ms'] = (f['display_ns']-row['source_ns'])/1e6
        frames.append(f)
    first, last = presents[0]['submit_start_ns'], presents[-1]['complete_ns']
    selected = [c for c in commits if first <= c['start_ns'] <= last]
    valid = [gpu[c['id']] for c in selected if c['id'] in gpu and gpu[c['id']]['value0']]
    if any(g['end_ns'] != 4 or g['value1'] < g['value0'] for g in valid):
        raise ValueError('GPU error or reversed command timing')
    states = [e for e in events if e['kind'] == 'state' and first <= e['start_ns'] <= last]
    shown = sorted((f for f in frames if f['display_ns'] is not None), key=lambda f: f['display_ns'])
    result = dict(clock_offset_ns=offset, clock_offset_spread_ns=spread,
                  clock_bracket_uncertainty_ns=uncertainty,
                  presents=len(frames), displayed=len(shown),
                  duplicate_display_timestamps=len(shown)-len({f['display_ns'] for f in shown}),
                  not_displayed=sum(f['display_callback'] and f['display_ns'] is None for f in frames),
                  missing_display_callbacks=sum(not f['display_callback'] for f in frames),
                  state_samples=len(states),
                  foreground_samples=sum(e['value0'] == 1 for e in states),
                  unlocked_console_samples=sum(e['value1'] == 3 for e in states),
                  timing_eligible=bool(states) and all(e['value0'] == 1 and e['value1'] == 3 for e in states),
                  commands=len(selected), completed_commands=len(valid),
                  command_execution_ms=stats([(g['value1']-g['value0'])/1e6 for g in valid]),
                  displayed_interval_ms=stats([(b['display_ns']-a['display_ns'])/1e6
                                                for a,b in zip(shown,shown[1:])]))
    for key in ('drawable_wait_ms','swapchain_wait_ms','swapchain_outside_drawable_ms',
                'queue_to_terminal_gpu_ms','terminal_gpu_ms','api_return_to_gpu_end_ms',
                'gpu_end_to_display_ms','api_return_to_display_ms','source_age_at_display_ms'):
        result[key] = stats([f[key] for f in frames if key in f])
    native = [dict(row, complete_ns=f['display_ns']) for row,f in zip(presents,frames)
              if row.get('interpolation') == 0 and f['display_ns'] is not None]
    if native and not result['duplicate_display_timestamps']:
        result['native_source_cadence_at_display'] = source_cadence(
            sorted(native, key=lambda r: r['complete_ns']))
    intervals = [(max(first,g['value0']+offset), min(last,g['value1']+offset)) for g in valid
                 if g['value0']+offset < last and g['value1']+offset > first]
    result['command_execution_coverage_percent'] = 100*union_duration(intervals)/(last-first)
    result['coverage_is_hardware_busy'] = False
    result['worst_drawable_waits'] = sorted(frames,key=lambda f:f['drawable_wait_ms'],reverse=True)[:8]
    result['frames'] = frames
    if any(e['kind'] == 'surface' for e in events):
        result['output_resources'] = output_resources(events, frames, commands, gpu, offset)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('metal', type=Path)
    parser.add_argument('pacing', type=Path)
    parser.add_argument('--start', type=int, default=1600)
    parser.add_argument('--end', type=int, default=2500)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = correlate(read(args.metal), read(args.pacing), args.start, args.end)
    result['host'] = analyze(args.pacing, start=args.start, end=args.end)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in
                     ('host','frames','worst_drawable_waits','output_resources')},indent=2))


if __name__ == '__main__':
    main()
