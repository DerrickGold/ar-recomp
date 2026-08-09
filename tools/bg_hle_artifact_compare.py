#!/usr/bin/env python3
"""Compare deterministic action-background matrix artifacts by target.

The matrix manifest intentionally stores run directories rather than duplicating
every file hash. This acceptance tool pins the complete comparison contract:
five final artifacts plus six components for every requested PPU snapshot.
Console/perf logs are diagnostic and deliberately excluded.
"""

import argparse
import hashlib
import json
import os
import sys


FINAL_ARTIFACTS = (
    "shot.ppm",
    "dump_wram.bin",
    "dump_sram.bin",
    "dump_dispatch_log.json",
    "dump_state.txt",
)
SNAPSHOT_SUFFIXES = (
    ".wram.bin",
    ".vram.bin",
    ".cgram.bin",
    ".oam.bin",
    ".highoam.bin",
    ".ppu.json",
)


class ArtifactCompareError(Exception):
    """A manifest or run directory does not satisfy the comparison contract."""


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path):
    try:
        with open(path, "r", encoding="utf-8") as source:
            manifest = json.load(source)
    except (OSError, ValueError) as error:
        raise ArtifactCompareError("cannot read %s: %s" % (path, error))
    results = manifest.get("results")
    capture_frames = manifest.get("capture_frames")
    if not isinstance(results, list) or not results:
        raise ArtifactCompareError("manifest has no results: %s" % path)
    if not isinstance(capture_frames, list) or not capture_frames:
        raise ArtifactCompareError("manifest has no capture frames: %s" % path)
    by_target = {}
    for result in results:
        target = result.get("target")
        run_directory = result.get("run_directory")
        if result.get("status") != "pass" or not target or not run_directory:
            raise ArtifactCompareError(
                "target %s is not a completed pass in %s" % (target, path))
        if target in by_target:
            raise ArtifactCompareError("duplicate target %s in %s" % (target, path))
        by_target[target] = run_directory
    return by_target, len(capture_frames)


def discover_artifacts(run_directory, snapshot_count):
    artifacts = list(FINAL_ARTIFACTS)
    snapshot_directory = os.path.join(run_directory, "snapshots")
    try:
        names = sorted(os.listdir(snapshot_directory))
    except OSError as error:
        raise ArtifactCompareError(
            "cannot inspect %s: %s" % (snapshot_directory, error))
    components = [
        os.path.join("snapshots", name)
        for name in names
        if name.endswith(SNAPSHOT_SUFFIXES)
    ]
    expected_components = snapshot_count * len(SNAPSHOT_SUFFIXES)
    if len(components) != expected_components:
        raise ArtifactCompareError(
            "%s has %d snapshot components, expected %d" %
            (run_directory, len(components), expected_components))
    artifacts.extend(components)
    for relative in artifacts:
        if not os.path.isfile(os.path.join(run_directory, relative)):
            raise ArtifactCompareError(
                "artifact missing: %s" % os.path.join(run_directory, relative))
    return artifacts


def compare_manifests(left_path, right_path):
    left, left_snapshots = load_manifest(left_path)
    right, right_snapshots = load_manifest(right_path)
    if left_snapshots != right_snapshots:
        raise ArtifactCompareError(
            "capture count differs: %d vs %d" %
            (left_snapshots, right_snapshots))
    if set(left) != set(right):
        missing_left = sorted(set(right) - set(left))
        missing_right = sorted(set(left) - set(right))
        raise ArtifactCompareError(
            "target sets differ: left-missing=%s right-missing=%s" %
            (missing_left, missing_right))

    mismatches = []
    compared = 0
    for target in sorted(left):
        left_artifacts = discover_artifacts(left[target], left_snapshots)
        right_artifacts = discover_artifacts(right[target], right_snapshots)
        if left_artifacts != right_artifacts:
            raise ArtifactCompareError(
                "artifact sets differ for target %s" % target)
        for relative in left_artifacts:
            compared += 1
            left_hash = file_sha256(os.path.join(left[target], relative))
            right_hash = file_sha256(os.path.join(right[target], relative))
            if left_hash != right_hash:
                mismatches.append({
                    "target": target,
                    "artifact": relative,
                    "left_sha256": left_hash,
                    "right_sha256": right_hash,
                })
    return {
        "targets": len(left),
        "artifacts": compared,
        "mismatches": mismatches,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left_manifest")
    parser.add_argument("right_manifest")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = compare_manifests(args.left_manifest, args.right_manifest)
    except ArtifactCompareError as error:
        print("bg_hle_artifact_compare: %s" % error, file=sys.stderr)
        return 2
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    elif result["mismatches"]:
        for mismatch in result["mismatches"]:
            print("%s %s: %s != %s" % (
                mismatch["target"], mismatch["artifact"],
                mismatch["left_sha256"], mismatch["right_sha256"]))
        print("%d/%d artifacts differ" %
              (len(result["mismatches"]), result["artifacts"]))
    else:
        print("%d targets, %d artifacts exact" %
              (result["targets"], result["artifacts"]))
    return 1 if result["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main())
