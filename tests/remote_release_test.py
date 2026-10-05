#!/usr/bin/env python3
"""Exercise source transfer, remote packaging and verified artifact retrieval."""

import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release_remote

SSH = '''#!{python}
import os, subprocess, sys
if os.environ.get("FAIL_HANDOFF"):
    result = subprocess.run(sys.argv[-1], shell=True, stdout=subprocess.PIPE)
    sys.stdout.buffer.write(result.stdout[:512])
    sys.exit(4)
sys.exit(subprocess.call(sys.argv[-1], shell=True))
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
if os.environ.get("INTERRUPT_REMOTE_BUILD"):
    import time
    (root / "installer/packaging/cache/build.pid").write_text(str(os.getpid()))
    while True:
        time.sleep(1)
if os.environ.get("FAIL_REMOTE_BUILD"):
    sys.exit(3)
platforms = next(a for a in args if a.startswith("-DBUILDER_PLATFORMS="))
legacy = next(a for a in args if a.startswith("-DBUILDER_LEGACY_ARCHIVES="))
plan = subprocess.check_output([{cmake!r}, "-DBUILDER_PRINT_PLAN=ON", platforms, legacy,
    "-P", "installer/packaging/release.cmake"], text=True)
release = root / "release"
release.mkdir()
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

    def run_release(self, *options, host="build-box", **environment):
        return subprocess.run(
            ["make", "release-remote", host, "PLATFORMS=macos-arm64 linux-arm64",
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
        for message in ("Local source:", "SSH host: build-box", "platforms: macos-arm64",
                        "cmake", "Remote temporary workspace removed.", "make failed",
                        "ssh failed"):
            self.assertIn(message, text)
        self.assertFalse(list(self.remote.glob("work-*")))

    def test_failed_upload_is_cleaned_up(self):
        # Exercise the worker before packaging starts, with a truncated upload.
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/release_remote.py"), "--worker",
             "--remote-dir", str(self.remote), "--platforms", "linux-arm64"],
            input=b"incomplete transfer", env=self.environment, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list(self.remote.glob("work-*")))

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
        for goals in (["release-remote"], ["release-remote", "build-box", "clean"],
                      ["clean", "release-remote", "build-box"],
                      ["release-remote", "release-remote"]):
            result = subprocess.run(["make", *goals], cwd=self.root, env=self.environment,
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((self.root / "edited.txt").exists())
            self.assertFalse(self.remote.exists())

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
