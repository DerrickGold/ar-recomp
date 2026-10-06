#!/usr/bin/env python3
"""Split release targets across SSH hosts and localhost, then verify their artifacts."""

import argparse
from collections import deque
from contextlib import ExitStack
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
from types import SimpleNamespace

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
    if not (packaging / "cache").is_symlink():
        (packaging / "cache").symlink_to(cache_directory(base / "downloads"),
                                         target_is_directory=True)
    build = packaging / "build"
    build.mkdir(parents=True, exist_ok=True)
    if not (build / "host-toolchain").is_symlink():
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
    (work / MANIFEST).write_text(json.dumps({
        "version": args.version, "platforms": platforms, "artifacts": names}))
    return names


def send_record(record):
    sys.stdout.buffer.write((json.dumps(record) + "\n").encode())
    sys.stdout.buffer.flush()


def read_record(source):
    line = source.readline(8193)
    if not line or len(line) > 8192 or not line.endswith(b"\n"):
        raise ValueError("Missing or incomplete build worker command")
    record = json.loads(line)
    if not isinstance(record, dict):
        raise ValueError("Unexpected build worker command")
    return record


def copy_bytes(source, output, size):
    while size:
        block = source.read(min(size, 64 * 1024))
        if not block:
            raise ValueError("Incomplete source transfer")
        output.write(block)
        size -= len(block)


def queued_release(args, work, base):
    allowed, _names = release_plan(args, work)
    built = set()
    while True:
        send_record({"event": "ready"})
        command = read_record(sys.stdin.buffer)
        if command == {"stop": True}:
            return
        target = command.get("target")
        if not isinstance(target, str) or target not in allowed or target in built:
            raise ValueError(f"Unexpected or duplicate assigned target: {target}")
        selected = argparse.Namespace(**vars(args))
        selected.platforms = target
        names = build_release(selected, work, base)
        transfer = work / ".release-target.tar"
        with tarfile.open(transfer, "w") as archive:
            archive.add(work / MANIFEST, arcname=MANIFEST)
            for name in names:
                archive.add(work / "release" / name, arcname=name)
        print(f"Returning verified artifacts for {target}…", file=sys.stderr, flush=True)
        send_record({"event": "artifacts", "target": target, "size": transfer.stat().st_size})
        with transfer.open("rb") as source:
            shutil.copyfileobj(source, sys.stdout.buffer)
        sys.stdout.buffer.flush()
        transfer.unlink()
        (work / MANIFEST).unlink()
        for name in names:
            (work / "release" / name).unlink()
        built.add(target)


def remote_action(args):
    # Each worker owns upload, packaging, handback and unconditional cleanup.
    # Local workers use the same isolation and cache locking without SSH.
    import fcntl
    local = getattr(args, "host", []) == ["localhost"]
    location = "Local" if local else "Remote"
    base = remote_root(args.remote_dir)
    with (base / "build.lock").open("a") as lock:
        print(f"Waiting for the {location.lower()} release cache…", file=sys.stderr, flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        cleanup_abandoned(base)
        missing = [tool for tool in ("python3", "make", "cmake", "go")
                   if not shutil.which(tool)]
        if missing:
            message = f"Missing {location.lower()} build prerequisites: " + ", ".join(missing)
            if sys.platform == "darwin":
                machine = "this Mac" if local else "the remote Mac"
                message += (f". On {machine}, run: brew install go cmake pkgconf xz zstd "
                            "squashfs glib shared-mime-info sevenzip")
            else:
                message += ". Install these tools on the remote host and ensure they are on PATH"
            raise ValueError(message)
        work = Path(tempfile.mkdtemp(prefix="work-", dir=base))
        try:
            (work / ".release-remote-job").write_text(MARKER)
            print(f"{location} workspace: {work}", file=sys.stderr, flush=True)
            if getattr(args, "queue_worker", False):
                size = read_record(sys.stdin.buffer).get("source_bytes")
                if type(size) is not int or size <= 0:
                    raise ValueError("Unexpected source transfer size")
                snapshot = work / ".release-source.tar.gz"
                with snapshot.open("wb") as output:
                    copy_bytes(sys.stdin.buffer, output, size)
                with tarfile.open(snapshot) as archive:
                    extract_files(archive, work)
                snapshot.unlink()
                queued_release(args, work, base)
                return
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
            print(f"{location} temporary workspace removed.", file=sys.stderr, flush=True)


def worker_arguments(args):
    command = ["--worker", "--queue-worker", "--remote-dir", args.remote_dir,
               "--platforms", args.platforms, "--desktop", args.desktop,
               "--version", args.version]
    if args.keep_build:
        command.append("--keep-build")
    return command


def ssh_command(args):
    # SSH resolves the supplied alias using the caller's own SSH configuration.
    # Quote every remote argument for the remote POSIX shell, including paths.
    program = Path(__file__).read_text()
    command = ["python3", "-c", program, *worker_arguments(args)]
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
        if error.cmd[0] == "ssh":
            return f"SSH connection or transport failed (exit {error.returncode})"
        return f"{Path(error.cmd[0]).name} failed (exit {error.returncode})"
    if isinstance(error, KeyboardInterrupt):
        return "interrupted"
    return str(error)


def log_entry(message):
    timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    payload = message.encode() if isinstance(message, str) else message
    return f"[{timestamp}] ".encode() + payload + b"\n"


def progress(log, message, host_log=None):
    entry = log_entry(message)
    print(entry.decode(), end="", flush=True)
    log.write(entry)
    log.flush()
    if host_log is not None:
        host_log.write(entry)
        host_log.flush()


def assign_targets(hosts, platforms):
    if not hosts or any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", host)
                        for host in hosts):
        raise ValueError("Supply SSH config names or localhost: make release-remote <host> [<host> ...]")
    if len(set(hosts)) != len(hosts):
        raise ValueError("Duplicate build host; supply each SSH config name or localhost once")
    targets = [target for target in re.split(r"[;\s]+", platforms.strip()) if target]
    if not targets or any(not re.fullmatch(r"[a-z0-9_-]+", target) for target in targets):
        raise ValueError("Select at least one valid release target")
    if len(set(targets)) != len(targets):
        raise ValueError("Duplicate release target; each target must be built exactly once")
    return ([(host, targets[index:index + 1]) for index, host in enumerate(hosts)],
            targets[len(hosts):])


def emit_diagnostics(job, block, log, final=False):
    job.diagnostics.extend(block)
    del job.diagnostics[:-64 * 1024]
    job.pending += block
    lines = job.pending.splitlines(keepends=True)
    job.pending = b""
    if lines and not final and not lines[-1].endswith((b"\r", b"\n")):
        job.pending = lines.pop()
    # Bound unterminated diagnostic lines while preserving the full host log.
    if len(job.pending) > 64 * 1024:
        lines.append(job.pending)
        job.pending = b""
    for line in lines:
        labeled = log_entry(f"[{job.host}] ".encode() + line.rstrip(b"\r\n"))
        log.write(labeled)
        job.log.write(labeled)
        if line.endswith(b"\r"):
            now = time.monotonic()
            if not line.rstrip(b"\r\n") or now - job.last_progress < 0.5:
                continue
            job.last_progress = now
        sys.stderr.buffer.write(labeled)
    sys.stderr.buffer.flush()
    log.flush()
    job.log.flush()


def queue_command(job, command, selector):
    if job.sending or job.uploading:
        raise ValueError(f"[{job.host}] Build command overlapped a pending transfer")
    job.sending = bytearray((json.dumps(command) + "\n").encode())
    selector.register(job.process.stdin, selectors.EVENT_WRITE, (job, "stdin"))


def write_worker_input(job, selector):
    if not job.sending and job.uploading:
        job.sending.extend(job.source.read(64 * 1024))
        if not job.sending:
            job.uploading = False
    if job.sending:
        try:
            count = os.write(job.process.stdin.fileno(), job.sending)
        except BlockingIOError:
            return
        del job.sending[:count]
    if job.uploading and not job.sending and job.source.tell() == job.source_bytes:
        job.uploading = False
    if not job.sending and not job.uploading:
        selector.unregister(job.process.stdin)
        if job.stopping:
            job.process.stdin.close()


def worker_record(job, record, selector, state, log):
    if not isinstance(record, dict) or job.stopping:
        raise ValueError(f"[{job.host}] Unexpected build worker response")
    if record.get("event") == "ready":
        if job.active or job.receiving:
            raise ValueError(f"[{job.host}] Worker became idle before returning its artifacts")
        target = job.initial
        job.initial = None
        if not target and state.queue:
            target = state.queue.popleft()
        if target:
            job.active = target
            queue_command(job, {"target": target}, selector)
            message = f"[{job.host}] Building {target}; {len(state.queue)} targets queued"
            progress(log, message, job.log)
        else:
            job.stopping = True
            progress(log, f"[{job.host}] No queued targets remain; cleaning up", job.log)
            queue_command(job, {"stop": True}, selector)
    elif record.get("event") == "artifacts":
        size = record.get("size")
        if not job.active or record.get("target") != job.active or type(size) is not int or size <= 0:
            raise ValueError(f"[{job.host}] Unexpected assigned target or artifact transfer size")
        result = SimpleNamespace(host=job.host, platforms=[job.active],
                                 transfer=state.stage / (job.host + "-" + job.active + ".tar"))
        job.result = result
        job.output = state.stack.enter_context(result.transfer.open("wb"))
        job.receiving = size
    else:
        raise ValueError(f"[{job.host}] Unexpected build worker event")


def read_worker_output(job, block, selector, state, log):
    job.responses.extend(block)
    while job.responses:
        if job.receiving:
            count = min(job.receiving, len(job.responses))
            job.output.write(job.responses[:count])
            del job.responses[:count]
            job.receiving -= count
            if not job.receiving:
                job.output.close()
                state.results.append(job.result)
                progress(log, f"[{job.host}] Received {job.active} "
                              f"({len(state.results)}/{state.total} targets complete)", job.log)
                job.active = None
        else:
            newline = job.responses.find(b"\n")
            if newline < 0:
                if len(job.responses) > 8192:
                    raise ValueError(f"[{job.host}] Unexpected build worker response length")
                return
            if newline > 8192:
                raise ValueError(f"[{job.host}] Unexpected build worker response length")
            record = json.loads(job.responses[:newline])
            del job.responses[:newline + 1]
            worker_record(job, record, selector, state, log)


def pump_workers(selector, state, log, stopping=False):
    for key, _events in selector.select(0.2):
        job, channel = key.data
        if channel == "stdin":
            try:
                write_worker_input(job, selector)
            except BrokenPipeError:
                selector.unregister(key.fileobj)
                key.fileobj.close()
            continue
        block = os.read(key.fd, 64 * 1024)
        if channel == "stderr":
            emit_diagnostics(job, block, log, final=not block)
        elif block and not stopping:
            try:
                read_worker_output(job, block, selector, state, log)
            except ValueError as error:
                raise ValueError(f"[{job.host}] Artifact stream failed: {error}") from error
        if not block:
            selector.unregister(key.fileobj)
            key.fileobj.close()
            if channel == "stderr":
                job.eof = True


def worker_failure(job, status):
    # SSH forwards remote exit codes; only 255 means connection/transport failure.
    if job.host != "localhost" and status == 255:
        return f"[{job.host}] SSH connection or transport failed (exit 255)"
    kind = "Local" if job.host == "localhost" else "Remote"
    for line in reversed(job.diagnostics.decode(errors="replace").splitlines()):
        if line.startswith("release-remote: "):
            return f"[{job.host}] {kind} release failed: " + line.removeprefix("release-remote: ")
    return (f"[{job.host}] {kind} release or artifact transfer failed (exit {status}); "
            "see the host log for diagnostics")


def run_workers(args, assignments, queued, snapshot, stage, attempt, log):
    """Upload once per host, then let each idle worker claim one queued target."""
    jobs = []
    with ExitStack() as stack:
        selector = stack.enter_context(selectors.DefaultSelector())
        state = SimpleNamespace(queue=deque(queued), results=[], stage=stage, stack=stack,
                                total=len(queued) + sum(len(targets) for _host, targets in assignments))
        complete = False
        try:
            for host, targets in assignments:
                if not targets:
                    continue
                selected = argparse.Namespace(**vars(args))
                selected.host = host
                job = SimpleNamespace(host=host, platforms=targets, eof=False,
                                      diagnostics=bytearray(), pending=b"", last_progress=0,
                                      initial=targets[0], active=None, stopping=False,
                                      uploading=True, receiving=0, responses=bytearray(),
                                      source_bytes=snapshot.stat().st_size,
                                      sending=bytearray((json.dumps({"source_bytes": snapshot.stat().st_size}) + "\n").encode()))
                host_log = attempt / "hosts" / (host + ".log")
                job.log = stack.enter_context(host_log.open("wb"))
                progress(log, f"[{host}] Host log: {host_log}")
                job.log.write(log_entry(f"[{host}] Release {args.version}; platforms: " +
                                        selected.platforms))
                job.log.flush()
                command = ([sys.executable, str(Path(__file__).resolve()), "--host", "localhost",
                            *worker_arguments(selected)] if host == "localhost"
                           else ssh_command(selected))
                environment = dict(os.environ)
                environment["PATH"] = environment.get("PATH", "") + ":/opt/homebrew/bin:/usr/local/bin"
                job.source = stack.enter_context(snapshot.open("rb"))
                job.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                               stderr=subprocess.PIPE, env=environment,
                                               start_new_session=True, bufsize=0)
                jobs.append(job)
                os.set_blocking(job.process.stdin.fileno(), False)
                selector.register(job.process.stdin, selectors.EVENT_WRITE, (job, "stdin"))
                selector.register(job.process.stdout, selectors.EVENT_READ, (job, "stdout"))
                selector.register(job.process.stderr, selectors.EVENT_READ, (job, "stderr"))
            while selector.get_map() or any(job.process.poll() is None for job in jobs):
                pump_workers(selector, state, log)
                for job in jobs:
                    status = job.process.poll()
                    if job.eof and status:
                        raise ValueError(worker_failure(job, status))
            for job in jobs:
                if job.process.wait():
                    raise ValueError(worker_failure(job, job.process.returncode))
                if not job.stopping or job.active or job.receiving or job.responses:
                    raise ValueError(f"[{job.host}] Incomplete build or artifact stream")
            if state.queue or len(state.results) != state.total:
                raise ValueError("Not all queued release targets completed")
            complete = True
        finally:
            if not complete:
                progress(log, "Stopping remaining build hosts and cleaning up…")
                for job in jobs:
                    if job.process.poll() is None:
                        job.process.terminate()
                    if not job.process.stdin.closed:
                        try:
                            selector.unregister(job.process.stdin)
                        except KeyError:
                            pass
                        job.process.stdin.close()
                deadline = time.monotonic() + 15
                while (selector.get_map() or any(job.process.poll() is None for job in jobs)) \
                        and time.monotonic() < deadline:
                    pump_workers(selector, state, log, stopping=True)
                for job in jobs:
                    if job.process.poll() is None:
                        job.process.kill()
                    job.process.wait()
            for job in jobs:
                if not job.process.stderr.closed:
                    emit_diagnostics(job, b"", log, final=True)
                    job.process.stderr.close()
                if not job.process.stdout.closed:
                    job.process.stdout.close()
                if not job.process.stdin.closed:
                    job.process.stdin.close()
    return state.results


def combine_artifacts(jobs, stage, version):
    combined = stage / "combined"
    combined.mkdir()
    names = []
    for job in jobs:
        try:
            received = stage / (job.transfer.stem + "-received")
            received.mkdir()
            with tarfile.open(job.transfer) as archive:
                extract_files(archive, received, flat=True)
            record = json.loads((received / MANIFEST).read_text())
            if not isinstance(record, dict):
                raise ValueError("Unexpected release manifest")
            artifacts = record["artifacts"]
            if (record["version"] != version or record["platforms"] != job.platforms or
                    not isinstance(artifacts, list) or not artifacts or
                    any(not isinstance(name, str) for name in artifacts) or
                    len(set(artifacts)) != len(artifacts)):
                raise ValueError("Unexpected release manifest or assigned targets")
            validate_names(artifacts)
            if {p.name for p in received.iterdir()} != set(artifacts) | {MANIFEST}:
                raise ValueError("Release transfer contains unexpected files")
            if set(names).intersection(artifacts):
                raise ValueError("Build hosts returned duplicate release artifacts")
            verify_artifacts(received, artifacts)
            for name in artifacts:
                shutil.move(received / name, combined / name)
            names.extend(artifacts)
        except (OSError, ValueError, KeyError, tarfile.TarError) as error:
            raise ValueError(f"[{job.host}] Artifact transfer validation failed: {error}") from error
    (combined / MANIFEST).write_text(json.dumps({"version": version, "artifacts": names}))
    return combined


def local_action(args):
    assignments, queued = assign_targets(args.host, args.platforms)
    root = args.root.resolve()
    files = source_files(root)
    args.version = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"], cwd=root, text=True).strip()
    if not args.version or any(c in args.version for c in ";\r\n"):
        raise ValueError("Cannot use this Git version as a release stamp")
    size = sum((root / name).stat().st_size for name in files)
    print(f"Release {args.version}: {len(files)} source files, {size / (1024 * 1024):.1f} MiB",
          flush=True)
    print(f"Build hosts: {', '.join(args.host)}; platforms: {args.platforms}", flush=True)
    for host, targets in assignments:
        print(f"[{host}] Initial target: {' '.join(targets) if targets else '(idle; no targets assigned)'}",
              flush=True)
    print(f"Shared queue: {' '.join(queued) if queued else '(empty)'}; idle hosts take the next target",
          flush=True)
    if args.dry_run:
        print(f"Remote cache: {args.remote_dir}; output: {root / 'release'}")
        print("Dry run: no SSH connection, transfer or build.")
        return
    logs = root / "runs/release-remote"
    logs.mkdir(parents=True, exist_ok=True)
    attempt = Path(tempfile.mkdtemp(prefix=datetime.now().strftime("%Y%m%d-%H%M%S-"), dir=logs))
    (attempt / "hosts").mkdir()
    log_path = attempt / "build.log"
    print(f"Build log: {log_path}", flush=True)
    with log_path.open("wb") as log:
        progress(log, f"Release {args.version}; build hosts: {', '.join(args.host)}; platforms: {args.platforms}")
        for host, targets in assignments:
            progress(log, f"[{host}] Initial target: {' '.join(targets) if targets else '(idle)'}")
        progress(log, f"Shared queue: {' '.join(queued) if queued else '(empty)'}")
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
                progress(log, "Distributing source, building concurrently and retrieving artifacts…")
                jobs = run_workers(args, assignments, queued, snapshot, stage, attempt, log)
                progress(log, "Verifying returned artifacts locally…")
                received = combine_artifacts(jobs, stage, args.version)
                publish(received, root / "release", args.version)
                progress(log, "Remote release completed successfully.")
        except (OSError, ValueError, KeyError, tarfile.TarError,
                subprocess.CalledProcessError, KeyboardInterrupt) as error:
            log.write(log_entry("release-remote: " + failure_message(error)))
            log.flush()
            raise
        finally:
            print(f"Build log: {log_path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", action="append", default=[],
                        help="SSH config name or localhost; repeat to split targets across hosts")
    parser.add_argument("--root", type=Path, default=(Path(__file__).resolve().parents[1]
                        if "__file__" in globals() else Path.cwd()))
    parser.add_argument("--remote-dir", default=DEFAULT_DIRECTORY)
    parser.add_argument("--platforms", required=True)
    parser.add_argument("--desktop", choices=("0", "1"), default="1")
    parser.add_argument("--keep-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--queue-worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--version", default="", help=argparse.SUPPRESS)
    args = parser.parse_args()
    def interrupted(_number, _frame):
        raise KeyboardInterrupt
    for number in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, interrupted)
    if args.worker:
        remote_action(args)
    else:
        local_action(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, tarfile.TarError,
            subprocess.CalledProcessError, KeyboardInterrupt) as error:
        sys.exit("release-remote: " + failure_message(error))
