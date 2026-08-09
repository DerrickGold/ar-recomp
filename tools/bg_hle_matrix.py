#!/usr/bin/env python3
"""Run the SPEC-bg-hle ordinary action-entry census matrix.

The runner starts from the deterministic replay's transition-capable world-map
window, stages each verified raw warp target, captures two stable game frames,
enables the read-only runtime comparator, and validates the resulting snapshots
with ``bg_hle_census.py``. It never enables a background provider.

    python3 tools/bg_hle_matrix.py
    python3 tools/bg_hle_matrix.py --targets 0201,0202 --fail-fast
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile

import bg_hle_census


DEFAULT_TARGETS = (
    "0101", "0102", "0201", "0202", "0301", "0303",
    "0401", "0404", "0501", "0504", "0601", "0605",
)

TARGET_NAMES = {
    "0101": "Fillmore act 1",
    "0102": "Fillmore act 2",
    "0201": "Bloodpool act 1",
    "0202": "Bloodpool act 2",
    "0301": "Kasandora act 1",
    "0303": "Kasandora act 2",
    "0401": "Aitos act 1",
    "0404": "Aitos act 2",
    "0501": "Marahna act 1",
    "0504": "Marahna act 2",
    "0601": "Northwall act 1",
    "0605": "Northwall act 2",
    "0608": "Northwall act 2 boss",
    "0701": "Death Heim hub",
}

RUN_DIRECTORY_RE = re.compile(r"\[run-dir\] (runs/[^ ]+) \(")
COMPARATOR_RE = re.compile(
    r"\[action-bg-hle\] summary frames=(?P<frames>\d+) "
    r"activations=(?P<activations>\d+) layers=(?P<layers>\d+) "
    r"tiles=(?P<tiles>\d+) mismatches=(?P<mismatches>\d+) "
    r"outside=(?P<outside>\d+) "
    r"fallbacks=\{blank:(?P<blank>\d+),mode:(?P<mode>\d+),"
    r"disabled:(?P<disabled>\d+),native:(?P<native>\d+),"
    r"invalid:(?P<invalid>\d+),alloc:(?P<alloc>\d+),"
    r"compare:(?P<compare>\d+)\}")


class MatrixError(Exception):
    """A target run did not satisfy the evidence contract."""


def parse_targets(value):
    targets = []
    for raw_target in value.split(","):
        target = raw_target.strip().upper()
        if not re.fullmatch(r"[0-9A-F]{4}", target):
            raise argparse.ArgumentTypeError(
                "target must be four hexadecimal digits: %s" % raw_target)
        if target not in TARGET_NAMES:
            raise argparse.ArgumentTypeError(
                "target is not in the verified table: %s" % target)
        if target not in targets:
            targets.append(target)
    if not targets:
        raise argparse.ArgumentTypeError("at least one target is required")
    return targets


def parse_comparator_summary(log):
    matches = list(COMPARATOR_RE.finditer(log))
    if not matches:
        return None
    return {key: int(value) for key, value in matches[-1].groupdict().items()}


def parse_run_directory(log):
    match = RUN_DIRECTORY_RE.search(log)
    return match.group(1) if match else None


def inspect_ppm(path):
    try:
        with open(path, "rb") as source:
            magic = source.readline().strip()
            dimensions = source.readline().strip().split()
            maximum = source.readline().strip()
    except OSError as error:
        raise MatrixError("framebuffer missing: %s" % error)
    if magic != b"P6" or len(dimensions) != 2 or maximum != b"255":
        raise MatrixError("malformed framebuffer PPM: %s" % path)
    try:
        width, height = (int(value) for value in dimensions)
    except ValueError:
        raise MatrixError("malformed framebuffer dimensions: %s" % path)
    if width <= 0 or height <= 0:
        raise MatrixError("empty framebuffer dimensions: %s" % path)
    return {
        "path": path,
        "width": width,
        "height": height,
        "sha256": bg_hle_census.file_sha256(path),
    }


def inspect_run(target, run_directory, log, rom_hash, expected_snapshots):
    comparator = parse_comparator_summary(log)
    if comparator is None:
        raise MatrixError("runtime comparator summary missing")
    for field in ("mismatches", "invalid", "alloc", "compare"):
        if comparator[field]:
            raise MatrixError("comparator %s=%d" % (field, comparator[field]))
    snapshot_directory = os.path.join(run_directory, "snapshots")
    try:
        prefixes = bg_hle_census.discover_prefixes([snapshot_directory])
        records = [
            bg_hle_census.analyze_snapshot(prefix, rom_hash)
            for prefix in prefixes
        ]
    except bg_hle_census.CensusError as error:
        raise MatrixError(str(error))
    if len(records) != expected_snapshots:
        raise MatrixError(
            "expected %d snapshots, found %d" %
            (expected_snapshots, len(records)))
    expected_group = int(target[:2], 16)
    expected_map = int(target[2:], 16)
    for record in records:
        if (record["map_group"], record["map_number"]) != (
                expected_group, expected_map):
            raise MatrixError(
                "snapshot reached %02X/%02X, expected %02X/%02X" % (
                    record["map_group"], record["map_number"],
                    expected_group, expected_map))
        if not record["ppu_metadata"]:
            raise MatrixError("snapshot PPU metadata missing")
        for layer in record["layers"]:
            if not layer["valid"]:
                raise MatrixError(
                    "BG%d invalid: %s" %
                    (layer["layer"], ",".join(layer["errors"])))
            if layer["ppu"]["eligible"] is not True:
                continue
            comparison = layer["comparison"]
            if not comparison["available"] or comparison["mismatches"]:
                raise MatrixError(
                    "BG%d eligible ring mismatch=%d" %
                    (layer["layer"], comparison["mismatches"]))
    framebuffer = inspect_ppm(os.path.join(run_directory, "shot.ppm"))
    return {
        "target": target,
        "name": TARGET_NAMES[target],
        "run_directory": run_directory,
        "comparator": comparator,
        "census": bg_hle_census.build_summary(records),
        "snapshots": records,
        "framebuffer": framebuffer,
        "status": "pass",
    }


def run_target(args, target, rom_hash):
    with tempfile.TemporaryDirectory(prefix="actraiser-bg-hle-") as temporary:
        settings_path = os.path.join(temporary, "settings.ini")
        with open(settings_path, "w", encoding="utf-8") as settings:
            settings.write(
                "# Generated by tools/bg_hle_matrix.py.\n"
                "diorama_mode = Off\n"
                "diorama_vertical_extend = 0\n")
        environment = os.environ.copy()
        environment.update({
            "AR_HEADLESS": "1",
            "AR_ACTION_BG_HLE_COMPARE": "1",
            "AR_INPUT_REPLAY": args.replay,
            "AR_SETTINGS_PATH": settings_path,
            "AR_WARP": target,
            "AR_WARP_AT": str(args.warp_frame),
            "AR_VRAMDUMP_GF": ",".join(
                str(frame) for frame in args.capture_frames),
            "AR_SHOT_AT_GF": str(args.capture_frames[0]),
            "AR_QUIT_FRAMES": str(args.quit_frames),
        })
        command = [args.binary, args.rom, "--config", args.config]
        completed = subprocess.run(
            command, cwd=args.cwd, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, check=False)
    log = completed.stdout
    run_directory = parse_run_directory(log)
    if completed.returncode != 0:
        raise MatrixError(
            "process exit %d%s" % (
                completed.returncode,
                " (%s)" % run_directory if run_directory else ""))
    if run_directory is None:
        raise MatrixError("run directory missing from process log")
    if not os.path.isabs(run_directory):
        run_directory = os.path.join(args.cwd, run_directory)
    return inspect_run(
        target, run_directory, log, rom_hash, len(args.capture_frames))


def write_manifest(path, args, rom_hash, results):
    manifest = {
        "format": 1,
        "generated_at": datetime.datetime.now().astimezone().isoformat(),
        "rom": args.rom,
        "rom_sha256": rom_hash,
        "binary": args.binary,
        "replay": args.replay,
        "warp_frame": args.warp_frame,
        "capture_frames": args.capture_frames,
        "quit_frames": args.quit_frames,
        "settings_fixture": "generated flat defaults; Diorama off",
        "results": results,
    }
    with open(path, "w", encoding="utf-8") as output:
        json.dump(manifest, output, indent=2, sort_keys=True)
        output.write("\n")


def parse_targets_as_frames(value):
    try:
        frames = [int(item.strip(), 0) for item in value.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError("capture frames must be integers")
    if not frames or any(frame < 0 or frame > 0xFFFF for frame in frames):
        raise argparse.ArgumentTypeError("capture frames must be in 0..65535")
    return frames


def parse_args(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cwd", default=root)
    parser.add_argument("--binary", default="build-release/ActRaiserRecomp")
    parser.add_argument("--rom", default="ar.sfc")
    parser.add_argument("--config", default="config.ini")
    parser.add_argument("--replay", default="saves/fillmore-act-2.rec")
    parser.add_argument("--targets", type=parse_targets,
                        default=list(DEFAULT_TARGETS))
    parser.add_argument("--warp-frame", type=int, default=400)
    parser.add_argument("--capture-frames", type=parse_targets_as_frames,
                        default=[900, 1200])
    parser.add_argument("--quit-frames", type=int, default=1900)
    parser.add_argument("--manifest",
                        help="output JSON (default: runs/bg-hle-matrix-<time>.json)")
    parser.add_argument("--fail-fast", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    args.cwd = os.path.abspath(args.cwd)
    for attribute in ("binary", "rom", "config", "replay"):
        value = getattr(args, attribute)
        if not os.path.isabs(value):
            value = os.path.join(args.cwd, value)
        setattr(args, attribute, value)
    for attribute in ("binary", "rom", "config", "replay"):
        path = getattr(args, attribute)
        if not os.path.isfile(path):
            print("bg_hle_matrix: %s does not exist: %s" % (attribute, path),
                  file=sys.stderr)
            return 2
    if not os.access(args.binary, os.X_OK):
        print("bg_hle_matrix: binary is not executable: %s" % args.binary,
              file=sys.stderr)
        return 2
    rom_hash = bg_hle_census.file_sha256(args.rom)

    results = []
    failed = False
    for index, target in enumerate(args.targets, 1):
        print("[%d/%d] %s %s" % (
            index, len(args.targets), target, TARGET_NAMES[target]), flush=True)
        try:
            result = run_target(args, target, rom_hash)
            results.append(result)
            comparator = result["comparator"]
            print("      PASS tiles=%d mismatches=0 native-fallbacks=%d run=%s" % (
                comparator["tiles"], comparator["native"],
                result["run_directory"]), flush=True)
        except (MatrixError, OSError) as error:
            failed = True
            results.append({
                "target": target,
                "name": TARGET_NAMES[target],
                "status": "fail",
                "error": str(error),
            })
            print("      FAIL %s" % error, file=sys.stderr, flush=True)
            if args.fail_fast:
                break

    manifest_path = args.manifest
    if not manifest_path:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        manifest_path = os.path.join(args.cwd, "runs", "bg-hle-matrix-%s.json" % stamp)
    elif not os.path.isabs(manifest_path):
        manifest_path = os.path.join(args.cwd, manifest_path)
    try:
        write_manifest(manifest_path, args, rom_hash, results)
    except OSError as error:
        print("bg_hle_matrix: could not write manifest: %s" % error,
              file=sys.stderr)
        return 2
    print("matrix manifest: %s" % manifest_path)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
