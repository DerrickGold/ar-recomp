"""SIM checkpoint artifacts: evidence and validation independent of command dispatch."""

from __future__ import annotations
import hashlib
import itertools
import json
from pathlib import Path
import platform
from ar_lib import write_rgb_png


def compare_d1_framebuffer_hashes(enhanced_path: Path, authentic_path: Path) -> dict:
    compared = mismatches = 0
    first_mismatch = None
    with (
        enhanced_path.open("r", encoding="utf-8") as enhanced,
        authentic_path.open("r", encoding="utf-8") as authentic,
    ):
        for line_number, pair in enumerate(itertools.zip_longest(enhanced, authentic), 1):
            left_line, right_line = pair
            if left_line is None or right_line is None:
                mismatches += 1
                if first_mismatch is None:
                    first_mismatch = {
                        "line": line_number,
                        "reason": "trace lengths differ",
                    }
                continue
            left = json.loads(left_line)
            right = json.loads(right_line)
            compared += 1
            left_key = (
                int(left["host_frame"]),
                int(left["game_frame"]),
                str(left["framebuffer_hash"]),
            )
            right_key = (
                int(right["host_frame"]),
                int(right["game_frame"]),
                str(right["framebuffer_hash"]),
            )
            if left_key != right_key:
                mismatches += 1
                if first_mismatch is None:
                    first_mismatch = {
                        "line": line_number,
                        "enhanced": left_key,
                        "authentic": right_key,
                    }
    return {
        "compared_frame_count": compared,
        "mismatch_count": mismatches,
        "first_mismatch": first_mismatch,
    }


def validate_d2_artifacts(prefix: Path) -> tuple[dict, list[str]]:
    """Prove D2's same-frame demo triplet is byte-exact and difference-free."""
    errors: list[str] = []
    paths = {
        "a": Path(f"{prefix}-A.ppm"),
        "b": Path(f"{prefix}-B.ppm"),
        "difference": Path(f"{prefix}-difference.ppm"),
        "metadata": Path(f"{prefix}.json"),
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        return {"paths": {key: str(value) for key, value in paths.items()}}, [
            f"D2 demo artifact(s) missing: {', '.join(missing)}"
        ]

    def read_ppm(path: Path) -> tuple[list[int], bytes]:
        chunks = path.read_bytes().split(b"\n", 3)
        if len(chunks) != 4 or chunks[0] != b"P6" or chunks[2] != b"255":
            raise ValueError(f"malformed P6 artifact: {path}")
        dimensions = [int(value) for value in chunks[1].split()]
        if len(dimensions) != 2 or len(chunks[3]) != dimensions[0] * dimensions[1] * 3:
            raise ValueError(f"invalid P6 dimensions/payload: {path}")
        return dimensions, chunks[3]

    dimensions_a, pixels_a = read_ppm(paths["a"])
    dimensions_b, pixels_b = read_ppm(paths["b"])
    dimensions_difference, pixels_difference = read_ppm(paths["difference"])
    metadata = json.loads(paths["metadata"].read_text(encoding="utf-8"))
    if dimensions_a != dimensions_b or dimensions_a != dimensions_difference:
        errors.append("D2 A/B/difference artifact dimensions differ")
    if pixels_a != pixels_b:
        errors.append("D2 A and B artifact pixels differ")
    nonzero_difference_bytes = sum(value != 0 for value in pixels_difference)
    if nonzero_difference_bytes:
        errors.append(f"D2 difference artifact has {nonzero_difference_bytes} nonzero byte(s)")
    if int(metadata.get("mismatch_pixels", -1)) != 0:
        errors.append(
            f"D2 artifact metadata mismatch_pixels is not zero: {metadata.get('mismatch_pixels')!r}"
        )

    png_paths = {
        "a_png": Path(f"{prefix}-A.png"),
        "b_png": Path(f"{prefix}-B.png"),
        "difference_png": Path(f"{prefix}-difference.png"),
    }
    write_rgb_png(png_paths["a_png"], *dimensions_a, pixels_a)
    write_rgb_png(png_paths["b_png"], *dimensions_b, pixels_b)
    write_rgb_png(png_paths["difference_png"], *dimensions_difference, pixels_difference)
    paths.update(png_paths)
    return {
        "paths": {key: str(value) for key, value in paths.items()},
        "dimensions": dimensions_a,
        "game_frame": int(metadata.get("game_frame", 0)),
        "mismatch_pixels": int(metadata.get("mismatch_pixels", -1)),
        "nonzero_difference_bytes": nonzero_difference_bytes,
        "a_sha256": hashlib.sha256(paths["a"].read_bytes()).hexdigest(),
        "b_sha256": hashlib.sha256(paths["b"].read_bytes()).hexdigest(),
        "difference_sha256": hashlib.sha256(paths["difference"].read_bytes()).hexdigest(),
    }, errors


def read_composite_series(run_dir: Path) -> list[tuple[int, str]]:
    """One run's composited screenshots, as (game_frame, sha256) pairs."""
    series = []
    for path in sorted(run_dir.glob("shot_*.ppm"), key=lambda item: int(item.stem.split("_")[1])):
        series.append((int(path.stem.split("_")[1]), hashlib.sha256(path.read_bytes()).hexdigest()))
    return series


def host_render_key() -> str:
    """Identifies the host whose GPU produced a composite reference.

    The screenshots read back the real swapchain, so their pixels belong to
    this machine's driver as much as to the source. Keying the reference means
    a second machine records its own instead of failing against someone
    else's.
    """
    return f"{platform.system()}-{platform.machine()}"


def validate_voxel_visual(
    output: Path, expected: dict, reference_root: Path
) -> tuple[dict, list[str]]:
    """Gate the background voxel town, which no other checkpoint renders.

    Four independent properties, because each catches a different failure and
    only two of them are portable:

    1. PRESENTATION-ONLY. The authentic framebuffer hashes must match the
       voxel-off pass frame for frame. The voxel renderer reads copies and
       holds no write path to the bus; this is what keeps that true.
    2. LIVENESS. The composited screenshots must DIFFER from the voxel-off
       pass. Without this the checkpoint passes just as happily when the
       renderer silently falls back to flat ground, which is the failure it
       exists to catch.
    3. DETERMINISM. A second identical pass must produce identical
       composites. The dynamic camera and the cloud drift both run on wall
       time, so a checkpoint that leaves either on compares noise -- and an
       A/B read against a non-deterministic capture reports a regression that
       is not there.
    4. REFERENCE. The composites must match what this host recorded last
       time. This is the only gate that catches a change which was supposed
       to be a no-op (ledger 74), and the only one that is not portable, so a
       host with no reference RECORDS one and says so rather than failing.
    """
    errors: list[str] = []
    run_dirs = sorted(
        (path for path in (output / "runs").glob("*") if path.is_dir() and path.name != "latest"),
        key=lambda item: item.name,
    )
    if len(run_dirs) != 3:
        return {"run_dirs": [str(path) for path in run_dirs]}, [
            "voxel checkpoint expected three replay passes "
            f"(voxel, voxel-off, repeat), found {len(run_dirs)}"
        ]
    voxel, voxel_off, repeat = (read_composite_series(path) for path in run_dirs)

    minimum = int(expected.get("composite_frames_minimum", 4))
    if len(voxel) < minimum:
        errors.append(
            f"voxel pass captured {len(voxel)} composited frames, expected at least {minimum}"
        )

    if [frame for frame, _ in voxel] != [frame for frame, _ in voxel_off]:
        errors.append(
            "voxel and voxel-off passes captured different frames; the two runs are not comparable"
        )
    else:
        changed = sum(1 for (_, a), (_, b) in zip(voxel, voxel_off) if a != b)
        changed_minimum = int(expected.get("changed_frames_minimum", 1))
        if changed < changed_minimum:
            errors.append(
                f"voxel town changed {changed} of {len(voxel)} composited "
                f"frames against AR_SIM3D_VOXEL_PRESET=Off, expected at least "
                f"{changed_minimum}; the renderer is not reaching the screen"
            )

    if voxel != repeat:
        first = next(
            (index for index, pair in enumerate(zip(voxel, repeat)) if pair[0] != pair[1]), None
        )
        errors.append(
            "voxel composite is not deterministic across two identical "
            f"passes; first difference at frame "
            f"{voxel[first][0] if first is not None else 'unknown'}"
        )

    key = host_render_key()
    reference_path = reference_root / f"{key}.json"
    reference_state = "compared"
    if not reference_path.is_file():
        reference_path.parent.mkdir(parents=True, exist_ok=True)
        reference_path.write_text(
            json.dumps(
                {
                    "schema": "actraiser-sim3d-voxel-reference-v1",
                    "host_render_key": key,
                    "composite": [
                        {"game_frame": frame, "sha256": digest} for frame, digest in voxel
                    ],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        reference_state = "recorded"
        print(f"[voxel] no reference for {key}; recorded {len(voxel)} frames -> {reference_path}")
    else:
        stored = json.loads(reference_path.read_text(encoding="utf-8"))
        want = [(int(item["game_frame"]), str(item["sha256"])) for item in stored["composite"]]
        if want != voxel:
            mismatched = [frame for (frame, a), (_, b) in zip(want, voxel) if a != b]
            errors.append(
                f"voxel composite differs from the {key} reference on "
                f"{len(mismatched) or 'a different set of'} frame(s) "
                f"{mismatched[:6]}; if the change was intended, delete "
                f"{reference_path} and re-run to re-record it"
            )

    return {
        "run_dirs": [str(path) for path in run_dirs],
        "composite_frame_count": len(voxel),
        "changed_against_voxel_off": sum(1 for (_, a), (_, b) in zip(voxel, voxel_off) if a != b),
        "deterministic": voxel == repeat,
        "host_render_key": key,
        "reference_path": str(reference_path),
        "reference_state": reference_state,
    }, errors


def validate_d3_artifact(
    output: Path,
    label: str,
    picker_topdown: bool,
    max_differing_pixels: int | None = None,
    expect_picker_unchanged: bool = False,
) -> tuple[dict, list[str]]:
    """Retain and compare the two requested geometry-profile readbacks."""
    candidates = sorted(
        path for path in output.glob("runs/*/shot.ppm") if path.parent.name != "latest"
    )
    picker_candidates = sorted(
        path for path in output.glob("runs/*/shot_1000.ppm") if path.parent.name != "latest"
    )
    if len(candidates) != 2:
        return {"candidate_paths": [str(path) for path in candidates]}, [
            f"{label} expected two renderer screenshots, found {len(candidates)}"
        ]

    def read_ppm(path: Path) -> tuple[list[int], bytes]:
        chunks = path.read_bytes().split(b"\n", 3)
        if len(chunks) != 4 or chunks[0] != b"P6" or chunks[2] != b"255":
            raise ValueError(f"{label} renderer screenshot is not P6: {path}")
        dimensions = [int(value) for value in chunks[1].split()]
        pixels = chunks[3]
        if len(dimensions) != 2 or len(pixels) != dimensions[0] * dimensions[1] * 3:
            raise ValueError(f"{label} screenshot dimensions/payload disagree: {path}")
        return dimensions, pixels

    dimensions, projected = read_ppm(candidates[0])
    authentic_dimensions, authentic = read_ppm(candidates[1])
    if len(picker_candidates) != 2:
        return {
            "candidate_paths": [str(path) for path in candidates],
            "picker_candidate_paths": [str(path) for path in picker_candidates],
        }, [f"{label} expected two picker screenshots, found {len(picker_candidates)}"]
    picker_dimensions, picker_enhanced = read_ppm(picker_candidates[0])
    picker_auth_dimensions, picker_authentic = read_ppm(picker_candidates[1])
    colors = {projected[index : index + 3] for index in range(0, len(projected), 3)}
    nonblack = sum(
        projected[index : index + 3] != b"\0\0\0" for index in range(0, len(projected), 3)
    )
    differing_pixels = sum(
        projected[index : index + 3] != authentic[index : index + 3]
        for index in range(0, min(len(projected), len(authentic)), 3)
    )
    errors = []
    if authentic_dimensions != dimensions:
        errors.append(f"{label} B/A screenshot dimensions differ")
    if len(colors) < 16:
        errors.append(f"{label} screenshot has only {len(colors)} unique colors")
    if nonblack < dimensions[0] * dimensions[1] // 10:
        errors.append(f"{label} screenshot is predominantly black")
    minimum_difference = dimensions[0] * dimensions[1] // 100 if label == "D3a" else 16
    if differing_pixels < minimum_difference:
        errors.append(f"{label} B/A profiles changed only {differing_pixels} output pixels")
    # An upper bound is what distinguishes "this stage refined something" from
    # "this stage moved the scene". A blur that shifts geometry, or a shadow
    # pass that leaks past the ground, blows through it immediately.
    if max_differing_pixels is not None and differing_pixels > max_differing_pixels:
        errors.append(
            f"{label} B/A profiles changed {differing_pixels} output pixels, "
            f"more than the {max_differing_pixels} this stage may touch"
        )
    picker_differing_pixels = sum(
        picker_enhanced[index : index + 3] != picker_authentic[index : index + 3]
        for index in range(0, min(len(picker_enhanced), len(picker_authentic)), 3)
    )
    if picker_dimensions != picker_auth_dimensions:
        errors.append(f"{label} enhanced/authentic picker dimensions differ")
    # With AR_SIM3D_PICKER_TOPDOWN=1 both profiles collapse to the same
    # authentic frame, so the picker screenshot must be pixel-identical. With
    # it compiled out the picker renders projected, and the two profiles differ
    # exactly where the stage under test differs -- which the B/A comparison
    # above already measures. Requiring equality there would assert the
    # opposite of the shipped behaviour.
    if expect_picker_unchanged and picker_differing_pixels:
        errors.append(
            f"{label} changed {picker_differing_pixels} picker pixels even "
            "though no effect emitter exists during that frame"
        )
    elif picker_topdown and picker_differing_pixels:
        errors.append(f"{label} picker fallback differs by {picker_differing_pixels} pixels")
    if (
        not expect_picker_unchanged
        and not picker_topdown
        and not picker_differing_pixels
        and differing_pixels
    ):
        errors.append(
            f"{label} projected picker frame is identical across profiles "
            "while ordinary frames differ; the picker may still be falling "
            "back to the authentic view"
        )

    width, height = dimensions

    projected_png = output / f"{label}-B.png"
    authentic_png = output / f"{label}-A.png"
    difference_png = output / f"{label}-difference.png"
    picker_png = output / f"{label}-picker-top-down.png"
    difference = bytes(
        abs(projected[i] - authentic[i]) for i in range(min(len(projected), len(authentic)))
    )
    write_rgb_png(projected_png, width, height, projected)
    write_rgb_png(authentic_png, width, height, authentic)
    if len(difference) == len(projected):
        write_rgb_png(difference_png, width, height, difference)
    if picker_dimensions == dimensions:
        write_rgb_png(picker_png, width, height, picker_enhanced)
    return {
        "projected_source_path": str(candidates[0]),
        "authentic_source_path": str(candidates[1]),
        "projected_png_path": str(projected_png),
        "authentic_png_path": str(authentic_png),
        "difference_png_path": str(difference_png),
        "picker_png_path": str(picker_png),
        "dimensions": dimensions,
        "unique_colors": len(colors),
        "nonblack_pixels": nonblack,
        "differing_pixels": differing_pixels,
        "picker_differing_pixels": picker_differing_pixels,
        "projected_sha256": hashlib.sha256(candidates[0].read_bytes()).hexdigest(),
        "authentic_sha256": hashlib.sha256(candidates[1].read_bytes()).hexdigest(),
    }, errors
