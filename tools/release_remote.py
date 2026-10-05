#!/usr/bin/env python3
"""Package the current source tree on an SSH host and retrieve verified releases."""

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile

MARKER = "ActRaiserRecomp release-remote v1\n"
MANIFEST = ".release-remote-artifacts.json"
DEFAULT_DIRECTORY = "~/.cache/actraiser-recomp/release-remote"


def source_files(root):
    listing = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=root)
    files = []
    for name in sorted(set(os.fsdecode(p) for p in listing.split(b"\0") if p)):
        path = root / name
        if path.is_symlink():
            raise ValueError(f"Source symlinks are not supported: {name}")
        if path.is_file():
            files.append(name)
        elif path.exists():
            raise ValueError(f"Expected a source file, not a directory/submodule: {name}")
    return files


def extract_files(archive, destination, flat=False):
    """Accept only regular files with relative, non-traversing paths."""
    for member in archive:
        name = PurePosixPath(member.name)
        if (not member.isfile() or name.is_absolute() or ".." in name.parts or
                not name.parts or ".git" in name.parts or
                (flat and len(name.parts) != 1)):
            raise ValueError(f"Unexpected transfer entry: {member.name}")
        target = destination / member.name
        if target.exists() or target.is_symlink():
            raise ValueError(f"Duplicate transfer entry: {member.name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.extractfile(member) as source, target.open("wb") as output:
            shutil.copyfileobj(source, output)
        target.chmod(member.mode & 0o777)


def remote_root(directory):
    base = Path(directory).expanduser().absolute()
    marker = base / ".release-remote-owned"
    if base.is_symlink():
        raise ValueError(f"Refusing a symlinked remote directory: {base}")
    if base.exists():
        if not base.is_dir() or marker.is_symlink() or not marker.is_file() or \
                marker.read_text() != MARKER:
            raise ValueError(f"Remote directory is not owned by release-remote: {base}")
    else:
        base.mkdir(parents=True)
        marker.write_text(MARKER)
    return base


def cleanup_abandoned(base):
    # Called with the shared build lock held, before this invocation has a job.
    for work in base.glob("work-*"):
        marker = work / ".release-remote-job"
        if (work.is_dir() and not work.is_symlink() and marker.is_file() and
                not marker.is_symlink() and marker.read_text() == MARKER):
            shutil.rmtree(work)


def cache_directory(path):
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise ValueError(f"Unexpected remote cache directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def release_plan(args, work):
    output = subprocess.check_output([
        "cmake", "-DBUILDER_PRINT_PLAN=ON", "-DBUILDER_PLATFORMS=" + args.platforms,
        "-DBUILDER_LEGACY_ARCHIVES=" + ("ON" if args.desktop == "0" else "OFF"),
        "-P", "installer/packaging/release.cmake"], cwd=work, text=True)
    platforms, names = [], []
    for line in output.splitlines():
        if "Release plan: " not in line:
            continue
        parts = line.split("Release plan: ", 1)[1].split(" | ")
        if len(parts) not in (3, 5) or not re.fullmatch(r"[a-z0-9_-]+", parts[0]):
            raise ValueError("Unexpected CMake release plan")
        platforms.append(parts[0])
        names.extend([parts[2], parts[2] + ".sha256"])
        if len(parts) == 5:
            names.extend([parts[4], parts[4] + ".sha256"])
    if not names or len(set(names)) != len(names):
        raise ValueError("CMake produced no unique release artifacts")
    validate_names(names)
    return platforms, names


def validate_names(names):
    for name in names:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]+", name):
            raise ValueError(f"Unexpected release filename: {name}")


def verify_artifacts(directory, names):
    validate_names(names)
    for name in names:
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing or unsafe release artifact: {name}")
    for name in names:
        path = directory / name
        if name.endswith(".sha256"):
            continue
        if name + ".sha256" not in names:
            raise ValueError(f"Missing release checksum: {name}")
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        checksum = (directory / (name + ".sha256")).read_text().strip()
        if checksum != digest.hexdigest() + "  " + name:
            raise ValueError(f"Release checksum mismatch: {name}")


def build_release(args, work, base):
    platforms, names = release_plan(args, work)
    packaging = work / "installer/packaging"
    (packaging / "cache").symlink_to(cache_directory(base / "downloads"),
                                     target_is_directory=True)
    build = packaging / "build"
    build.mkdir(parents=True)
    (build / "host-toolchain").symlink_to(cache_directory(base / "host-toolchain"),
                                          target_is_directory=True)
    go_cache = cache_directory(base / "go-cache")
    for platform in platforms:
        platform_build = build / platform
        platform_build.mkdir()
        (platform_build / "go-cache").symlink_to(
            cache_directory(go_cache / platform), target_is_directory=True)
    command = ["make", "release", "PLATFORMS=" + args.platforms,
               "DESKTOP=" + args.desktop, "RELEASE_VERSION=" + args.version]
    if args.keep_build:
        command.append("KEEP_BUILD=1")
    # Logs go to stderr; stdout is reserved for the returned artifact archive.
    # A separate process group lets interruption stop all build descendants.
    environment = dict(os.environ)
    for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
        environment.pop(name, None)
    process = subprocess.Popen(command, cwd=work, stdin=subprocess.DEVNULL,
                               stdout=sys.stderr, start_new_session=True, env=environment)
    try:
        status = process.wait()
        if status:
            raise subprocess.CalledProcessError(status, command)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        # Make may exit before its children. Stop any remaining descendants
        # before deleting their workspace, even if the parent already exited.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        raise
    verify_artifacts(work / "release", names)
    (work / MANIFEST).write_text(json.dumps({"version": args.version, "artifacts": names}))
    return names


def remote_action(args):
    # Only the remote worker imports the Unix locking API. The single SSH
    # session owns upload, packaging, artifact handback and unconditional cleanup.
    import fcntl
    base = remote_root(args.remote_dir)
    with (base / "build.lock").open("a") as lock:
        print("Waiting for the remote release cache…", file=sys.stderr, flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        cleanup_abandoned(base)
        for tool in ("python3", "make", "cmake", "go"):
            if not shutil.which(tool):
                raise ValueError(f"Remote packaging prerequisite is missing: {tool}")
        work = Path(tempfile.mkdtemp(prefix="work-", dir=base))
        try:
            (work / ".release-remote-job").write_text(MARKER)
            print(f"Remote workspace: {work}", file=sys.stderr, flush=True)
            with tarfile.open(fileobj=sys.stdin.buffer, mode="r|*") as archive:
                extract_files(archive, work)
            names = build_release(args, work, base)
            print("Returning verified release artifacts…", file=sys.stderr, flush=True)
            with tarfile.open(fileobj=sys.stdout.buffer, mode="w|") as archive:
                archive.add(work / MANIFEST, arcname=MANIFEST)
                for name in names:
                    archive.add(work / "release" / name, arcname=name)
            sys.stdout.buffer.flush()
        finally:
            shutil.rmtree(work)
            print("Remote temporary workspace removed.", file=sys.stderr, flush=True)


def ssh_command(args):
    # SSH resolves the supplied alias using the caller's own SSH configuration.
    # Quote every remote argument for the remote POSIX shell, including paths.
    program = Path(__file__).read_text()
    command = ["python3", "-c", program, "--worker",
               "--remote-dir", args.remote_dir,
               "--platforms", args.platforms, "--desktop", args.desktop,
               "--version", args.version]
    if args.keep_build:
        command.append("--keep-build")
    shell = "export PATH=$PATH:/opt/homebrew/bin:/usr/local/bin; exec " + shlex.join(command)
    remote = shlex.join(["/bin/sh", "-c", shell])
    return ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", args.host, remote]


def publish(directory, output, version):
    record = json.loads((directory / MANIFEST).read_text())
    names = record["artifacts"]
    if record["version"] != version or not names or len(set(names)) != len(names):
        raise ValueError("Unexpected release manifest")
    if {p.name for p in directory.iterdir()} != set(names) | {MANIFEST}:
        raise ValueError("Release transfer contains unexpected files")
    verify_artifacts(directory, names)
    if output.is_symlink() or (output.exists() and not output.is_dir()):
        raise ValueError(f"Unexpected local release directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    for name in names:
        target = output / name
        if target.is_symlink() or (target.exists() and not target.is_file()):
            raise ValueError(f"Unsafe local release destination: {name}")
    # Stage on the destination filesystem before replacing deliverables; /tmp
    # and the checkout need not share a volume.
    with tempfile.TemporaryDirectory(prefix=".remote-stage-", dir=output) as temporary:
        staged = Path(temporary)
        for name in names:
            shutil.copy2(directory / name, staged / name)
        for name in names:
            os.replace(staged / name, output / name)
    print(f"Retrieved {len(names) // 2} verified release artifacts into {output}", flush=True)


def failure_message(error):
    if isinstance(error, subprocess.CalledProcessError):
        return f"{Path(error.cmd[0]).name} failed (exit {error.returncode})"
    if isinstance(error, KeyboardInterrupt):
        return "interrupted"
    return str(error)


def progress(log, message):
    print(message, flush=True)
    log.write((message + "\n").encode())
    log.flush()


def transfer_and_build(args, snapshot, transfer, log):
    """Stream SSH diagnostics to the terminal and a durable local build log."""
    with snapshot.open("rb") as source, transfer.open("wb") as output:
        process = subprocess.Popen(ssh_command(args), stdin=source, stdout=output,
                                   stderr=subprocess.PIPE)
        try:
            for block in iter(lambda: process.stderr.read1(64 * 1024), b""):
                sys.stderr.buffer.write(block)
                sys.stderr.buffer.flush()
                log.write(block)
                log.flush()
            status = process.wait()
            if status:
                raise subprocess.CalledProcessError(status, ["ssh"])
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
        finally:
            process.stderr.close()


def local_action(args):
    if not args.host or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.host):
        raise ValueError("Supply one SSH config name: make release-remote <ssh-config-name>")
    root = args.root.resolve()
    files = source_files(root)
    args.version = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"], cwd=root, text=True).strip()
    if not args.version or any(c in args.version for c in ";\r\n"):
        raise ValueError("Cannot use this Git version as a release stamp")
    size = sum((root / name).stat().st_size for name in files)
    print(f"Release {args.version}: {len(files)} source files, {size / (1024 * 1024):.1f} MiB",
          flush=True)
    print(f"SSH host: {args.host}; platforms: {args.platforms}", flush=True)
    if args.dry_run:
        print(f"Remote cache: {args.remote_dir}; output: {root / 'release'}")
        print("Dry run: no SSH connection, transfer or build.")
        return
    logs = root / "runs/release-remote"
    logs.mkdir(parents=True, exist_ok=True)
    attempt = Path(tempfile.mkdtemp(prefix=datetime.now().strftime("%Y%m%d-%H%M%S-"), dir=logs))
    log_path = attempt / "build.log"
    print(f"Build log: {log_path}", flush=True)
    with log_path.open("wb") as log:
        progress(log, f"Release {args.version}; SSH host: {args.host}; platforms: {args.platforms}")
        progress(log, f"Local source: {root}; remote cache: {args.remote_dir}")
        try:
            with tempfile.TemporaryDirectory(prefix="actraiser-release-remote-") as directory:
                stage = Path(directory)
                snapshot = stage / "source.tar.gz"
                progress(log, "Preparing current working-tree source…")
                with tarfile.open(snapshot, "w:gz", compresslevel=1,
                                  dereference=True) as archive:
                    for name in files:
                        archive.add(root / name, arcname=name, recursive=False)
                progress(log, "Sending source, building remotely and retrieving artifacts…")
                transfer = stage / "release.tar"
                transfer_and_build(args, snapshot, transfer, log)
                received = stage / "received"
                received.mkdir()
                progress(log, "Verifying returned artifacts locally…")
                with tarfile.open(transfer) as archive:
                    extract_files(archive, received, flat=True)
                publish(received, root / "release", args.version)
                progress(log, "Remote release completed successfully.")
        except (OSError, ValueError, KeyError, tarfile.TarError,
                subprocess.CalledProcessError, KeyboardInterrupt) as error:
            log.write(("release-remote: " + failure_message(error) + "\n").encode())
            raise
        finally:
            print(f"Build log: {log_path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host")
    parser.add_argument("--root", type=Path, default=(Path(__file__).resolve().parents[1]
                        if "__file__" in globals() else Path.cwd()))
    parser.add_argument("--remote-dir", default=DEFAULT_DIRECTORY)
    parser.add_argument("--platforms", required=True)
    parser.add_argument("--desktop", choices=("0", "1"), default="1")
    parser.add_argument("--keep-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--version", default="", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        def interrupted(_number, _frame):
            raise KeyboardInterrupt
        for number in (signal.SIGTERM, signal.SIGHUP):
            signal.signal(number, interrupted)
        remote_action(args)
    else:
        local_action(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, tarfile.TarError,
            subprocess.CalledProcessError, KeyboardInterrupt) as error:
        sys.exit("release-remote: " + failure_message(error))
