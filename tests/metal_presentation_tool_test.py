"""Reject ambiguous CPU/GPU/display associations instead of inventing latency."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_metal_presentation import correlate, pipeline_events, union_duration, wait_events


class MetalPresentationTest(unittest.TestCase):
    def test_native_waits_separate_thread_cpu_and_missing_measurements(self):
        events = [dict(kind='wait_scope', id=1, start_ns=1, end_ns=0, value0=7, value1=1)]
        def call(kind, start, end, cpu):
            events.append(dict(kind=kind, id=10, start_ns=start, end_ns=end, value0=cpu, value1=99))
        call('wait_complete', 2_000_000, 32_000_000, 50_000)
        call('queue_command', 33_000_000, 35_000_000, (1 << 64)-1)
        call('render_encoder', 36_000_000, 36_001_000, 1100)
        call('wait_complete', 100_000_000, 200_000_000, 1000)  # Outside selection.
        events.append(dict(kind='scope_faults', id=10, start_ns=2_000_000, end_ns=32_000_000,
                           value0=3, value1=5))
        result = wait_events(events, 1_000_000, 40_000_000)
        self.assertEqual(result['calls'], 3)
        self.assertEqual(result['missing_cpu_samples'], 1)
        self.assertEqual(len(result['long_calls']), 2)
        wait = result['long_calls'][0]
        self.assertEqual(wait['wall_ms'], 30)
        self.assertAlmostEqual(wait['off_cpu_ms'], 29.95)
        self.assertEqual(wait['process_major_faults'], 3)
        self.assertEqual(wait['process_minor_faults'], 5)
        self.assertNotIn('process_major_faults', result['long_calls'][1])
        self.assertNotIn('cpu_ms', result['long_calls'][1])
        self.assertEqual(result['metrics']['render_encoder']['off_cpu_ms']['max'], 0)
        with self.assertRaisesRegex(ValueError, 'Missing/unsupported'):
            wait_events(events[1:], 0, 40_000_000)

    def evidence(self):
        events, pacing = [], []
        def event(kind, ident, start, end=0, a=0, b=0):
            events.append(dict(kind=kind,id=ident,start_ns=start,end_ns=end,value0=a,value1=b))
        event('clock',0,1000,1000,1_001_000)
        event('state',0,12000,0,1,3)
        for i in range(2):
            t=10000+i*10000
            event('acquire',i,t+1000,t+2000,1,99)
            event('commit',i,t+3000,t+4000,99)
            # A worker submission inside the same CPU window is not the terminal one.
            event('commit',i+10,t+3100,t+3900,100)
            event('gpu',i,t+9000,4,t+1_005_000,t+1_006_000)
            event('gpu',i+10,t+9000,4,t+1_005_000,t+1_005_500)
            event('display',i,t+15000,0,t+1_012_000)
            pacing.append(dict(presented=1,tick=1600+i,submit_start_ns=t,complete_ns=t+4500,
                               backend_swapchain_ns=1200))
        return events,pacing

    def test_distinguishes_gpu_completion_display_and_callback(self):
        result=correlate(*self.evidence())
        self.assertTrue(result['timing_eligible'])
        self.assertEqual(result['displayed'],2)
        self.assertAlmostEqual(result['terminal_gpu_ms']['mean'],.001)
        self.assertAlmostEqual(result['gpu_end_to_display_ms']['mean'],.006)
        self.assertAlmostEqual(result['api_return_to_display_ms']['mean'],.0075)
        self.assertEqual(result['displayed_interval_ms']['mean'],.01)
        self.assertFalse(result['coverage_is_hardware_busy'])

    def test_zero_presentation_is_not_reported_as_displayed(self):
        events,rows=self.evidence()
        next(e for e in events if e['kind']=='display')['value0']=0
        result=correlate(events,rows)
        self.assertEqual(result['not_displayed'],1)
        self.assertEqual(result['displayed'],1)
        self.assertEqual(result['displayed_interval_ms']['samples'],0)

    def test_native_cadence_uses_display_time_and_excludes_duplicate_timestamps(self):
        events, rows=self.evidence()
        for row in rows:
            row.update(source_ns=row['submit_start_ns']-5000, interpolation=0,
                       interval_ns=10000, epoch=1)
        result=correlate(events,rows)
        self.assertAlmostEqual(result['source_age_at_display_ms']['mean'],.017)
        self.assertEqual(result['native_source_cadence_at_display']['samples'],2)
        self.assertEqual(result['native_source_cadence_at_display']['best_phase_mismatches'],0)
        shown=[e for e in events if e['kind']=='display']
        shown[1]['value0']=shown[0]['value0']
        result=correlate(events,rows)
        self.assertEqual(result['duplicate_display_timestamps'],1)
        self.assertNotIn('native_source_cadence_at_display',result)

    def test_rejects_ambiguous_owner_submission(self):
        events,rows=self.evidence()
        next(e for e in events if e['kind']=='commit' and e['id']==10)['value0']=99
        with self.assertRaisesRegex(ValueError,'uniquely identify'):
            correlate(events,rows)

    def test_missing_callback_and_locked_session_remain_explicit(self):
        events,rows=self.evidence()
        events=[e for e in events if e['kind']!='display']
        next(e for e in events if e['kind']=='state')['value1']=7
        result=correlate(events,rows)
        self.assertFalse(result['timing_eligible'])
        self.assertEqual(result['missing_display_callbacks'],2)
        self.assertEqual(result['gpu_end_to_display_ms']['samples'],0)

    def test_rejects_hidden_drawable_and_inconsistent_cpu_scope(self):
        events,rows=self.evidence()
        acq=next(e for e in events if e['kind']=='acquire')
        acq['value0']=0
        with self.assertRaisesRegex(ValueError,'Missing drawable'):
            correlate(events,rows)
        acq['value0']=1
        rows[0]['backend_swapchain_ns']=900
        with self.assertRaisesRegex(ValueError,'exceeds its CPU'):
            correlate(events,rows)

    def test_rejects_missing_or_failed_gpu_completion(self):
        events,rows=self.evidence()
        completion=next(e for e in events if e['kind']=='gpu' and e['id']==0)
        completion['end_ns']=5
        with self.assertRaisesRegex(ValueError,'Missing/failed terminal'):
            correlate(events,rows)
        events.remove(completion)
        with self.assertRaisesRegex(ValueError,'Missing/failed terminal'):
            correlate(events,rows)

    def resource_evidence(self, cycle=False):
        events, rows = self.evidence()
        for i in range(2):
            t = 10000+i*10000
            source = 500+i if cycle else 500
            def event(kind, ident, time, end=0, a=0, b=0):
                events.append(dict(kind=kind,id=ident,start_ns=time,end_ns=end,value0=a,value1=b))
            # Reused command/encoder identities must not merge different frames.
            event('target',2000,t+50,4000,source,(1920<<32)|1080)
            event('commit',20+i,t+100,t+110,99,2000)
            event('gpu',20+i,t+9999,4,t+1_001_000,t+1_001_800)
            event('surface',i,t+2000,6000,10000+i,7000)
            event('target',1000,t+2100,5000,10000+i,(1920<<32)|1080)
            event('sample',5000,t+2200,0,source,0)
            event('present',1000,t+2500,0,10000+i,6000)
            next(e for e in events if e['kind']=='commit' and e['id']==i)['value1']=1000
        events.sort(key=lambda e: e['start_ns'])
        return events, rows

    def test_distinguishes_cycled_backings_and_reused_encoder_addresses(self):
        same=correlate(*self.resource_evidence())['output_resources']
        cycled=correlate(*self.resource_evidence(True))['output_resources']
        self.assertEqual(same['textures'],1)
        self.assertEqual(cycled['textures'],2)
        self.assertEqual([f['writer'] for f in same['frames']],[20,21])
        self.assertEqual(same['metrics']['encode_before_previous_read_end_ms']['samples'],1)
        self.assertEqual(cycled['metrics']['encode_before_previous_read_end_ms']['samples'],0)

    def test_rejects_incomplete_resource_associations(self):
        events, rows=self.resource_evidence()
        sample=next(e for e in events if e['kind']=='sample')
        events.remove(sample)
        with self.assertRaisesRegex(ValueError,'output sampler'):
            correlate(events,rows)
        events, rows=self.resource_evidence()
        next(e for e in events if e['kind']=='surface')['value0']=1234
        with self.assertRaisesRegex(ValueError,'drawable render pass'):
            correlate(events,rows)

    def scheduling_evidence(self):
        events, rows = self.evidence()
        for i in range(2):
            t = 10000+i*10000
            def event(kind, ident, start, end=0, a=0, b=0):
                events.append(dict(kind=kind, id=ident, start_ns=start, end_ns=end, value0=a, value1=b))
            # Native object address repeats; the drawable ID must disambiguate it.
            event('drawable', i, t+2000, 100+i, 6000, 7000)
            event('scheduled', i, t+7000, 3, 101)
            event('drawable_present', 100+i, t+6500, t+6800, 6000, 101)
            event('drawable_at_time', 100+i, t+6600, t+6700, 6000, 0)
            event('acquire_cpu', i, t+1000, t+2000, 100)
        return events, rows

    def test_scheduling_separates_gpu_time_callback_and_actual_present(self):
        result = correlate(*self.scheduling_evidence())['scheduling']
        self.assertEqual(result['missing_requests'], 0)
        self.assertEqual(result['missing_scheduled_callbacks'], 0)
        self.assertEqual(result['missing_acquire_cpu_samples'], 0)
        metrics = result['metrics']
        self.assertAlmostEqual(metrics['drawable_wait_cpu_ms']['mean'], .0001)
        self.assertAlmostEqual(metrics['request_after_gpu_end_ms']['mean'], .0005)
        self.assertAlmostEqual(metrics['scheduled_callback_after_gpu_end_ms']['mean'], .001)
        self.assertAlmostEqual(metrics['request_call_ms']['mean'], .0003)
        self.assertAlmostEqual(metrics['request_return_to_display_ms']['mean'], .0052)
        self.assertEqual([r['native_drawable'] for r in result['frames']], [100, 101])

    def test_scheduling_does_not_invent_missing_requests(self):
        events, rows = self.scheduling_evidence()
        events = [e for e in events if e['kind'] not in ('drawable_present', 'drawable_at_time', 'scheduled')]
        result = correlate(events, rows)['scheduling']
        self.assertEqual(result['missing_requests'], 2)
        self.assertEqual(result['missing_scheduled_callbacks'], 2)
        self.assertEqual(result['metrics']['request_call_ms']['samples'], 0)

    def test_scheduling_rejects_multiple_nonnested_requests(self):
        events, rows = self.scheduling_evidence()
        event = next(e for e in events if e['kind'] == 'drawable_at_time')
        event['start_ns'] += 1000
        event['end_ns'] += 1000
        with self.assertRaisesRegex(ValueError, 'Multiple nonnested'):
            correlate(events, rows)

    def test_scheduling_cpu_clock_failure_is_not_zero_cpu_usage(self):
        events, rows = self.scheduling_evidence()
        for event in events:
            if event['kind'] == 'acquire_cpu':
                event['kind'] = 'acquire_cpu_unavailable'
                event['value0'] = 0
        result = correlate(events, rows)['scheduling']
        self.assertEqual(result['missing_acquire_cpu_samples'], 2)
        self.assertEqual(result['metrics']['drawable_wait_cpu_ms']['samples'], 0)

    def test_clock_reset_and_overlapping_execution(self):
        events,rows=self.evidence()
        events.append(dict(kind='clock',id=0,start_ns=0,end_ns=100,value0=99_000_000,value1=0))
        correlate(events,rows)
        self.assertEqual(union_duration([(1,5),(2,4),(4,9),(12,13)]),9)
        with self.assertRaises(ValueError): union_duration([(3,2)])
        events[-1]['start_ns']=100
        events.insert(1,events.pop())
        with self.assertRaisesRegex(ValueError,'clock mapping'):
            correlate(events,rows)


class MetalPipelineTest(unittest.TestCase):
    def evidence(self):
        def e(kind, ident, start, end=0, a=0, b=42):
            return dict(kind=kind, id=ident, start_ns=start, end_ns=end, value0=a, value1=b)
        return [e('pipeline_scope',42,1,b=1), e('library_source',100,10,20),
                e('pipeline_render',200,30,40), e('pipeline_compute',300,50,60),
                e('bind_render',200,70,71), e('bind_compute',300,72,73),
                e('bind_render',200,101,102), e('bind_compute',300,103,104)]

    def test_warm_bindings_are_not_shader_creation_or_cache_misses(self):
        result = pipeline_events(self.evidence(), 100, 200)
        self.assertTrue(result['coverage_complete'])
        self.assertEqual(result['creation_calls_in_window'], 0)
        self.assertEqual(result['pipeline_generations_bound'], 2)
        self.assertEqual(result['first_binds_in_window'], [])
        self.assertEqual(result['binding_api_ms']['samples'], 2)
        self.assertFalse(result['measures_driver_cache_misses'])

    def test_recycled_pointer_gets_new_first_use(self):
        events = self.evidence()
        events.append(dict(events[2], start_ns=80, end_ns=90))
        result = pipeline_events(events, 100, 200)
        self.assertEqual(result['pipeline_generations_bound'], 3)
        self.assertEqual([e['id'] for e in result['first_binds_in_window']], [200])
        self.assertEqual(result['last_first_bind_ns'], 101)

    def test_long_creation_overlapping_window_is_included(self):
        events = self.evidence()
        events.append(dict(events[2], start_ns=90, end_ns=150, id=400))
        result = pipeline_events(events, 100, 200)
        self.assertEqual(result['creation_calls_in_window'], 1)
        self.assertEqual(result['creations_in_window'][0]['start_ns'], 90)

    def test_missing_or_failed_creation_and_unscoped_device_are_explicit(self):
        events = self.evidence()
        events[2]['id'] = 0
        result = pipeline_events(events, 100, 200)
        self.assertFalse(result['coverage_complete'])
        self.assertEqual(result['failed_creations'], 1)
        self.assertEqual(result['unmatched_bindings'], 2)
        events = self.evidence()
        events[2]['value1'] = 43
        result = pipeline_events(events, 100, 200)
        self.assertFalse(result['coverage_complete'])
        self.assertEqual(result['unscoped_device_calls'], 1)
        with self.assertRaisesRegex(ValueError, 'not enabled'):
            pipeline_events(events[1:], 100, 200)

    def test_rejects_reversed_pipeline_duration(self):
        events = self.evidence()
        events[-1]['end_ns'] = 102
        with self.assertRaisesRegex(ValueError, 'Reversed pipeline'):
            pipeline_events(events, 100, 200)


if __name__=='__main__': unittest.main()
