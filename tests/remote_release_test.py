#!/usr/bin/env python3
"""Exercise source transfer, remote packaging and verified artifact retrieval."""

import io
import hashlib
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release_remote

SSH = '''#!{python}
import json, os, shlex, subprocess, sys
host = sys.argv[-2]
if host == "localhost":
    sys.exit("localhost must never use SSH")
if os.environ.get("FAIL_HANDOFF"):
    remaining = json.loads(sys.stdin.buffer.readline())["source_bytes"]
    while remaining:
        remaining -= len(sys.stdin.buffer.read(min(remaining, 65536)))
    print(json.dumps(dict(event="ready")), flush=True)
    target = json.loads(sys.stdin.buffer.readline())["target"]
    print(json.dumps(dict(event="artifacts", target=target, size=10000)), flush=True)
    sys.stdout.buffer.write(b"truncated transfer")
    sys.exit(4)
if os.environ.get("MULTI_HOST"):
    shell = shlex.split(sys.argv[-1])[-1]
    command = shlex.split(shell.split("; exec ", 1)[1])
    index = command.index("--remote-dir") + 1
    command[index] += "-" + host
    os.environ["BUILD_HOST"] = host
    os.execvpe(command[0], command, os.environ)
os.execv("/bin/sh", ["/bin/sh", "-c", sys.argv[-1]])
'''

CMAKE = '''#!{python}
import hashlib, json, os, subprocess, sys
from pathlib import Path
args = sys.argv[1:]
if "-DBUILDER_PRINT_PLAN=ON" in args:
    os.execv({cmake!r}, [{cmake!r}, *args])
root = Path.cwd()
assert not (root / "private.sfc").exists()
assert not (root / "development").exists()
assert not (root / ".git").exists()
assert (root / "fresh.txt").read_text() == "uncommitted addition"
assert not (root / "deleted.txt").exists()
(root / "installer/packaging/cache/retained.txt").write_text("cached dependency")
platforms = next(a for a in args if a.startswith("-DBUILDER_PLATFORMS="))
host = os.environ.get("BUILD_HOST", "localhost")
if os.environ.get("BUILD_BARRIER"):
    import time
    barrier = Path(os.environ["BUILD_BARRIER"])
    (root / "installer/packaging/cache/build.pid").write_text(str(os.getpid()))
    if not (barrier / (host + ".ready")).exists():
        (barrier / (host + ".ready")).write_text(json.dumps(platforms.split("=", 1)[1].split()))
    deadline = time.monotonic() + 10
    while len(list(barrier.glob("*.ready"))) < int(os.environ["BUILD_COUNT"]):
        if time.monotonic() > deadline:
            sys.exit("build hosts did not run concurrently")
        time.sleep(0.01)
    if host == os.environ.get("SLOW_BUILD_HOST"):
        while len(list(barrier.glob("localhost-*.done"))) < int(os.environ["FAST_BUILD_COUNT"]):
            if time.monotonic() > deadline:
                sys.exit("idle host did not steal the remaining queued targets")
            time.sleep(0.01)
if host == os.environ.get("HANG_BUILD_HOST") or os.environ.get("INTERRUPT_REMOTE_BUILD") or (os.environ.get("HANG_OTHER_HOSTS") and
                                              host != os.environ.get("FAIL_BUILD_HOST")):
    import time
    (root / "installer/packaging/cache/build.pid").write_text(str(os.getpid()))
    while True:
        time.sleep(1)
if (os.environ.get("FAIL_REMOTE_BUILD") or host == os.environ.get("FAIL_BUILD_HOST") or
        platforms.split("=", 1)[1] == os.environ.get("FAIL_BUILD_TARGET")):
    print("fixture compiler failure on " + host, file=sys.stderr, flush=True)
    sys.exit(3)
legacy = next(a for a in args if a.startswith("-DBUILDER_LEGACY_ARCHIVES="))
plan = subprocess.check_output([{cmake!r}, "-DBUILDER_PRINT_PLAN=ON", platforms, legacy,
    "-P", "installer/packaging/release.cmake"], text=True)
release = root / "release"
release.mkdir(exist_ok=True)
for line in plan.splitlines():
    if "Release plan: " not in line:
        continue
    parts = line.split("Release plan: ", 1)[1].split(" | ")
    names = [parts[2]] + ([parts[4]] if len(parts) == 5 else [])
    for name in names:
        content = (root / "edited.txt").read_bytes()
        artifact = release / name
        artifact.write_bytes(content)
        artifact.chmod(0o755)
        digest = hashlib.sha256(content).hexdigest()
        if os.environ.get("CORRUPT_REMOTE_CHECKSUM"):
            digest = "0" * 64
        (release / (name + ".sha256")).write_text(digest + "  " + name + "\\n")
(root / "installer/packaging/cache/release-command.json").write_text(json.dumps(args))
with (root / "installer/packaging/cache/release-events.jsonl").open("a") as events:
    events.write(json.dumps(args) + "\\n")
if os.environ.get("BUILD_BARRIER"):
    for target in platforms.split("=", 1)[1].split():
        done = barrier / (host + "-" + target + ".done")
        assert not done.exists(), "target built twice on the same host"
        done.write_text("finished")
'''


class RemoteReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="remote-release-test-")
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name)
        self.root = base / "source tree"
        self.root.mkdir()
        self.remote = base / "remote cache with spaces"
        self.bin = base / "bin"
        self.bin.mkdir()
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake required for the real release-plan contract")
        for name in ("Makefile", "tools/release_remote.py", "installer/packaging/release.cmake",
                     "installer/packaging/release_policy.cmake"):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (self.root / ".gitignore").write_text("private.sfc\ndevelopment/\nrelease/\nruns/\n")
        (self.root / "edited.txt").write_text("committed source")
        (self.root / "deleted.txt").write_text("remove before transfer")
        for command in (["git", "init", "-q"], ["git", "add", "."],
                        ["git", "-c", "user.name=Release fixture", "-c",
                         "user.email=release@example.invalid", "commit", "-qm", "fixture"]):
            subprocess.run(command, cwd=self.root, check=True, capture_output=True)
        (self.root / "edited.txt").write_text("dirty source")
        (self.root / "deleted.txt").unlink()
        (self.root / "fresh.txt").write_text("uncommitted addition")
        (self.root / "private.sfc").write_text("private ROM")
        (self.root / "development").mkdir()
        (self.root / "development/internal.md").write_text("private progress")
        for name, text in (("ssh", SSH), ("cmake", CMAKE)):
            script = self.bin / name
            script.write_text(text.format(python=sys.executable, cmake=cmake))
            script.chmod(0o755)
        # The worker checks tool presence; compilation is replaced by CMAKE above.
        (self.bin / "go").write_text("#!/bin/sh\nexit 0\n")
        (self.bin / "go").chmod(0o755)
        self.environment = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
            self.environment.pop(name, None)

    def run_release(self, *options, host="build-box", hosts=None, **environment):
        return subprocess.run(
            ["make", "release-remote", *(hosts or [host]), "PLATFORMS=macos-arm64 linux-arm64",
             "REMOTE_DIR=" + str(self.remote), "PYTHON=" + sys.executable, *options],
            cwd=self.root, env=dict(self.environment, **environment),
            text=True, capture_output=True, timeout=60)

    def source_archive(self):
        snapshot = Path(self.temporary.name) / "source.tar"
        with tarfile.open(snapshot, "w") as archive:
            for name in release_remote.source_files(self.root):
                archive.add(self.root / name, arcname=name)
        return snapshot

    def test_dirty_source_transfer_artifacts_and_reusable_caches(self):
        result = self.run_release()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        release = self.root / "release"
        names = {p.name for p in release.iterdir()}
        self.assertEqual(len(names), 6)
        self.assertIn("ActRaiserRecompBuilder-macos-arm64-portable.zip", names)
        self.assertIn("actraiser-recomp-linux-arm64.tar.xz", names)
        for path in release.iterdir():
            if not path.name.endswith(".sha256"):
                self.assertEqual(path.read_text(), "dirty source")
                self.assertTrue(path.stat().st_mode & 0o111)
        self.assertEqual((self.remote / "downloads/retained.txt").read_text(), "cached dependency")
        self.assertFalse(list(self.remote.glob("work-*")))
        (release / "unrelated.txt").write_text("keep me")
        (self.root / "edited.txt").write_text("next source")
        result = self.run_release()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((release / "actraiser-recomp-linux-arm64.tar.xz").read_text(), "next source")
        self.assertEqual((release / "unrelated.txt").read_text(), "keep me")

    def test_legacy_keep_build_version_and_alias_matching_a_local_target(self):
        # An SSH alias called 'clean' must never execute the local clean recipe.
        result = self.run_release("DESKTOP=0", "KEEP_BUILD=1", host="clean")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(list(self.remote.glob("work-*")))
        command = json.loads((self.remote / "downloads/release-command.json").read_text())
        self.assertIn("-DBUILDER_LEGACY_ARCHIVES=ON", command)
        self.assertIn("-DBUILDER_KEEP_BUILD=ON", command)
        self.assertTrue(any(arg.startswith("-DSNESBUILD_VERSION=") and arg.endswith("-dirty")
                            for arg in command))
        self.assertTrue((self.root / "edited.txt").exists())
        self.assertEqual(len(list((self.root / "release").iterdir())), 4)

    def test_failed_build_and_bad_checksums_preserve_local_outputs(self):
        release = self.root / "release"
        release.mkdir()
        old = release / "actraiser-recomp-linux-arm64.tar.xz"
        old.write_text("previous release")
        for failure in ("FAIL_REMOTE_BUILD", "CORRUPT_REMOTE_CHECKSUM", "FAIL_HANDOFF"):
            result = self.run_release(**{failure: "1"})
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(old.read_text(), "previous release")
            self.assertEqual(list(release.iterdir()), [old])
            self.assertFalse(list(self.remote.glob("work-*")))

    def test_next_run_removes_abandoned_jobs_without_removing_unowned_data(self):
        self.remote.mkdir()
        (self.remote / ".release-remote-owned").write_text(release_remote.MARKER)
        abandoned = self.remote / "work-interrupted"
        abandoned.mkdir()
        (abandoned / ".release-remote-job").write_text(release_remote.MARKER)
        (abandoned / "unfinished.txt").write_text("old job")
        unowned = self.remote / "work-custom"
        unowned.mkdir()
        (unowned / "my-files.txt").write_text("keep me")
        result = self.run_release()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(abandoned.exists())
        self.assertEqual((unowned / "my-files.txt").read_text(), "keep me")

    def test_interrupted_build_stops_children_and_removes_the_job(self):
        # Signal the actual worker while its Make/CMake descendants are running.
        snapshot = self.source_archive()
        with snapshot.open("rb") as source:
            process = subprocess.Popen(
                [sys.executable, str(ROOT / "tools/release_remote.py"), "--worker",
                 "--remote-dir", str(self.remote), "--platforms", "linux-arm64",
                 "--version", "interrupted-test"], stdin=source,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=dict(self.environment, INTERRUPT_REMOTE_BUILD="1"))
            try:
                pid_file = self.remote / "downloads/build.pid"
                deadline = time.monotonic() + 10
                while not pid_file.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        break
                    time.sleep(0.05)
                self.assertTrue(pid_file.exists(), "worker did not start the build")
                process.terminate()
                _output, errors = process.communicate(timeout=15)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(b"interrupted", errors)
                pid = int(pid_file.read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate()
        self.assertFalse(list(self.remote.glob("work-*")))
        self.assertFalse((self.root / "release").exists())

    def test_broken_artifact_stream_removes_the_remote_job(self):
        with self.source_archive().open("rb") as source:
            process = subprocess.Popen(
                [sys.executable, str(ROOT / "tools/release_remote.py"), "--worker",
                 "--remote-dir", str(self.remote), "--platforms", "linux-arm64",
                 "--version", "handoff-test"], stdin=source,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
            process.stdout.close()
            try:
                process.wait(timeout=15)
                errors = process.stderr.read()
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(b"Broken pipe", errors)
                self.assertIn(b"Remote temporary workspace removed.", errors)
                self.assertFalse(list(self.remote.glob("work-*")))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                process.stderr.close()

    def test_live_progress_and_errors_are_saved_locally(self):
        result = self.run_release(FAIL_REMOTE_BUILD="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Remote workspace:", result.stderr)
        self.assertIn("make failed", result.stderr)
        logs = list((self.root / "runs/release-remote").glob("*/build.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn(str(logs[0]), result.stdout)
        text = logs[0].read_text()
        entries = text.splitlines()
        timestamps = []
        for entry in entries:
            match = re.match(r"^\[(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)\] ", entry)
            self.assertIsNotNone(match, entry)
            timestamps.append(datetime.fromisoformat(match[1].replace("Z", "+00:00")))
        self.assertEqual(timestamps, sorted(timestamps))
        host_entries = (logs[0].parent / "hosts/build-box.log").read_text().splitlines()
        for entry in host_entries:
            self.assertRegex(entry, r"^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z\] \[build-box\] ")
        self.assertTrue(all(entry in entries for entry in host_entries if "Release " not in entry))
        for message in ("Local source:", "build hosts: build-box", "platforms: macos-arm64",
                        "cmake", "Remote temporary workspace removed.", "make failed",
                        "Remote release failed: make failed"):
            self.assertIn(message, text)
        self.assertNotIn("ssh failed", result.stderr)
        self.assertFalse(list(self.remote.glob("work-*")))

    def test_remote_prerequisite_errors_retain_the_cause_and_install_hint(self):
        (self.bin / "ssh").write_text(
            "#!/bin/sh\n"
            "echo 'release-remote: Missing remote build prerequisites: cmake, go. "
            "On the remote Mac, run: brew install go cmake' >&2\nexit 1\n")
        result = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Remote release failed: Missing remote build prerequisites: cmake, go",
                      result.stderr)
        self.assertIn("On the remote Mac, run: brew install go cmake", result.stderr)
        self.assertNotIn("SSH connection", result.stderr)
        self.assertFalse((self.root / "release").exists())

    def test_worker_reports_all_missing_base_tools_before_creating_a_job(self):
        available = shutil.which
        with mock.patch.object(release_remote.shutil, "which", side_effect=lambda name:
                               None if name in ("cmake", "go") else available(name)), \
                mock.patch.object(release_remote.sys, "platform", "darwin"), \
                self.assertRaises(ValueError) as error:
            release_remote.remote_action(SimpleNamespace(remote_dir=str(self.remote)))
        self.assertIn("Missing remote build prerequisites: cmake, go", str(error.exception))
        self.assertIn("On the remote Mac, run: brew install go cmake", str(error.exception))
        self.assertFalse(list(self.remote.glob("work-*")))

    def test_connection_failures_are_identified_separately(self):
        (self.bin / "ssh").write_text(
            "#!/bin/sh\necho 'Permission denied (publickey).' >&2\nexit 255\n")
        result = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SSH connection or transport failed (exit 255)", result.stderr)
        self.assertIn("Permission denied", result.stderr)
        self.assertNotIn("Remote release failed", result.stderr)
        self.assertFalse((self.root / "release").exists())

    def test_failed_upload_is_cleaned_up(self):
        # Exercise the worker before packaging starts, with a truncated upload.
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/release_remote.py"), "--worker",
             "--remote-dir", str(self.remote), "--platforms", "linux-arm64"],
            input=b"incomplete transfer", env=self.environment, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list(self.remote.glob("work-*")))

    def test_queued_worker_cleans_up_after_a_partial_source_upload(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/release_remote.py"), "--worker", "--queue-worker",
             "--remote-dir", str(self.remote), "--platforms", "linux-arm64"],
            input=b'{"source_bytes":1000}\npartial source', env=self.environment,
            capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Incomplete source transfer", result.stderr)
        self.assertIn(b"Remote temporary workspace removed.", result.stderr)
        self.assertFalse(list(self.remote.glob("work-*")))

    def test_queued_worker_cleans_up_when_artifact_handback_breaks(self):
        # Exceed the pipe buffer so the worker is still returning its payload
        # when the coordinator disconnects, rather than waiting for more work.
        (self.root / "edited.txt").write_bytes(b"x" * (1024 * 1024))
        snapshot = self.source_archive().read_bytes()
        process = subprocess.Popen(
            [sys.executable, str(ROOT / "tools/release_remote.py"), "--worker", "--queue-worker",
             "--remote-dir", str(self.remote), "--platforms", "linux-arm64",
             "--version", "handoff-test"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            process.stdin.write((json.dumps({"source_bytes": len(snapshot)}) + "\n").encode())
            process.stdin.write(snapshot)
            process.stdin.flush()
            self.assertEqual(json.loads(process.stdout.readline()), {"event": "ready"})
            process.stdin.write(b'{"target":"linux-arm64"}\n')
            process.stdin.flush()
            record = json.loads(process.stdout.readline())
            self.assertEqual(record["event"], "artifacts")
            self.assertEqual(record["target"], "linux-arm64")
            process.stdout.close()
            process.wait(timeout=15)
            errors = process.stderr.read()
            self.assertNotEqual(process.returncode, 0)
            self.assertIn(b"Broken pipe", errors)
            self.assertIn(b"Remote temporary workspace removed.", errors)
            self.assertFalse(list(self.remote.glob("work-*")))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            process.stderr.close()
            if not process.stdout.closed:
                process.stdout.close()

    def test_unsafe_local_destination_prevents_partial_publication(self):
        release = self.root / "release"
        release.mkdir()
        old = release / "ActRaiserRecompBuilder-macos-arm64.app.zip"
        old.write_text("previous release")
        unsafe = release / "actraiser-recomp-linux-arm64.tar.xz"
        unsafe.symlink_to(self.root / "private.sfc")
        result = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsafe local release destination", result.stderr)
        self.assertEqual(old.read_text(), "previous release")
        self.assertEqual((self.root / "private.sfc").read_text(), "private ROM")
        self.assertEqual(set(release.iterdir()), {old, unsafe})
        self.assertFalse(list(self.remote.glob("work-*")))

    def test_unowned_remote_directory_is_never_overwritten(self):
        self.remote.mkdir()
        sentinel = self.remote / "my-files.txt"
        sentinel.write_text("keep me")
        result = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not owned by release-remote", result.stderr)
        self.assertEqual(list(self.remote.iterdir()), [sentinel])

    def test_dry_run_needs_no_ssh_and_does_not_create_outputs(self):
        (self.bin / "ssh").write_text("#!/bin/sh\nexit 87\n")
        result = self.run_release("REMOTE_DRY_RUN=1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.remote.exists())
        self.assertFalse((self.root / "release").exists())

    def test_invalid_make_goal_lists_do_not_run_any_target(self):
        for goals in (["release-remote"], ["release-remote", "build-box", "build-box"],
                      ["clean", "release-remote", "build-box"],
                      ["release-remote", "release-remote"]):
            result = subprocess.run(["make", *goals], cwd=self.root, env=self.environment,
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((self.root / "edited.txt").exists())
            self.assertFalse(self.remote.exists())

    def test_localhost_builds_without_ssh_and_cleans_up(self):
        (self.bin / "ssh").write_text("#!/bin/sh\nexit 87\n")
        result = self.run_release(host="localhost")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(list((self.root / "release").iterdir())), 6)
        self.assertFalse(list(self.remote.glob("work-*")))
        self.assertTrue((self.remote / "downloads/retained.txt").exists())
        self.assertTrue((self.root / "fresh.txt").exists())
        self.assertFalse((self.root / "installer/packaging/build").exists())

    def test_three_hosts_build_the_matrix_concurrently_without_duplicate_targets(self):
        barrier = Path(self.temporary.name) / "barrier"
        barrier.mkdir()
        platforms = "macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64 windows-arm64 steam-deck"
        hosts = ["first-box", "clean", "localhost"]
        result = self.run_release("PLATFORMS=" + platforms, hosts=hosts, MULTI_HOST="1",
                                  BUILD_BARRIER=str(barrier), BUILD_COUNT="3")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        built = []
        versions = []
        log = next((self.root / "runs/release-remote").glob("*/build.log"))
        for index, host in enumerate(hosts):
            self.assertEqual(json.loads((barrier / (host + ".ready")).read_text()), [platforms.split()[index]])
            cache = self.remote if host == "localhost" else Path(str(self.remote) + "-" + host)
            self.assertFalse(list(cache.glob("work-*")))
            commands = [json.loads(line) for line in (cache / "downloads/release-events.jsonl").read_text().splitlines()]
            host_log = log.parent / "hosts" / (host + ".log")
            self.assertTrue(host_log.exists())
            for entry in host_log.read_text().splitlines():
                self.assertRegex(entry, r"^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z\] ")
                if "Release " not in entry:
                    self.assertIn(entry, log.read_text().splitlines())
            for command in commands:
                targets = next(arg.split("=", 1)[1].split() for arg in command
                               if arg.startswith("-DBUILDER_PLATFORMS="))
                self.assertEqual(len(targets), 1)
                built.extend(targets)
                versions.append(next(arg for arg in command if arg.startswith("-DSNESBUILD_VERSION=")))
                self.assertIn("Building " + targets[0], host_log.read_text())
            self.assertIn("[" + host + "]", log.read_text())
        self.assertCountEqual(built, platforms.split())
        self.assertEqual(len(built), len(set(built)))
        self.assertEqual(len(set(versions)), 1)
        self.assertTrue(versions[0].endswith("-dirty"))
        names = {p.name for p in (self.root / "release").iterdir()}
        self.assertEqual(len(names), 24)
        self.assertIn("ActRaiserRecompBuilder-steam-deck-portable.tar.xz", names)
        self.assertTrue((self.root / "edited.txt").exists())

    def test_failed_host_stops_peers_preserves_outputs_and_keeps_host_diagnostics(self):
        barrier = Path(self.temporary.name) / "barrier"
        barrier.mkdir()
        release = self.root / "release"
        release.mkdir()
        old = release / "previous.txt"
        old.write_text("previous release")
        result = self.run_release("PLATFORMS=linux-x86_64 linux-arm64 windows-arm64",
                                  hosts=["first-box", "broken-box", "localhost"], MULTI_HOST="1",
                                  BUILD_BARRIER=str(barrier), BUILD_COUNT="3",
                                  FAIL_BUILD_HOST="broken-box", HANG_OTHER_HOSTS="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("[broken-box] Remote release failed: make failed", result.stderr)
        self.assertEqual(list(release.iterdir()), [old])
        self.assertEqual(old.read_text(), "previous release")
        for host in ("first-box", "broken-box", "localhost"):
            cache = self.remote if host == "localhost" else Path(str(self.remote) + "-" + host)
            self.assertFalse(list(cache.glob("work-*")))
            pid = int((cache / "downloads/build.pid").read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
        log = next((self.root / "runs/release-remote").glob("*/build.log"))
        self.assertIn("fixture compiler failure on broken-box",
                      (log.parent / "hosts/broken-box.log").read_text())
        self.assertIn("Stopping remaining build hosts", log.read_text())

    def test_extra_hosts_are_idle_and_invalid_selections_do_not_start_jobs(self):
        result = self.run_release("REMOTE_DRY_RUN=1", hosts=["first-box", "localhost", "idle-box"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("[first-box] Initial target: macos-arm64", result.stdout)
        self.assertIn("[localhost] Initial target: linux-arm64", result.stdout)
        self.assertIn("[idle-box] Initial target: (idle; no targets assigned)", result.stdout)
        self.assertFalse(self.remote.exists())
        for options, hosts in ((["PLATFORMS=linux-arm64 linux-arm64"], ["first-box", "localhost"]),
                               ([], ["localhost", "localhost"]), ([], ["bad@alias"])):
            result = self.run_release(*options, hosts=hosts)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(self.remote.exists())
            self.assertFalse((self.root / "release").exists())
        result = self.run_release(hosts=["first-box", "localhost", "idle-box"], MULTI_HOST="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(Path(str(self.remote) + "-idle-box").exists())
        self.assertFalse(list((self.root / "runs/release-remote").glob("*/hosts/idle-box.log")))

    def test_idle_host_steals_all_queued_targets_while_slow_host_is_still_building(self):
        barrier = Path(self.temporary.name) / "barrier"
        barrier.mkdir()
        platforms = "linux-x86_64 linux-arm64 windows-x86_64 windows-arm64 steam-deck"
        result = self.run_release("PLATFORMS=" + platforms, "DESKTOP=0",
                                  hosts=["slow-box", "localhost"], MULTI_HOST="1",
                                  BUILD_BARRIER=str(barrier), BUILD_COUNT="2",
                                  SLOW_BUILD_HOST="slow-box", FAST_BUILD_COUNT="4")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(list(barrier.glob("slow-box-*.done"))), 1)
        self.assertEqual(len(list(barrier.glob("localhost-*.done"))), 4)
        self.assertCountEqual([path.name.removesuffix(".done").removeprefix("slow-box-")
                               .removeprefix("localhost-") for path in barrier.glob("*.done")],
                              platforms.split())
        self.assertEqual(len(list((self.root / "release").iterdir())), 10)
        for host in ("slow-box", "localhost"):
            cache = self.remote if host == "localhost" else Path(str(self.remote) + "-" + host)
            self.assertFalse(list(cache.glob("work-*")))
            log = next((self.root / "runs/release-remote").glob("*/hosts/" + host + ".log"))
            self.assertEqual(log.read_text().count("workspace:"), 1, "source must be uploaded once per host")

    def test_failure_after_a_stolen_target_preserves_outputs_and_stops_peers(self):
        barrier = Path(self.temporary.name) / "barrier"
        barrier.mkdir()
        release = self.root / "release"
        release.mkdir()
        old = release / "previous.txt"
        old.write_text("previous release")
        result = self.run_release("PLATFORMS=linux-x86_64 linux-arm64 windows-arm64",
                                  hosts=["first-box", "localhost"], MULTI_HOST="1",
                                  BUILD_BARRIER=str(barrier), BUILD_COUNT="2",
                                  HANG_BUILD_HOST="first-box", FAIL_BUILD_TARGET="windows-arm64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("[localhost] Local release failed: make failed", result.stderr)
        self.assertIn("Received linux-arm64", result.stdout)
        self.assertIn("[localhost] Building windows-arm64", result.stdout)
        self.assertEqual(list(release.iterdir()), [old])
        self.assertEqual(old.read_text(), "previous release")
        for cache in (Path(str(self.remote) + "-first-box"), self.remote):
            self.assertFalse(list(cache.glob("work-*")))
            with self.assertRaises(ProcessLookupError):
                os.kill(int((cache / "downloads/build.pid").read_text()), 0)

    def test_coordinator_interruption_stops_all_workers_and_cleans_up(self):
        barrier = Path(self.temporary.name) / "barrier"
        barrier.mkdir()
        caches = [Path(str(self.remote) + "-first-box"), self.remote]
        process = subprocess.Popen(
            [sys.executable, str(self.root / "tools/release_remote.py"), "--host", "first-box",
             "--host", "localhost", "--remote-dir", str(self.remote),
             "--platforms", "linux-x86_64 linux-arm64"], cwd=self.root,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(self.environment, MULTI_HOST="1", BUILD_BARRIER=str(barrier),
                     BUILD_COUNT="2", INTERRUPT_REMOTE_BUILD="1"))
        try:
            deadline = time.monotonic() + 10
            while len(list(barrier.glob("*.ready"))) != 2 and time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                time.sleep(0.05)
            self.assertEqual(len(list(barrier.glob("*.ready"))), 2, "both workers must start")
            process.terminate()
            _output, errors = process.communicate(timeout=25)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn(b"interrupted", errors)
            for cache in caches:
                self.assertFalse(list(cache.glob("work-*")))
                pid = int((cache / "downloads/build.pid").read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            self.assertFalse((self.root / "release").exists())
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=25)

    def test_returned_targets_and_cross_host_artifact_duplicates_are_rejected(self):
        for mode in ("wrong-targets", "duplicate-artifacts", "corrupt-transfer"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                stage = Path(directory)
                jobs = []
                for index, host in enumerate(("first-box", "second-box")):
                    targets = ["linux-x86_64" if index == 0 else "linux-arm64"]
                    name = "fixture.zip" if index == 0 or mode == "duplicate-artifacts" else "other.zip"
                    content = b"verified release"
                    digest = hashlib.sha256(content).hexdigest()
                    record = {"version": "fixture", "platforms": targets,
                              "artifacts": [name, name + ".sha256"]}
                    if index == 1 and mode == "wrong-targets":
                        record["platforms"] = ["unassigned-target"]
                    if index == 1 and mode == "corrupt-transfer":
                        digest = "0" * 64
                    transfer = stage / (host + ".tar")
                    with tarfile.open(transfer, "w") as archive:
                        for filename, data in ((release_remote.MANIFEST, json.dumps(record).encode()),
                                               (name, content), (name + ".sha256",
                                                (digest + "  " + name + "\n").encode())):
                            entry = tarfile.TarInfo(filename)
                            entry.size = len(data)
                            archive.addfile(entry, io.BytesIO(data))
                    jobs.append(SimpleNamespace(host=host, platforms=targets, transfer=transfer))
                with self.assertRaises(ValueError) as error:
                    release_remote.combine_artifacts(jobs, stage, "fixture")
                self.assertIn("[second-box] Artifact transfer validation failed", str(error.exception))

    def test_download_updates_are_throttled_without_losing_logs_or_errors(self):
        terminal = io.BytesIO()
        combined = io.BytesIO()
        job = SimpleNamespace(host="build-box", log=io.BytesIO(), diagnostics=bytearray(),
                              pending=b"", last_progress=0)
        output = b"\r10%\r20%\rcompiler error\nunterminated diagnostic"
        with mock.patch.object(release_remote.sys, "stderr", SimpleNamespace(buffer=terminal)), \
                mock.patch.object(release_remote.time, "monotonic", return_value=1):
            release_remote.emit_diagnostics(job, output, combined)
            release_remote.emit_diagnostics(job, b"", combined, final=True)
        self.assertEqual(job.log.getvalue(), combined.getvalue())
        normalized = re.sub(rb"^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z\] \[build-box\] ",
                            b"", job.log.getvalue(), flags=re.MULTILINE)
        self.assertEqual(normalized.splitlines(), output.splitlines())
        self.assertIn(b"[build-box] 10%", terminal.getvalue())
        self.assertNotIn(b"20%", terminal.getvalue())
        self.assertIn(b"[build-box] compiler error", terminal.getvalue())
        self.assertIn(b"[build-box] unterminated diagnostic", terminal.getvalue())
        self.assertIn(b"[build-box] 20%", combined.getvalue())

    def test_source_symlink_is_refused_before_transfer(self):
        (self.root / "linked.txt").symlink_to(self.root / "private.sfc")
        result = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Source symlinks are not supported", result.stderr)
        self.assertFalse(self.remote.exists())

    def test_transfer_rejects_paths_links_and_duplicate_entries(self):
        for kind in ("traversal", "symlink", "duplicate"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                stream = io.BytesIO()
                with tarfile.open(fileobj=stream, mode="w") as archive:
                    member = tarfile.TarInfo("../outside" if kind == "traversal" else "file")
                    if kind == "symlink":
                        member.type = tarfile.SYMTYPE
                        member.linkname = "../outside"
                    archive.addfile(member)
                    if kind == "duplicate":
                        archive.addfile(member)
                stream.seek(0)
                with tarfile.open(fileobj=stream) as archive, self.assertRaises(ValueError):
                    release_remote.extract_files(archive, Path(directory))

    def test_packaging_reports_platform_progress_and_preserves_the_version(self):
        # Run the production CMake orchestrator, replacing compilation only.
        real_cmake = shutil.which("cmake", path=os.environ["PATH"])
        tools = Path(self.temporary.name) / "packaging tools"
        tools.mkdir()
        events = tools / "events.jsonl"
        stub = (f"#!{sys.executable}\nimport json, sys\n"
                f"with open({str(events)!r}, 'a') as log:\n"
                "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n")
        for name in ("cmake", "cpack"):
            (tools / name).write_text(stub)
            (tools / name).chmod(0o755)
        script = tools / "run.cmake"
        script.write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            f"set(CMAKE_COMMAND [[{tools / 'cmake'}]])\n"
            "set(BUILDER_PLATFORMS [[linux-arm64 windows-arm64]])\n"
            "set(BUILDER_LEGACY_ARCHIVES ON)\n"
            "set(SNESBUILD_VERSION v0494-remote-test)\n"
            f"include([[{self.root / 'installer/packaging/release.cmake'}]])\n")
        result = subprocess.run([real_cmake, "-P", str(script)], text=True,
                                capture_output=True, cwd=self.root, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for message in ("[1/2] Packaging linux-arm64", "[1/2] Finished linux-arm64",
                        "[2/2] Packaging windows-arm64", "[2/2] Finished windows-arm64"):
            self.assertIn(message, result.stdout)
        configure = [json.loads(line) for line in events.read_text().splitlines()
                     if "-DSNESBUILD_VERSION=" in line]
        self.assertEqual(len(configure), 2)
        for command in configure:
            self.assertIn("-DSNESBUILD_VERSION=v0494-remote-test", command)


if __name__ == "__main__":
    unittest.main()
