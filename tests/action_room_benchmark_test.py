"""ROM-free validation of room-sweep inputs, evidence and failure handling."""
import argparse
import copy
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/action_editor'))
import benchmark_rooms as benchmark

BINARY = None


class RoomBenchmarkTest(unittest.TestCase):
    def test_default_routes_and_disabled_player_are_valid(self):
        plan = benchmark.load_plan(ROOT / 'tools/action_editor/benchmark_routes.json')
        self.assertGreater(len(plan), 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'plan.json'
            route = dict(name='test-route', room='0502', start=[0, -224],
                         end=[128, 0], player=[-1, 0])
            path.write_text(json.dumps([route]))
            self.assertEqual(benchmark.load_plan(path), [route])

    def test_invalid_routes_fail_before_starting_runs(self):
        route = dict(name='test-route', room='0502', start=[0, 224],
                     end=[128, 224], player=[128, 404])
        invalid = [[], {}, [None], [route, route]]
        for key, value in [('name', '../outside'), ('name', ''), ('room', '../0502'),
                           ('room', '050'), ('start', [0]), ('end', [True, 0]),
                           ('end', [0, 1.5]), ('player', [2**31, 0]),
                           ('player', [2**31 - 1, 0])]:
            invalid.append([dict(route, **{key: value})])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'plan.json'
            for plan in invalid:
                with self.subTest(plan=plan):
                    path.write_text(json.dumps(plan))
                    with self.assertRaises(ValueError):
                        benchmark.load_plan(path)

    def test_display_check_requires_valid_awake_backlight(self):
        state = {'/panel/actual_brightness': '200', '/panel/bl_power': '0'}
        self.assertTrue(benchmark.display_awake(state))
        for value in ['', 'asleep', '0', '-1']:
            self.assertFalse(benchmark.display_awake(dict(state, **{'/panel/actual_brightness': value})))
        self.assertFalse(benchmark.display_awake({}))
        self.assertFalse(benchmark.display_awake(dict(state, **{'/panel/bl_power': '4'})))

    def test_statistics_reject_incomplete_or_inconsistent_measurements(self):
        rows = [dict(frame=i, prepare_ms=1, compose_submit_ms=2, gpu_wait_ms=i+1,
                     complete_ms=i+4) for i in range(3)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sweep.csv'

            def save(values, fields=tuple(rows[0])):
                with path.open('w', newline='') as file:
                    writer = csv.DictWriter(file, fieldnames=fields, extrasaction='ignore')
                    writer.writeheader()
                    writer.writerows(values)

            save(rows)
            summary = benchmark.summarize(path, 3)
            self.assertEqual(summary['compose_complete_ms']['mean'], 4)
            self.assertEqual(summary['compose_complete_ms']['p99'], 5)
            invalid = [rows[:-1], rows[::-1]]
            for key, value in [('gpu_wait_ms', 'nan'), ('gpu_wait_ms', 'inf'),
                               ('prepare_ms', -1), ('complete_ms', 999),
                               ('frame', 'bad'), ('prepare_ms', 'not-a-time')]:
                changed = copy.deepcopy(rows)
                changed[1][key] = value
                invalid.append(changed)
            for values in invalid:
                with self.subTest(values=values):
                    save(values)
                    with self.assertRaisesRegex(ValueError, 'sweep.csv'):
                        benchmark.summarize(path, 3)
            save(rows, ('frame', 'prepare_ms'))
            with self.assertRaisesRegex(ValueError, 'sweep.csv'):
                benchmark.summarize(path, 3)

    def test_failed_and_timed_out_children_preserve_evidence(self):
        for code, timeout, exception in [
                ('import sys; print("expected failure", flush=True); sys.exit(7)', 5, RuntimeError),
                ('import time; print("started", flush=True); time.sleep(30)', .01, TimeoutError)]:
            with self.subTest(exception=exception), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                with self.assertRaises(exception):
                    benchmark.run_process([sys.executable, '-c', code], dict(os.environ), output, timeout)
                self.assertTrue((output / 'run.log').read_text().strip())
                self.assertIsInstance(json.loads((output / 'telemetry.json').read_text()), list)


class NativeArgumentsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY:
            raise unittest.SkipTest('Pass --binary to also test native argument handling')

    def test_malformed_arguments_are_rejected_without_a_display(self):
        cases = [(None, 11, 'wrong'), (None, 5, '12px'), (None, 7, str(2**80)),
                 ('AR_BENCH_WIDTH', None, '0'), ('AR_BENCH_HEIGHT', None, '2049'),
                 ('AR_BENCH_WARMUP', None, '-1'), ('AR_BENCH_EFFECTS', None, '2'),
                 ('AR_BENCH_FINAL_BATTLE', None, '2'),
                 ('AR_BENCH_STEP', None, 'abc'), ('AR_BENCH_CLOCK', None, '')]
        for variable, position, value in cases:
            with self.subTest(variable=variable, position=position, value=value):
                self.run_invalid(variable, position, value)

    def test_conflicting_outputs_do_not_truncate_an_existing_file(self):
        for variable in ['AR_BENCH_PROFILE', 'AR_BENCH_SHOT']:
            with self.subTest(variable=variable):
                self.run_invalid(variable, None, 'same-output')

    def run_invalid(self, variable, position, value):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'trace.csv'
            output.write_text('existing evidence')
            command = [str(BINARY), str(output), 'layers.ini', 'effects.ini', '0502.arscene',
                       '0', '224', '128', '224', '128', '404', '2']
            env = {k: v for k, v in os.environ.items() if not k.startswith(('AR_', 'SDL_', 'SNESRECOMP_'))}
            env['SDL_VIDEO_DRIVER'] = 'dummy'
            if variable:
                env[variable] = str(output) if value == 'same-output' else value
            else:
                command[position] = value
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn('[benchmark]', result.stderr)
            self.assertEqual(output.read_text(), 'existing evidence')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve() if args.binary else None
    unittest.main(argv=[sys.argv[0], *remaining])
