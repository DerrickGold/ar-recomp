#!/usr/bin/env python3
"""Compare deterministic action-background matrix artifacts by target.

The matrix manifest intentionally stores run directories rather than duplicating
every file hash. This acceptance tool pins the complete comparison contract:
five final artifacts plus six components for every requested PPU snapshot.
Console/perf logs are diagnostic and deliberately excluded.

The default policy requires every byte to match. ``--framebuffer-policy
authentic-center`` retains that exact contract for emulated state and PPU
snapshots, requires the centered 256-pixel authentic viewport to match, and
reports (but accepts) intentional HLE differences confined to side margins.
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
AUTHENTIC_WIDTH = 256


class ArtifactCompareError(Exception):
    """A manifest or run directory does not satisfy the comparison contract."""


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_ppm(path):
    try:
        with open(path, "rb") as source:
            data = source.read()
    except OSError as error:
        raise ArtifactCompareError("cannot read %s: %s" % (path, error))
    marker = b"\n255\n"
    header_end = data.find(marker)
    if header_end < 0:
        raise ArtifactCompareError("unsupported PPM header: %s" % path)
    header = data[:header_end].splitlines()
    if len(header) != 2 or header[0] != b"P6":
        raise ArtifactCompareError("unsupported PPM header: %s" % path)
    try:
        width, height = (int(value) for value in header[1].split())
    except (ValueError, TypeError):
        raise ArtifactCompareError("invalid PPM dimensions: %s" % path)
    pixels = data[header_end + len(marker):]
    if width < AUTHENTIC_WIDTH or height <= 0 or len(pixels) != width * height * 3:
        raise ArtifactCompareError("invalid PPM payload: %s" % path)
    return width, height, pixels


def compare_authentic_center(left_path, right_path):
    left_width, left_height, left = read_ppm(left_path)
    right_width, right_height, right = read_ppm(right_path)
    if (left_width, left_height) != (right_width, right_height):
        return {
            "center_pixels": None,
            "changed_pixels": None,
            "error": "framebuffer dimensions differ: %dx%d vs %dx%d" % (
                left_width, left_height, right_width, right_height),
        }
    width, height = left_width, left_height
    left_margin = (width - AUTHENTIC_WIDTH) // 2
    right_margin = width - AUTHENTIC_WIDTH - left_margin
    center_changed = 0
    margin_changed = 0
    changed_left = 0
    changed_right = 0
    bounds = None
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * 3
            if left[offset:offset + 3] == right[offset:offset + 3]:
                continue
            if bounds is None:
                bounds = [x, y, x + 1, y + 1]
            else:
                bounds[0] = min(bounds[0], x)
                bounds[1] = min(bounds[1], y)
                bounds[2] = max(bounds[2], x + 1)
                bounds[3] = max(bounds[3], y + 1)
            if x < left_margin:
                changed_left += 1
                margin_changed += 1
            elif x >= width - right_margin:
                changed_right += 1
                margin_changed += 1
            else:
                center_changed += 1
    return {
        "width": width,
        "height": height,
        "authentic_x0": left_margin,
        "authentic_x1": left_margin + AUTHENTIC_WIDTH,
        "center_pixels": center_changed,
        "changed_pixels": center_changed + margin_changed,
        "left_margin_pixels": changed_left,
        "right_margin_pixels": changed_right,
        "bounds": bounds,
    }


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


def compare_manifests(left_path, right_path, framebuffer_policy="exact"):
    if framebuffer_policy not in ("exact", "authentic-center"):
        raise ArtifactCompareError(
            "unknown framebuffer policy: %s" % framebuffer_policy)
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
    framebuffer_differences = []
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
                if (relative == "shot.ppm" and
                        framebuffer_policy == "authentic-center"):
                    comparison = compare_authentic_center(
                        os.path.join(left[target], relative),
                        os.path.join(right[target], relative))
                    if comparison.get("center_pixels") == 0:
                        comparison["target"] = target
                        framebuffer_differences.append(comparison)
                        continue
                mismatches.append({
                    "target": target,
                    "artifact": relative,
                    "left_sha256": left_hash,
                    "right_sha256": right_hash,
                    **({"framebuffer": comparison}
                       if relative == "shot.ppm" and
                       framebuffer_policy == "authentic-center" else {}),
                })
    return {
        "targets": len(left),
        "artifacts": compared,
        "framebuffer_policy": framebuffer_policy,
        "framebuffer_differences": framebuffer_differences,
        "mismatches": mismatches,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left_manifest")
    parser.add_argument("right_manifest")
    parser.add_argument(
        "--framebuffer-policy", choices=("exact", "authentic-center"),
        default="exact",
        help="exact full frame, or exact centered 256px plus reported margins")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = compare_manifests(
            args.left_manifest, args.right_manifest, args.framebuffer_policy)
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
        changed = result["framebuffer_differences"]
        if changed:
            pixels = sum(item["changed_pixels"] for item in changed)
            print("%d targets, %d artifacts accepted; %d margin pixels differ "
                  "across %d framebuffer(s), authentic centers exact" % (
                      result["targets"], result["artifacts"], pixels,
                      len(changed)))
            for item in changed:
                print("  %s margins=%d/%d bbox=%s" % (
                    item["target"], item["left_margin_pixels"],
                    item["right_margin_pixels"], item["bounds"]))
        else:
            print("%d targets, %d artifacts exact" %
                  (result["targets"], result["artifacts"]))
    return 1 if result["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main())
