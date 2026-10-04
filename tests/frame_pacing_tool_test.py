"""Timing analysis must preserve idle uploads and reject broken traces."""
import csv
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_frame_pacing import analyze, source_cadence


class PacingTraceTest(unittest.TestCase):
    def rows(self):
        rows = []
        for tick in range(1200, 1320):
            clock = (tick - 1199) * 16_000_000
            base = dict(loop_ns=clock, prepare_ns=clock+100_000,
                        ready_ns=clock+2_100_000, complete_ns=clock+2_200_000,
                        draw_ns=0, deadline_ns=clock+4_000_000,
                        upload_ns=2_000_000, uploads=1, draw_work_ns=0,
                        swap_ns=0, vector_wait_ns=0, queue_before=1, queue_after=0,
                        presented=0, tick=tick, epoch=1, source_ns=clock-12_000_000,
                        producer_start_ns=clock-11_900_000, producer_complete_ns=clock-1_900_000,
                        input_ns=clock-12_000_000, pump_ns=90_000,
                        input_events_ns=9_000, owner_poll_ns=1_000)
            rows.append(base)
            rows.append(dict(base, loop_ns=clock+3_900_000, prepare_ns=clock+4_000_000,
                             ready_ns=clock+4_000_000, draw_ns=clock+4_000_000,
                             complete_ns=clock+5_500_000, presented=1, upload_ns=0,
                             uploads=0, draw_work_ns=1_000_000, swap_ns=500_000,
                             vector_wait_ns=250_000, queue_before=0))
        return rows

    def report(self, rows, **options):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'pacing.csv'
            with path.open('w', newline='') as f:
                writer = csv.DictWriter(f, rows[0].keys())
                writer.writeheader()
                writer.writerows(rows)
            return analyze(path, **options)

    def test_synchronous_trace_uses_source_schedule(self):
        period = 16639263
        rows = []
        for i in range(180):
            now = 1_000_000_000 + i * 1_000_000_000 // 90
            tick = (now - 1_000_000_000) // period + 1200
            rows.append(dict(sample_ns=now, complete_ns=now+2_000_000,
                interval_ns=period, remainder_ns=0, alpha=0.0, tick=tick,
                produced=1, interpolation=0, source_ns=1_000_000_000+(tick-1200)*period,
                epoch=1, tick_delta=1 if i == 0 or tick != rows[-1]['tick'] else 0,
                pacing_source=2, target_ns=now, map_group=0, map_number=4))
        report = self.report(rows)
        self.assertEqual(report['source_cadence']['best_phase_mismatches'], 0)
        self.assertGreater(report['zero_tick_presents'], 50)
        self.assertEqual(report['pacing_sources'], {'2': 180})
        self.assertNotIn('producer_work_ms', report)

    def test_native_cadence_allows_natural_holds_and_ntsc_drift(self):
        period = 16639263
        for refresh in (30, 60, 90, 120):
            rows = []
            for i in range(1, refresh * 30):
                complete = 1_000_000_000 + i * 1_000_000_000 // refresh
                tick = (complete - 14_000_000) // period
                rows.append(dict(tick=tick, source_ns=tick*period,
                                 complete_ns=complete, interval_ns=period,
                                 epoch=1, interpolation=0))
            clean = source_cadence(rows)
            self.assertEqual(clean['best_phase_mismatches'], 0)
            for i in range(100, len(rows), 100):
                rows[i]['tick'] -= 1
                rows[i]['source_ns'] -= period
            bad = source_cadence(rows)
            self.assertGreater(bad['best_phase_mismatches'], 0)
            for row in rows:
                row['interpolation'] = 1
            self.assertIsNone(source_cadence(rows)['best_phase_mismatch_percent'])

    def test_native_epoch_resets_and_legacy_opt_in(self):
        rows = [dict(tick=i, source_ns=i*16000000, complete_ns=i*16000000+delay,
                     interval_ns=16000000, epoch=epoch)
                for epoch, delay in [(1, 20000000), (2, 50000000)] for i in range(100)]
        self.assertEqual(source_cadence(rows)['samples'], 0)
        result = source_cadence(rows, assume_native=True)
        self.assertEqual(result['best_phase_mismatches'], 0)
        self.assertEqual(result['skipped_ticks'], 0)
        self.assertEqual(result['held_presents'], 0)

    def test_cadence_threshold_exit_status(self):
        rows = self.rows()
        for row in rows:
            row.update(interpolation=0, interval_ns=16000000, producer_cpu_ns=7000000)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'pacing.csv'
            def write():
                with path.open('w', newline='') as f:
                    writer = csv.DictWriter(f, rows[0].keys())
                    writer.writeheader(); writer.writerows(rows)
            command = [sys.executable, str(Path(__file__).resolve().parents[1] /
                       'tools/analyze_frame_pacing.py'), str(path), '--max-irregular-percent', '0']
            write()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            rows[121]['source_ns'] -= 16000000
            rows[121]['tick'] -= 1
            write()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
        report = self.report(self.rows())
        self.assertEqual(report['producer_cpu_ms']['samples'], 0)
        measured = self.report(rows)
        self.assertEqual(measured['producer_cpu_ms']['mean'], 7)
        self.assertEqual(measured['producer_unaccounted_wall_ms']['mean'], 3)

    def test_idle_uploads_and_overlapping_scopes(self):
        result = self.report(self.rows())
        self.assertEqual(result['presents'], 120)
        self.assertEqual(result['upload_without_present'], 120)
        self.assertEqual(result['producer_work_ms']['samples'], 120)
        self.assertEqual(result['producer_work_ms']['mean'], 10)
        self.assertEqual(result['upload_ms']['mean'], 2)
        self.assertEqual(result['complete_interval_ms']['mean'], 16)
        self.assertEqual(result['draw_work_ms']['mean'], 1)
        self.assertEqual(result['vector_wait_ms']['mean'], .25)
        self.assertEqual(result['deadline_over_1ms'], 0)

    def test_reject_invalid_stage_or_partial_run(self):
        rows = self.rows()
        for key, value in [('draw_ns', 0), ('vector_wait_ns', 2_000_000),
                           ('loop_ns', rows[0]['loop_ns'])]:
            with self.subTest(key=key):
                bad = [dict(row) for row in rows]
                bad[1][key] = value
                with self.assertRaises(ValueError):
                    self.report(bad)
        with self.assertRaises(ValueError):
            self.report(rows[:20])

    def test_epoch_transition_is_not_a_frame_interval(self):
        rows = self.rows()
        for row in rows[120:]:
            row['epoch'] = 2
        result = self.report(rows)
        self.assertEqual(result['complete_interval_ms']['samples'], 118)
        self.assertEqual(result['all_complete_interval_ms']['samples'], 119)
        self.assertEqual(result['epoch_boundaries'], 1)

    def test_warmup_uses_actual_stream_start(self):
        rows = self.rows()
        # A large absolute clock must not satisfy a relative warmup by itself.
        for row in rows:
            for key in ('loop_ns', 'prepare_ns', 'ready_ns', 'complete_ns',
                        'deadline_ns', 'source_ns', 'producer_start_ns',
                        'producer_complete_ns', 'input_ns'):
                row[key] += 20_000_000_000
            if row['draw_ns']:
                row['draw_ns'] += 20_000_000_000
        result = self.report(rows, warmup_seconds=.16)
        self.assertEqual(result['first_stream_tick'], 1200)
        self.assertEqual(result['measured_ticks'], [1210, 1319])
        self.assertEqual(result['presents'], 110)
        self.assertGreaterEqual(result['first_present_after_stream_ms'], 160)
        with self.assertRaises(ValueError):
            self.report(rows, warmup_seconds=10)

    def test_separate_scheduled_wait_from_backend_present(self):
        rows = self.rows()
        for row in rows:
            row.update(submit_start_ns=0, submit_deadline_ns=0, submit_wait_ns=0)
            if row['presented']:
                row.update(submit_start_ns=row['complete_ns']-300_000,
                           submit_deadline_ns=row['complete_ns']-400_000,
                           submit_wait_ns=200_000)
        result = self.report(rows)
        self.assertEqual(result['backend_present_ms']['mean'], .3)
        self.assertEqual(result['submit_wait_ms']['mean'], .2)
        self.assertEqual(result['submit_late_ms']['mean'], .1)
        worst = result['worst_intervals'][0]
        self.assertEqual(worst['backend_present_ms'], .3)
        self.assertEqual(worst['submit_late_ms'], .1)
        self.assertEqual(worst['source_ready_slack_ms'], 7)
        rows[1]['submit_start_ns'] = 0
        with self.assertRaises(ValueError):
            self.report(rows)

    def test_early_acquire_is_outside_final_present(self):
        rows = self.rows()
        for row in rows:
            row.update(submit_start_ns=0, submit_deadline_ns=0, submit_wait_ns=0,
                       backend_flush_ns=0, backend_acquire_ns=0, backend_submit_ns=0)
            if row['presented']:
                row.update(submit_start_ns=row['complete_ns']-100_000,
                           submit_deadline_ns=row['complete_ns']-100_000,
                           backend_flush_ns=200_000, backend_acquire_ns=300_000,
                           backend_submit_ns=100_000)
        result = self.report(rows)
        # Early CPU work must not be added a second time to completion or
        # misreported as work still occurring after the presentation deadline.
        self.assertEqual(result['backend_present_ms']['mean'], .1)
        self.assertEqual(result['backend_acquire_ms']['mean'], .3)
        self.assertEqual(result['worst_intervals'][0]['backend_flush_ms'], .2)
        self.assertEqual(result['complete_interval_ms']['mean'], 16)


if __name__ == '__main__':
    unittest.main()
