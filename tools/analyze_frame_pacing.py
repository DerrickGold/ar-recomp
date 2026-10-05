#!/usr/bin/env python3
"""Analyze AR_FRAME_PACING_TRACE without confusing CPU scopes with GPU timings.

Completion is the return from the backend present call, not physical scanout.
Upload rows without a present are retained: preparation can happen between
refresh deadlines. CPU producer work overlaps presentation and is not additive.
With early swapchain preparation, backend flush/acquire and blit recording
precede the scheduled wait. They are not contained in backend_present_ms,
which measures only the final Present call and its immediate bookkeeping.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def stats(values):
    values = sorted(values)
    if not values:
        return dict(samples=0, mean=0, p50=0, p95=0, p99=0, max=0)
    def percentile(fraction):
        return values[min(len(values)-1, int((len(values)-1)*fraction))]
    return dict(samples=len(values), mean=statistics.fmean(values),
                p50=percentile(.5), p95=percentile(.95), p99=percentile(.99), max=values[-1])


def presentation_fps(presents):
    """Count completed presents over elapsed time, including held source ticks.

    The 1% low is the reciprocal of the mean slowest ceil(1% * intervals)
    frame times, not the reciprocal of p99 or a CPU scope. Epoch transitions
    remain in the measurement so a room-change stall cannot inflate FPS.
    Completion means backend present return, not physical scanout.
    """
    intervals = [b['complete_ns'] - a['complete_ns'] for a, b in zip(presents, presents[1:])]
    if not intervals or any(value <= 0 for value in intervals):
        raise ValueError('FPS requires at least two monotonically completed presents')
    elapsed = sum(intervals) / 1e9
    slow = sorted(intervals, reverse=True)[:math.ceil(len(intervals) / 100)]
    return dict(average=len(intervals) / elapsed,
                one_percent_low=1e9 / statistics.fmean(slow),
                completed_presents=len(presents), elapsed_seconds=elapsed)


def source_cadence(presents, assume_native=False):
    """Measure real held/skipped ticks and fit one constant playout phase/epoch.

    A native tick is correct for any delay in (age-period, age]. Maximum
    interval overlap finds the best stable phase, without counting the normal
    60.0988/60 drift or 90 Hz 1/2 holds as judder. This is a diagnostic lower
    bound on irregular selection, not proof of physical scanout timing.
    Interpolated endpoint IDs are not displayed-frame IDs; exclude them.
    """
    epochs = {}
    for row in presents:
        if row.get('interpolation', 0 if assume_native else 1) == 0:
            epochs.setdefault(row['epoch'], []).append(row)
    samples = repeats = skips = mismatches = 0
    holds = {}
    phases = []
    for epoch, rows in epochs.items():
        samples += len(rows)
        events = {}
        for row in rows:
            age = row['complete_ns'] - row['source_ns']
            period = row.get('interval_ns', 16639263)
            if period <= 0:
                raise ValueError('Invalid source interval')
            lo, hi = age - period + 1, age + 1
            events[lo] = events.get(lo, 0) + 1
            events[hi] = events.get(hi, 0) - 1
        active = best = 0
        phase = 0
        for point, change in sorted(events.items()):
            active += change
            if active > best:
                best, phase = active, point
        mismatches += len(rows) - best
        phases.append(dict(epoch=epoch, fitted_delay_ms=phase / 1e6,
                           samples=len(rows), mismatches=len(rows)-best))
        run = 1
        first_run = True
        for a, b in zip(rows, rows[1:]):
            delta = b['tick'] - a['tick']
            if delta == 0:
                repeats += 1
                run += 1
            else:
                skips += max(0, delta - 1)
                if not first_run:
                    holds[str(run)] = holds.get(str(run), 0) + 1
                first_run, run = False, 1
        # Initial/final holds can be truncated by the requested sample window.
    return dict(samples=samples, held_presents=repeats, skipped_ticks=skips,
                complete_hold_lengths=holds, best_phase_mismatches=mismatches,
                best_phase_mismatch_percent=100 * mismatches / samples if samples else None,
                epochs=phases)


def analyze_sync(rows, path, start, end, refresh, warmup_seconds):
    """Synchronous traces have no packet queue or independent producer timings."""
    first = next((r for r in rows if r['source_ns'] > 0), None)
    if first is None:
        raise ValueError('No synchronous source in trace')
    cutoff = first['sample_ns'] + int(warmup_seconds * 1e9)
    rows = [r for r in rows if start <= r['tick'] <= end and
            r['sample_ns'] >= cutoff and r['source_ns'] > 0]
    if len(rows) < 100:
        raise ValueError('Need at least 100 completed presents in the requested tick range')
    for a, b in zip(rows, rows[1:]):
        if b['sample_ns'] <= a['sample_ns'] or b['complete_ns'] <= a['complete_ns']:
            raise ValueError('Non-monotonic trace')
    if any(r['complete_ns'] < r['sample_ns'] or r['tick_delta'] < 0 for r in rows):
        raise ValueError('Inconsistent synchronous timestamps/ticks')
    sources = {}
    for r in rows:
        key = str(r['pacing_source'])
        sources[key] = sources.get(key, 0) + 1
    return dict(path=str(path), path_kind='synchronous', presents=len(rows),
        presentation_fps=presentation_fps(rows),
        measured_ticks=[rows[0]['tick'], rows[-1]['tick']], pacing_sources=sources,
        epoch_boundaries=sum(a['epoch'] != b['epoch'] for a,b in zip(rows,rows[1:])),
        source_cadence=source_cadence(rows),
        all_complete_interval_ms=stats([(b['complete_ns']-a['complete_ns'])/1e6
                                      for a,b in zip(rows,rows[1:])]),
        source_age_at_complete_ms=stats([(r['complete_ns']-r['source_ns'])/1e6 for r in rows]),
        iteration_ms=stats([(r['complete_ns']-r['sample_ns'])/1e6 for r in rows]),
        zero_tick_presents=sum(r['tick_delta'] == 0 for r in rows),
        multi_tick_presents=sum(r['tick_delta'] > 1 for r in rows))


def analyze(path, start=1200, end=3300, refresh=90, warmup_seconds=0, assume_native=False):
    if refresh <= 0 or end < start or warmup_seconds < 0:
        raise ValueError('Invalid timing range or warmup')
    with Path(path).open() as f:
        rows = [{k:(float(v) if k == 'alpha' else int(v)) for k,v in row.items()}
                for row in csv.DictReader(f)]
    if rows and 'tick_delta' in rows[0]:
        return analyze_sync(rows, path, start, end, refresh, warmup_seconds)
    # The game may enter the room long before the independent presenter starts.
    # Warm up against the first actual streamed endpoint, not a boot tick guess.
    first_source = next((r for r in rows if r['tick'] > 0 and r['source_ns']), None)
    if first_source is None:
        raise ValueError('No streamed source in trace')
    cutoff = first_source['loop_ns'] + int(warmup_seconds * 1e9)
    rows = [r for r in rows if start <= r['tick'] <= end and r['loop_ns'] >= cutoff]
    presents = [r for r in rows if r['presented']]
    if len(presents) < 100:
        raise ValueError('Need at least 100 completed presents in the requested tick range')
    for a,b in zip(rows,rows[1:]):
        if b['loop_ns'] <= a['loop_ns']:
            raise ValueError('Non-monotonic trace')
    for r in rows:
        if not r['loop_ns'] <= r['prepare_ns'] <= r['ready_ns'] <= r['complete_ns']:
            raise ValueError('Inconsistent stage timestamps')
        if r['presented'] and not r['ready_ns'] <= r['draw_ns'] <= r['complete_ns']:
            raise ValueError('Missing/inconsistent draw timestamp')
        if r['vector_wait_ns'] > r['draw_work_ns']:
            raise ValueError('Fence wait is not contained in drawing')
    interval = 1000 / refresh
    def cadence(field):
        return [(b[field]-a[field])/1e6 for a,b in zip(presents,presents[1:]) if a['epoch']==b['epoch']]
    def durations(key, selection=rows):
        return [r[key]/1e6 for r in selection]
    uploads = [r for r in rows if r['uploads']]
    sources = {(r['epoch'],r['tick']):r for r in uploads}
    lateness = [max(0,r['draw_ns']-r['deadline_ns'])/1e6 for r in presents if r['deadline_ns']]
    result = dict(path=str(path), ticks=[start,end], presents=len(presents), uploads=len(uploads),
                  presentation_fps=presentation_fps(presents),
                  warmup_seconds=warmup_seconds, first_stream_tick=first_source['tick'],
                  measured_ticks=[presents[0]['tick'], presents[-1]['tick']],
                  first_present_after_stream_ms=(presents[0]['draw_ns']-first_source['loop_ns'])/1e6,
                  epoch_boundaries=sum(a['epoch']!=b['epoch'] for a,b in zip(presents,presents[1:])),
                  all_complete_interval_ms=stats([(b['complete_ns']-a['complete_ns'])/1e6 for a,b in zip(presents,presents[1:])]),
                  upload_without_present=sum(not r['presented'] for r in uploads),
                  multiple_uploads=sum(r['uploads']>1 for r in uploads),
                  queue_depth=stats([r['queue_before'] for r in rows]),
                  producer_work_ms=stats([(r['producer_complete_ns']-r['producer_start_ns'])/1e6 for r in sources.values()]),
                  producer_start_late_ms=stats([max(0,r['producer_start_ns']-r['source_ns'])/1e6 for r in sources.values()]),
                  input_age_at_draw_ms=stats([(r['draw_ns']-r['input_ns'])/1e6 for r in presents]),
                  source_age_at_draw_ms=stats([(r['draw_ns']-r['source_ns'])/1e6 for r in presents]),
                  deadline_late_ms=stats(lateness),
                  start_interval_ms=stats(cadence('prepare_ns')),
                  draw_interval_ms=stats(cadence('draw_ns')),
                  complete_interval_ms=stats(cadence('complete_ns')),
                  interval_over_budget_plus_1ms=sum(v>interval+1 for v in cadence('complete_ns')),
                  deadline_over_1ms=sum(v>1 for v in lateness),
                  deadline_over_2ms=sum(v>2 for v in lateness),
                  upload_crosses_deadline=sum(r['deadline_ns'] and r['prepare_ns'] < r['deadline_ns'] < r['ready_ns'] for r in uploads),
                  upload_ms=stats(durations('upload_ns',uploads)),
                  draw_work_ms=stats(durations('draw_work_ns',presents)),
                  swap_ms=stats(durations('swap_ns',presents)),
                  vector_wait_ms=stats(durations('vector_wait_ns',presents)),
                  vector_wait_nonzero_ms=stats([r['vector_wait_ns']/1e6 for r in presents if r['vector_wait_ns']]),
                  event_work_ms=stats([(r['prepare_ns']-r['loop_ns'])/1e6 for r in rows]))
    result['source_cadence'] = source_cadence(presents, assume_native)
    if presents and 'pacing_source' in presents[0]:
        result['pacing_sources'] = {str(k):sum(r['pacing_source'] == k for r in presents)
                                    for k in sorted({r['pacing_source'] for r in presents})}
    scheduled_native = [r for r in presents if r.get('interpolation') == 0 and
                        r.get('interval_ns', 0) and r.get('playout_target_ns', 0)]
    result['native_target_late_presents'] = sum(
        r['source_ns'] + r['interval_ns'] <= r['playout_target_ns'] for r in scheduled_native)
    result['native_target_future_presents'] = sum(
        r['source_ns'] > r['playout_target_ns'] for r in scheduled_native)
    result['sample_prediction_error_ms'] = stats([
        (r['complete_ns'] - r['sample_ns']) / 1e6 for r in scheduled_native if r.get('sample_ns', 0)])
    result['source_age_at_complete_ms'] = stats([
        (r['complete_ns'] - r['source_ns']) / 1e6 for r in presents])
    cpu_sources = [r for r in sources.values() if r.get('producer_cpu_ns', 0)]
    result['producer_cpu_ms'] = stats([r['producer_cpu_ns'] / 1e6 for r in cpu_sources])
    # Includes waits, descheduling and clock/scope noise. Not evidence that the
    # thread was runnable the entire time, nor GPU execution time.
    result['producer_unaccounted_wall_ms'] = stats([
        max(0, r['producer_complete_ns'] - r['producer_start_ns'] - r['producer_cpu_ns']) / 1e6
        for r in cpu_sources])
    cpu_draws = [r for r in presents if r.get('draw_cpu_ns', 0)]
    result['draw_cpu_ms'] = stats([r['draw_cpu_ns'] / 1e6 for r in cpu_draws])
    # Excludes helper CPU time. Wall minus owner CPU includes helper joins,
    # driver waits and descheduling; it is not a GPU timer.
    result['draw_unaccounted_wall_ms'] = stats([
        max(0, r['draw_work_ns'] - r['draw_cpu_ns']) / 1e6 for r in cpu_draws])
    for key in ('pump_ns', 'input_events_ns', 'owner_poll_ns'):
        if key in rows[0]: result[key.replace('_ns','_ms')] = stats(durations(key))
    if 'submit_start_ns' in rows[0]:
        for r in presents:
            if not r['draw_ns'] <= r['submit_start_ns'] <= r['complete_ns']:
                raise ValueError('Missing/inconsistent submit timestamp')
        result['submit_interval_ms'] = stats(cadence('submit_start_ns'))
        result['submit_wait_ms'] = stats(durations('submit_wait_ns', presents))
        result['backend_present_ms'] = stats([(r['complete_ns']-r['submit_start_ns'])/1e6 for r in presents])
        result['submit_late_ms'] = stats([max(0,r['submit_start_ns']-r['submit_deadline_ns'])/1e6
                                         for r in presents if r['submit_deadline_ns']])
    for key in ('backend_flush_ns', 'backend_acquire_ns', 'backend_submit_ns'):
        if key in rows[0]: result[key.replace('_ns','_ms')] = stats(durations(key, presents))
    # Preserve the worst intervals with their stage evidence for causal inspection.
    worst = []
    for a,b in zip(presents,presents[1:]):
        if a['epoch'] != b['epoch']: continue
        evidence = dict(tick=b['tick'], interval_ms=(b['complete_ns']-a['complete_ns'])/1e6,
                          upload_ms=b['upload_ns']/1e6, draw_ms=b['draw_work_ns']/1e6,
                          swap_ms=b['swap_ns']/1e6, wait_ms=b['vector_wait_ns']/1e6,
                          source_age_ms=(b['draw_ns']-b['source_ns'])/1e6)
        if b['deadline_ns']:
            evidence['deadline_late_ms'] = max(0,b['draw_ns']-b['deadline_ns'])/1e6
        if 'submit_start_ns' in b:
            evidence['backend_present_ms'] = (b['complete_ns']-b['submit_start_ns'])/1e6
            if b['submit_deadline_ns']:
                evidence['submit_late_ms'] = max(0,b['submit_start_ns']-b['submit_deadline_ns'])/1e6
                # Signed slack: negative means the producer itself finished
                # after this output deadline, before owner-side preparation.
                evidence['source_ready_slack_ms'] = (b['submit_deadline_ns']-b['producer_complete_ns'])/1e6
        for key in ('backend_flush_ns', 'backend_acquire_ns', 'backend_submit_ns'):
            if key in b: evidence[key.replace('_ns','_ms')] = b[key]/1e6
        if b.get('draw_cpu_ns', 0):
            evidence['draw_cpu_ms'] = b['draw_cpu_ns']/1e6
            evidence['draw_unaccounted_wall_ms'] = max(0, b['draw_work_ns']-b['draw_cpu_ns'])/1e6
        worst.append(evidence)
    result['worst_intervals'] = sorted(worst,key=lambda r:r['interval_ms'],reverse=True)[:12]
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('traces',type=Path,nargs='+')
    p.add_argument('--start',type=int,default=1200)
    p.add_argument('--end',type=int,default=3300)
    p.add_argument('--refresh',type=int,default=90)
    p.add_argument('--warmup-seconds',type=float,default=0,
                   help='Exclude time after the first streamed endpoint (e.g. 10), in addition to --start')
    p.add_argument('--output',type=Path)
    p.add_argument('--native', action='store_true', help='Identify legacy traces known to have interpolation disabled')
    p.add_argument('--max-irregular-percent', type=float,
                   help='Fail when the native best-phase mismatch percentage exceeds this limit')
    a=p.parse_args()
    if a.refresh<=0 or a.end<a.start or a.warmup_seconds<0: p.error('Invalid timing range or warmup')
    reports=[analyze(t,a.start,a.end,a.refresh,a.warmup_seconds,a.native) for t in a.traces]
    payload=json.dumps(reports,indent=2)
    if a.output:a.output.write_text(payload+'\n')
    print(payload)
    if a.max_irregular_percent is not None:
        if not 0 <= a.max_irregular_percent <= 100:
            p.error('Irregular percentage must be between 0 and 100')
        rates = [r['source_cadence']['best_phase_mismatch_percent'] for r in reports]
        if any(rate is None for rate in rates):
            p.error('Native source metadata (or --native for a legacy trace) is required')
        if any(rate > a.max_irregular_percent for rate in rates):
            raise SystemExit(1)


if __name__=='__main__':main()
