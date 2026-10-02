"""Timing analysis must preserve idle uploads and reject broken traces."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_frame_pacing import analyze


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

    def report(self, rows):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'pacing.csv'
            with path.open('w', newline='') as f:
                writer = csv.DictWriter(f, rows[0].keys())
                writer.writeheader()
                writer.writerows(rows)
            return analyze(path)

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


if __name__ == '__main__':
    unittest.main()
