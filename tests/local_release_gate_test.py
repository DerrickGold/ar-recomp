#!/usr/bin/env python3
"""Exercise the real release dependency graph without building packages."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Each process appends one event atomically, including parallel package commands.
STUB = '''#!{python}
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
event = "check" if name == "gate" else (
    "package" if "-P" in args else "release-test" if name == "ctest" else args[0])
fd = os.open("events.jsonl", os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
os.write(fd, (json.dumps([event, args]) + "\\n").encode())
os.close(fd)
sys.exit(1 if os.environ.get("FAIL_GATE") == event else 0)
'''


class LocalReleaseGateTest(unittest.TestCase):
    def run_release(self, fail=""):
        with tempfile.TemporaryDirectory(prefix="actraiser-release-gate-") as directory:
            root = Path(directory)
            shutil.copyfile(ROOT / "Makefile", root / "Makefile")
            # Preserve release/check-release recipes and their dependency graph.
            # Stub the already-covered Debug/quality work and external programs.
            (root / "harness.mk").write_text(
                'include Makefile\n'
                'check: ; @gate check\n'
                'check-quality check-constants check-go-vet check-c check-go '
                'check-shaders: ; @:\n')
            bin_dir = root / "bin"
            bin_dir.mkdir()
            for name in ("gate", "cmake", "ctest"):
                path = bin_dir / name
                path.write_text(STUB.format(python=sys.executable))
                path.chmod(0o755)
            environment = dict(os.environ, FAIL_GATE=fail,
                               PATH=str(bin_dir) + os.pathsep + os.environ["PATH"])
            # Do not inherit the invoking Make jobserver or command-line overrides.
            for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
                environment.pop(name, None)
            result = subprocess.run(
                ["make", "-f", "harness.mk", "-j4", "release", "release-macos-arm64",
                 "release-windows-x86_64", "CHECK_JOBS=2"],
                cwd=root, env=environment, text=True, capture_output=True, timeout=30)
            events_path = root / "events.jsonl"
            events = [json.loads(line) for line in events_path.read_text().splitlines()]
            return result, events

    def test_multiple_destinations_share_sequential_debug_and_release_gates(self):
        result, events = self.run_release()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        names = [event[0] for event in events]
        self.assertEqual(names[:4], ["check", "--preset", "--build", "release-test"])
        self.assertEqual(names[4:], ["package"] * 3)
        self.assertEqual(events[1][1], ["--preset", "tests-release"])
        self.assertEqual(events[2][1], ["--build", "--preset", "tests-release", "--parallel", "2"])
        self.assertEqual(events[3][1], ["--preset", "tests-release", "-j", "2"])

    def test_debug_failure_stops_release_tests_and_all_packaging(self):
        result, events = self.run_release("check")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([event[0] for event in events], ["check"])

    def test_release_test_failure_stops_all_packaging(self):
        result, events = self.run_release("release-test")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([event[0] for event in events],
                         ["check", "--preset", "--build", "release-test"])


if __name__ == "__main__":
    unittest.main()
