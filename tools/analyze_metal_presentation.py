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


def scheduling_events(events, frames, commands):
    """Separate CPU callback delivery, the actual drawable request and display.

    Native drawable IDs and object identities are both needed: object pointers
    are reused. Nested public present methods are one request, not two frames.
    Scheduled handlers have CPU delivery latency; they are not GPU timestamps.
    """
    acquired = {e['id']: e for e in events if e['kind'] == 'drawable'}
    scheduled = {e['id']: e for e in events if e['kind'] == 'scheduled'}
    acquire_cpu = {e['id']: e for e in events if e['kind'] == 'acquire_cpu'}
    requests = {}
    for e in events:
        if e['kind'] in ('drawable_present', 'drawable_at_time', 'drawable_after_duration'):
            if e['end_ns'] < e['start_ns']:
                raise ValueError('Reversed drawable presentation request')
            requests.setdefault((e['id'], e['value0']), []).append(e)
    rows = []
    for f in frames:
        row = dict(tick=f['tick'], drawable=f['drawable'], command=f['command'])
        command = commands[f['command']]
        callback = scheduled.get(f['command'])
        if callback:
            row['scheduled_callback_after_commit_ms'] = (callback['start_ns']-command['start_ns'])/1e6
            row['scheduled_callback_after_gpu_end_ms'] = (callback['start_ns']-f['gpu_end_ns'])/1e6
        cpu = acquire_cpu.get(f['drawable'])
        if cpu:
            row['drawable_wait_cpu_ms'] = cpu['value0']/1e6
        acq = acquired.get(f['drawable'])
        if acq:
            candidates = sorted((e for e in requests.get((acq['end_ns'], acq['value0']), [])
                                 if e['start_ns'] >= acq['start_ns']), key=lambda e:e['start_ns'])
            if candidates:
                request = candidates[0]
                if any(e['end_ns'] > request['end_ns'] for e in candidates[1:]):
                    raise ValueError('Multiple nonnested requests for one drawable')
                row.update(native_drawable=acq['end_ns'],
                           request_start_ns=request['start_ns'], request_end_ns=request['end_ns'],
                           request_kind=request['kind'],
                           request_after_commit_ms=(request['start_ns']-command['start_ns'])/1e6,
                           request_call_ms=(request['end_ns']-request['start_ns'])/1e6,
                           request_after_gpu_end_ms=(request['start_ns']-f['gpu_end_ns'])/1e6)
                if f['display_ns'] is not None:
                    row['request_return_to_display_ms'] = (f['display_ns']-request['end_ns'])/1e6
        rows.append(row)
    metrics = ('scheduled_callback_after_commit_ms', 'scheduled_callback_after_gpu_end_ms',
               'drawable_wait_cpu_ms', 'request_after_commit_ms', 'request_call_ms',
               'request_after_gpu_end_ms', 'request_return_to_display_ms')
    return dict(frames=rows, missing_requests=sum('request_start_ns' not in r for r in rows),
                missing_scheduled_callbacks=sum('scheduled_callback_after_commit_ms' not in r for r in rows),
                missing_acquire_cpu_samples=sum('drawable_wait_cpu_ms' not in r for r in rows),
                metrics={key:stats([r[key] for r in rows if key in r]) for key in metrics})


def pipeline_events(events, first, last):
    """Native creation/binding wall times, including SDL's internal PSOs.

    A successful creation can be a driver cache hit. Bindings identify first
    use in this process, not a hardware compiler cache miss. Match each bind
    to the latest completed creation of its kind/address: addresses can recur.
    Missing coverage stays explicit rather than becoming 'zero compilations'.
    """
    creation_kinds = ('library_source', 'library_data', 'pipeline_render', 'pipeline_compute')
    scopes = [e for e in events if e['kind'] == 'pipeline_scope']
    if not scopes:
        raise ValueError('Pipeline tracing was not enabled')
    if any(e['value1'] != 1 for e in scopes) or last < first:
        raise ValueError('Unsupported pipeline scope or reversed selection')
    devices = {e['id'] for e in scopes}
    calls = [e for e in events if e['kind'] in creation_kinds]
    binds = sorted((e for e in events if e['kind'] in ('bind_render', 'bind_compute')),
                   key=lambda e: e['start_ns'])
    if any(e['end_ns'] < e['start_ns'] for e in calls + binds):
        raise ValueError('Reversed pipeline timing')
    overlaps = lambda e: e['start_ns'] <= last and e['end_ns'] >= first
    histories = {}
    for i, e in enumerate(calls):
        if e['id'] and e['kind'].startswith('pipeline_'):
            histories.setdefault((e['kind'], e['id']), []).append((e['end_ns'], i))
    for history in histories.values():
        history.sort()
    ends = {key: [end for end, _ in history] for key, history in histories.items()}
    used, first_uses, unmatched = set(), [], []
    for e in binds:
        key = (e['kind'].replace('bind_', 'pipeline_'), e['id'])
        index = bisect.bisect_right(ends.get(key, []), e['start_ns']) - 1
        if index < 0:
            unmatched.append(e)
            continue
        _, birth = histories[key][index]
        if birth not in used:
            used.add(birth)
            first_uses.append(dict(e, created_ns=calls[birth]['end_ns']))
    selected_calls = [e for e in calls if overlaps(e)]
    selected_binds = [e for e in binds if overlaps(e)]
    selected_first = [e for e in first_uses if overlaps(e)]
    observed_kinds = {e['kind'] for e in calls}
    unknown_devices = sum(e['value1'] not in devices for e in calls)
    return dict(
        window_start_ns=first, window_end_ns=last,
        coverage_complete=bool(binds) and not unmatched and not unknown_devices and
            {'pipeline_render', 'pipeline_compute'} <= observed_kinds and
            bool({'library_source', 'library_data'} & observed_kinds),
        creation_calls=len(calls), failed_creations=sum(not e['id'] for e in calls),
        unscoped_device_calls=unknown_devices,
        creation_calls_in_window=len(selected_calls), creations_in_window=selected_calls,
        creation_api_ms={kind: stats([(e['end_ns']-e['start_ns'])/1e6 for e in calls
                                     if e['kind'] == kind]) for kind in creation_kinds},
        last_creation_end_ns=max((e['end_ns'] for e in calls), default=None),
        pipeline_generations_bound=len(used), binding_calls=len(binds),
        unmatched_bindings=len(unmatched), unmatched_binding_examples=unmatched[:8],
        first_binds_in_window=selected_first,
        last_first_bind_ns=max((e['start_ns'] for e in first_uses), default=None),
        binding_api_ms=stats([(e['end_ns']-e['start_ns'])/1e6 for e in selected_binds]),
        slow_bindings_in_window=[e for e in selected_binds if e['end_ns']-e['start_ns'] >= 1_000_000],
        measures_driver_cache_misses=False)


def wait_events(events, first, last):
    """Existing Metal waits/allocations, separating wall time from thread CPU.

    Off-CPU time includes both blocking and descheduling. It is not a GPU busy
    measurement or proof of a particular kernel dependency.
    """
    scopes = [e for e in events if e['kind'] == 'wait_scope']
    if not scopes or any(e['value1'] != 1 for e in scopes) or last < first:
        raise ValueError('Missing/unsupported wait scope or reversed selection')
    kinds = ('wait_complete', 'queue_command', 'render_encoder')
    calls = [e for e in events if e['kind'] in kinds and
             e['start_ns'] <= last and e['end_ns'] >= first]
    if any(e['end_ns'] < e['start_ns'] for e in calls):
        raise ValueError('Reversed native wait timing')
    counters = {kind: {(e['id'], e['start_ns'], e['end_ns']): e for e in events if e['kind'] == kind}
                for kind in ('scope_faults', 'scope_switches')}
    rows = []
    for e in calls:
        row = dict(kind=e['kind'], object=e['id'], thread=e['value1'],
                   start_ns=e['start_ns'], end_ns=e['end_ns'],
                   wall_ms=(e['end_ns']-e['start_ns'])/1e6)
        if e['value0'] != (1 << 64)-1:
            row['cpu_ms'] = e['value0']/1e6
            row['off_cpu_ms'] = max(0, row['wall_ms']-row['cpu_ms'])
        key = (e['id'], e['start_ns'], e['end_ns'])
        faults, switches = counters['scope_faults'].get(key), counters['scope_switches'].get(key)
        if faults:
            row.update(process_major_faults=faults['value0'], process_minor_faults=faults['value1'])
        if switches:
            row.update(process_voluntary_switches=switches['value0'],
                       process_involuntary_switches=switches['value1'])
        rows.append(row)
    return dict(calls=len(rows), missing_cpu_samples=sum('cpu_ms' not in r for r in rows),
                process_memory_observed=any(e['kind'] == 'memory_scope' for e in events),
                metrics={kind: {key: stats([r[key] for r in rows if r['kind'] == kind and key in r])
                                for key in ('wall_ms', 'cpu_ms', 'off_cpu_ms')} for kind in kinds},
                long_calls=[r for r in rows if r['wall_ms'] >= 1])


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
    if any(e['kind'] == 'drawable' for e in events):
        result['scheduling'] = scheduling_events(events, frames, commands)
    if any(e['kind'] == 'pipeline_scope' for e in events):
        result['pipelines'] = pipeline_events(events, presents[0].get('loop_ns', first),
                                             max([last] + [f['display_ns'] for f in shown]))
    if any(e['kind'] == 'wait_scope' for e in events):
        result['native_waits'] = wait_events(events, presents[0].get('loop_ns', first), last)
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
                     ('host','frames','worst_drawable_waits','output_resources','scheduling')},indent=2))


if __name__ == '__main__':
    main()
