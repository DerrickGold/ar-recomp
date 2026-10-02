#!/usr/bin/env python3
"""Analyze AR_FRAME_PACING_TRACE without confusing CPU scopes with GPU timings.

Completion is the return from the backend present call, not physical scanout.
Upload rows without a present are retained: preparation can happen between
refresh deadlines. CPU producer work overlaps presentation and is not additive.
"""
from __future__ import annotations

import argparse
import csv
import json
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


def analyze(path, start=1200, end=3300, refresh=90):
    with Path(path).open() as f:
        rows = [{k:int(v) for k,v in row.items()} for row in csv.DictReader(f)]
    rows = [r for r in rows if start <= r['tick'] <= end]
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
    for key in ('pump_ns', 'input_events_ns', 'owner_poll_ns'):
        if key in rows[0]: result[key.replace('_ns','_ms')] = stats(durations(key))
    # Preserve the worst intervals with their stage evidence for causal inspection.
    worst = []
    for a,b in zip(presents,presents[1:]):
        if a['epoch'] != b['epoch']: continue
        worst.append(dict(tick=b['tick'], interval_ms=(b['complete_ns']-a['complete_ns'])/1e6,
                          upload_ms=b['upload_ns']/1e6, draw_ms=b['draw_work_ns']/1e6,
                          swap_ms=b['swap_ns']/1e6, wait_ms=b['vector_wait_ns']/1e6,
                          deadline_late_ms=max(0,b['draw_ns']-b['deadline_ns'])/1e6,
                          source_age_ms=(b['draw_ns']-b['source_ns'])/1e6))
    result['worst_intervals'] = sorted(worst,key=lambda r:r['interval_ms'],reverse=True)[:12]
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('traces',type=Path,nargs='+')
    p.add_argument('--start',type=int,default=1200)
    p.add_argument('--end',type=int,default=3300)
    p.add_argument('--refresh',type=int,default=90)
    p.add_argument('--output',type=Path)
    a=p.parse_args()
    if a.refresh<=0 or a.end<a.start: p.error('Invalid timing range')
    reports=[analyze(t,a.start,a.end,a.refresh) for t in a.traces]
    payload=json.dumps(reports,indent=2)
    if a.output:a.output.write_text(payload+'\n')
    print(payload)


if __name__=='__main__':main()
