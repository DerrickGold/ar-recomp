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
``--snapshot-vram-policy provider-owned`` accepts changed VRAM words only inside
an eligible BG1/BG2 64x64 tilemap whose authentic-ring census is exact on both
sides. This pins the intentional BH8 removal of offscreen host repair writes.
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
VRAM_WORDS = 0x8000
TILEMAP_WORDS_64X64 = 0x1000


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
        by_target[target] = result
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


def read_vram_words(path):
    try:
        with open(path, "rb") as source:
            payload = source.read()
    except OSError as error:
        raise ArtifactCompareError("cannot read %s: %s" % (path, error))
    if len(payload) != VRAM_WORDS * 2:
        raise ArtifactCompareError("invalid VRAM payload: %s" % path)
    return [payload[index] | (payload[index + 1] << 8)
            for index in range(0, len(payload), 2)]


def snapshot_record(result, relative):
    name = os.path.basename(relative)[:-len(".vram.bin")]
    matches = [record for record in result.get("snapshots", [])
               if os.path.basename(record.get("snapshot", "")) == name]
    if len(matches) != 1:
        raise ArtifactCompareError(
            "cannot resolve census metadata for %s target %s" %
            (relative, result.get("target")))
    return matches[0]


def eligible_tilemap_ranges(record):
    ranges = []
    for layer in record.get("layers", []):
        comparison = layer.get("comparison", {})
        ppu = layer.get("ppu", {})
        base = layer.get("tilemap_base")
        if (not comparison.get("available") or
                comparison.get("mismatches") != 0 or
                comparison.get("outside_world") != 0 or
                ppu.get("eligible") is not True or
                not isinstance(base, int) or
                base < 0 or base + TILEMAP_WORDS_64X64 > VRAM_WORDS):
            continue
        ranges.append((base, base + TILEMAP_WORDS_64X64))
    return ranges


def compact_word_ranges(addresses):
    if not addresses:
        return []
    ranges = []
    first = previous = addresses[0]
    for address in addresses[1:]:
        if address != previous + 1:
            ranges.append([first, previous + 1])
            first = address
        previous = address
    ranges.append([first, previous + 1])
    return ranges


def format_word_ranges(ranges, limit=8):
    shown = ranges[:limit]
    text = ",".join("$%04X-$%04X" % (start, end - 1)
                    for start, end in shown)
    if len(ranges) > limit:
        text += ",...(+%d ranges)" % (len(ranges) - limit)
    return text


def compare_provider_owned_vram(left_path, right_path, left_result,
                                right_result, relative):
    left_words = read_vram_words(left_path)
    right_words = read_vram_words(right_path)
    changed = [address for address, words in
               enumerate(zip(left_words, right_words)) if words[0] != words[1]]
    left_ranges = eligible_tilemap_ranges(
        snapshot_record(left_result, relative))
    right_ranges = eligible_tilemap_ranges(
        snapshot_record(right_result, relative))
    shared_ranges = [entry for entry in left_ranges if entry in right_ranges]
    outside = [address for address in changed
               if not any(start <= address < end
                          for start, end in shared_ranges)]
    return {
        "changed_words": len(changed),
        "ranges": compact_word_ranges(changed),
        "outside_provider_tilemaps": len(outside),
        "outside_ranges": compact_word_ranges(outside),
    }


def compare_manifests(left_path, right_path, framebuffer_policy="exact",
                      snapshot_vram_policy="exact"):
    if framebuffer_policy not in ("exact", "authentic-center"):
        raise ArtifactCompareError(
            "unknown framebuffer policy: %s" % framebuffer_policy)
    if snapshot_vram_policy not in ("exact", "provider-owned"):
        raise ArtifactCompareError(
            "unknown snapshot VRAM policy: %s" % snapshot_vram_policy)
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
    vram_differences = []
    compared = 0
    for target in sorted(left):
        left_result = left[target]
        right_result = right[target]
        left_run = left_result["run_directory"]
        right_run = right_result["run_directory"]
        left_artifacts = discover_artifacts(left_run, left_snapshots)
        right_artifacts = discover_artifacts(right_run, right_snapshots)
        if left_artifacts != right_artifacts:
            raise ArtifactCompareError(
                "artifact sets differ for target %s" % target)
        for relative in left_artifacts:
            compared += 1
            left_artifact = os.path.join(left_run, relative)
            right_artifact = os.path.join(right_run, relative)
            left_hash = file_sha256(left_artifact)
            right_hash = file_sha256(right_artifact)
            if left_hash != right_hash:
                if (relative == "shot.ppm" and
                        framebuffer_policy == "authentic-center"):
                    comparison = compare_authentic_center(
                        left_artifact, right_artifact)
                    if comparison.get("center_pixels") == 0:
                        comparison["target"] = target
                        framebuffer_differences.append(comparison)
                        continue
                if (relative.endswith(".vram.bin") and
                        snapshot_vram_policy == "provider-owned"):
                    comparison = compare_provider_owned_vram(
                        left_artifact, right_artifact, left_result,
                        right_result, relative)
                    if (comparison["changed_words"] and
                            not comparison["outside_provider_tilemaps"]):
                        comparison["target"] = target
                        comparison["artifact"] = relative
                        vram_differences.append(comparison)
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
        "snapshot_vram_policy": snapshot_vram_policy,
        "vram_differences": vram_differences,
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
    parser.add_argument(
        "--snapshot-vram-policy", choices=("exact", "provider-owned"),
        default="exact",
        help="exact VRAM, or allow changes confined to provider-eligible "
             "tilemaps with exact authentic-ring census")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = compare_manifests(
            args.left_manifest, args.right_manifest, args.framebuffer_policy,
            args.snapshot_vram_policy)
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
        vram = result["vram_differences"]
        if changed or vram:
            pixels = sum(item["changed_pixels"] for item in changed)
            words = sum(item["changed_words"] for item in vram)
            print("%d targets, %d artifacts accepted; %d margin pixels differ "
                  "across %d framebuffer(s), %d provider-owned VRAM words "
                  "differ across %d snapshot(s)" % (
                      result["targets"], result["artifacts"], pixels,
                      len(changed), words, len(vram)))
            for item in changed:
                print("  %s margins=%d/%d bbox=%s" % (
                    item["target"], item["left_margin_pixels"],
                    item["right_margin_pixels"], item["bounds"]))
            for item in vram:
                print("  %s %s words=%d ranges=%s" % (
                    item["target"], item["artifact"], item["changed_words"],
                    format_word_ranges(item["ranges"])))
        else:
            print("%d targets, %d artifacts exact" %
                  (result["targets"], result["artifacts"]))
    return 1 if result["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main())
