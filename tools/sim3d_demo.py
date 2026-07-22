#!/usr/bin/env python3
"""Run bounded, deterministic SIM 3D implementation checkpoints."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "fixtures" / "sim3d" / "checkpoints.json"
PICKER_ROUTINES = {
    "0193DC": "type_0b_position_picker",
    "01972F": "direct_people",
    "019754": "targeted_miracle",
}
SIM3D_CAPTURE_STATUS_NAMES = {
    0: "inactive",
    1: "master_off",
    2: "not_requested",
    3: "picker",
    4: "no_renderer",
    5: "overlay_conflict",
    6: "unsupported_ppu",
    7: "unsupported_color_math",
    8: "allocation_failure",
    9: "capturing",
    10: "atlas_invalid",
    11: "pixel_mismatch",
    12: "ready",
}
SIM3D_CAPTURE_READY = 12
# Every stage toggle a checkpoint must pin once it pins any of them. A
# checkpoint that names only some stages silently inherits the shipped default
# for the rest, so landing a new stage would change what older checkpoints
# render -- which is exactly what happened when soft shadows landed. Add a new
# stage here and to every manifest env/baseline_env at the same time.
SIM3D_STAGE_ENV = (
    "AR_SIM3D_SEPARATED", "AR_SIM3D_GROUND", "AR_SIM3D_BILLBOARDS",
    "AR_SIM3D_HEIGHT", "AR_SIM3D_SHADOWS", "AR_SIM3D_SOFT_SHADOWS",
    "AR_SIM3D_RIM_LIGHT", "AR_SIM3D_WORLD_UNDERLAY", "AR_SIM3D_CLOUDS",
)
# Only these D3c presentation classes may lift a billboard off the ground.
LIFTED_HEIGHT_CLASSES = ("flying", "flying_projectile", "semi_grounded")
# ROM-positioned contact classes must land exactly, never ease into place.
CONTACT_EXACT_HEIGHT_CLASSES = ("ground_effect", "ground_strike")
MAP_PLANE_TRAIT = 1 << 0
NO_SHADOW_TRAIT = 1 << 2
# kSimHeightSlewStep in sim_render_metadata.h.
HEIGHT_SLEW_STEP = 4


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "actraiser-sim3d-checkpoints-v1":
        raise ValueError(f"unsupported checkpoint manifest schema in {path}")
    return data


def resolve(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def picker_ranges(frames: list[dict]) -> list[list[int]]:
    values = [int(frame["game_frame"]) for frame in frames
              if frame.get("picker_active")]
    if not values:
        return []
    ranges: list[list[int]] = []
    start = prior = values[0]
    for value in values[1:]:
        if value != prior + 1:
            ranges.append([start, prior])
            start = value
        prior = value
    ranges.append([start, prior])
    return ranges


def picker_transitions(frames: list[dict]) -> list[dict]:
    active = [frame for frame in frames if frame.get("picker_active")]
    if not active:
        return []

    groups: list[list[dict]] = [[active[0]]]
    for frame in active[1:]:
        if int(frame["game_frame"]) != int(groups[-1][-1]["game_frame"]) + 1:
            groups.append([])
        groups[-1].append(frame)

    transitions: list[dict] = []
    for group in groups:
        pending_types = sorted({int(frame.get("pending_world_type", 0))
                                for frame in group})
        if pending_types == [0x000B]:
            type_evidence = "type_0b_position_picker"
        elif pending_types == [0x0009]:
            type_evidence = "type_09_picker_ambiguous"
        else:
            type_evidence = "unknown_or_mixed"
        first = group[0]
        last = group[-1]
        transitions.append({
            "start_game_frame": int(first["game_frame"]),
            "end_game_frame": int(last["game_frame"]),
            "duration_frames": len(group),
            "pending_world_types": pending_types,
            "type_evidence": type_evidence,
            "aimed_cell_start": [int(first["aimed_cell_x"]),
                                 int(first["aimed_cell_y"])],
            "aimed_cell_end": [int(last["aimed_cell_x"]),
                               int(last["aimed_cell_y"])],
            "sim_target_start": [int(first["sim_target_x"]),
                                 int(first["sim_target_y"])],
            "sim_target_end": [int(last["sim_target_x"]),
                               int(last["sim_target_y"])],
        })
    return transitions


def summarize(frames: list[dict], routine_calls: list[dict],
              object_evidence: dict) -> dict:
    color_states = {
        (frame["ppu"]["windowsel"], frame["ppu"]["cgwsel"],
         frame["ppu"]["cgadsub"])
        for frame in frames if frame.get("ppu") is not None
    }
    visible_town_frames = [
        frame for frame in frames
        if frame.get("town") and frame.get("ppu") is not None and
        int(frame["ppu"].get("inidisp", 0x80)) != 0x80
    ]
    return {
        "frame_count": len(frames),
        "towns": sorted({int(frame["map"]) for frame in frames
                         if frame.get("town")}),
        "views": sorted({str(frame["expected_view"]) for frame in frames}),
        "picker_frame_count": sum(bool(frame.get("picker_active"))
                                  for frame in frames),
        "picker_ranges": picker_ranges(frames),
        "picker_transitions": picker_transitions(frames),
        "picker_pending_world_types": sorted({
            int(frame.get("pending_world_type", 0))
            for frame in frames if frame.get("picker_active")
        }),
        "picker_routine_calls": routine_calls,
        "picker_operations": [call["operation"] for call in routine_calls],
        "miracle_kinds": sorted({int(frame.get("miracle_kind", 0))
                                 for frame in frames
                                 if int(frame.get("miracle_kind", 0)) != 0}),
        "object_evidence": object_evidence,
        "ppu_modes": sorted({int(frame["ppu"]["bgmode"]) for frame in frames
                             if frame.get("ppu") is not None}),
        "color_state_count": len(color_states),
        "first_game_frame": int(frames[0]["game_frame"]) if frames else None,
        "last_game_frame": int(frames[-1]["game_frame"]) if frames else None,
        "visible_town_camera_x_min": min(
            (int(frame["camera_x"]) for frame in visible_town_frames),
            default=None),
        "visible_town_camera_x_max": max(
            (int(frame["camera_x"]) for frame in visible_town_frames),
            default=None),
    }


def validate(summary: dict, expected: dict) -> list[str]:
    errors: list[str] = []
    for field in ("towns", "views", "ppu_modes"):
        if field in expected and summary[field] != expected[field]:
            errors.append(
                f"{field}: expected {expected[field]!r}, got {summary[field]!r}")
    for field in ("picker_ranges", "picker_pending_world_types",
                  "picker_operations", "miracle_kinds"):
        if field in expected and summary[field] != expected[field]:
            errors.append(
                f"{field}: expected {expected[field]!r}, got {summary[field]!r}")
    for field in ("visible_town_camera_x_min",
                  "visible_town_camera_x_max"):
        if field in expected and summary[field] != expected[field]:
            errors.append(
                f"{field}: expected {expected[field]!r}, "
                f"got {summary[field]!r}")
    minimum = int(expected.get("picker_frames_min", 0))
    if summary["picker_frame_count"] < minimum:
        errors.append(
            "picker_frame_count: expected at least "
            f"{minimum}, got {summary['picker_frame_count']}")
    for category in expected.get("object_evidence", []):
        if category not in summary["object_evidence"]:
            errors.append(f"object_evidence: missing {category!r}")
    if not summary["frame_count"]:
        errors.append("trace contains no SIM-town frames")
    return errors


def read_trace(path: Path) -> list[dict]:
    frames: list[dict] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            try:
                frames.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
    return frames


def read_picker_routine_calls(path: Path) -> list[dict]:
    calls: list[dict] = []
    if not path.is_file():
        return calls
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            try:
                event = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
            pc = str(event.get("pc", "")).upper()
            operation = PICKER_ROUTINES.get(pc)
            if not operation:
                continue
            calls.append({
                "operation": operation,
                "routine": f"${pc[:2]}:{pc[2:]}",
                "host_frame": int(event["hf"]),
                "game_frame": int(event["gf"]),
            })
    return calls


def read_object_evidence(path: Path) -> dict:
    categories: dict[str, list[dict]] = {}
    if not path.is_file():
        return categories
    for line in path.read_text(errors="replace").splitlines():
        if "[simcat]" not in line or "tier=W" not in line:
            continue
        fields = dict(re.findall(r"([a-zA-Z0-9_]+)=([^ ]+)", line))
        try:
            frame = int(fields["frame"], 16)
            event = {
                "game_frame": int(fields["gf"], 10),
                "composition": frame,
                "record": int(fields["rec"], 16),
                "type": int(fields["type"], 16),
            }
        except (KeyError, ValueError):
            continue
        if frame in (0xE1BD, 0xE209, 0xE255):
            category = "blue_demon_lightning"
        elif frame in (0xE71B, 0xE73A, 0xE75E):
            category = "napper_ground_pluck"
        elif frame in (0xE6CA, 0xE6D0, 0xE6D6):
            category = "ground_fire"
        elif 0xE676 <= frame <= 0xE6B5:
            category = "ground_people"
        else:
            continue
        categories.setdefault(category, []).append(event)

    evidence = {}
    for category, events in categories.items():
        evidence[category] = {
            "event_count": len(events),
            "first_game_frame": min(event["game_frame"] for event in events),
            "last_game_frame": max(event["game_frame"] for event in events),
            "compositions": [f"${value:04X}" for value in sorted({
                event["composition"] for event in events
            })],
            "record_types": [f"${value:02X}" for value in sorted({
                event["type"] for event in events
            })],
        }
    return evidence


def read_d1_metadata(path: Path) -> tuple[dict, list[str]]:
    """Validate D1 metadata and D2's flat-composite gate while streaming."""
    errors: list[str] = []
    error_count = 0
    frame_count = valid_count = invalid_count = 0
    picker_count = fallback_count = 0
    atlas_valid_count = atlas_invalid_count = atlas_valid_frames = 0
    separated_valid_count = separated_ready_count = 0
    separated_mismatch_total = separated_mismatch_max = 0
    max_atlas_used_width = max_atlas_used_height = 0
    max_sources = max_objects = max_emitted = 0
    requested_features: set[int] = set()
    effective_features: set[int] = set()
    integrity_flags: set[int] = set()
    priorities: set[int] = set()
    hashes: set[str] = set()
    separated_hashes: set[str] = set()
    projection_cameras: set[tuple[int, int, int]] = set()
    separated_status_counts: dict[int, int] = {}
    height_class_counts: dict[str, int] = {}
    max_virtual_height = max_classified_height = lifted_object_count = 0
    height_ramp_steps = height_slew_violations = 0
    shadow_caster_count = shadow_caster_lifted_count = 0
    shadow_opacity_values: set[int] = set()
    shadow_softness_values: set[int] = set()
    light_values: set[tuple[int, int]] = set()
    picker_topdown_values: set[bool] = set()
    picker_flag_frames = 0
    picker_flag_topdown_frames = picker_flag_enhanced_frames = 0
    picker_flag_map_plane_frames = 0
    height_carry: dict[int, int] = {}
    previous_height_view = None
    previous_height_serial = None
    picker_exit_frames: list[dict] = []
    previous_view = None
    first_serial = last_serial = None

    def issue(line_number: int, message: str) -> None:
        nonlocal error_count
        error_count += 1
        if len(errors) < 20:
            errors.append(f"D1 metadata line {line_number}: {message}")

    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            try:
                frame = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
            frame_count += 1
            valid = bool(frame.get("metadata_valid"))
            valid_count += valid
            invalid_count += not valid
            picker_count += frame.get("view") == "authentic_picker"
            fallback_count += frame.get("view") == "authentic_fallback"
            view = str(frame.get("view", ""))
            serial = int(frame.get("build_serial", 0))
            if first_serial is None:
                first_serial = serial
            last_serial = serial

            requested_features.add(int(frame.get("requested", 0)))
            effective_features.add(int(frame.get("effective", 0)))
            effective_separated = int(frame.get("effective", 0)) & 1
            flags = int(frame.get("integrity_flags", 0))
            integrity_flags.add(flags)
            hashes.add(str(frame.get("framebuffer_hash", "")))
            separated_valid = bool(frame.get("separated_valid"))
            separated_status = int(frame.get("separated_status", 0))
            separated_mismatch = int(
                frame.get("separated_mismatch_pixels", 0))
            separated_valid_count += separated_valid
            separated_ready_count += separated_status == SIM3D_CAPTURE_READY
            separated_mismatch_total += separated_mismatch
            separated_mismatch_max = max(
                separated_mismatch_max, separated_mismatch)
            separated_status_counts[separated_status] = (
                separated_status_counts.get(separated_status, 0) + 1)
            separated_hash = str(frame.get("separated_hash", ""))
            camera = frame.get("projection_camera", [0, 0, 0])
            if len(camera) == 3:
                projection_cameras.add(tuple(int(value) for value in camera))
            if separated_valid:
                separated_hashes.add(separated_hash)
                if separated_status != SIM3D_CAPTURE_READY:
                    issue(line_number,
                          "separated_valid frame does not report ready")
                if separated_mismatch:
                    issue(line_number,
                          "separated_valid frame has pixel mismatches")
            if separated_status == SIM3D_CAPTURE_READY and not separated_valid:
                issue(line_number, "ready capture is not separated_valid")
            if effective_separated and not separated_valid:
                issue(line_number,
                      "separated feature is effective without a valid capture")

            # The binary reports its compiled picker policy so the checkpoint
            # asserts the contract that actually shipped, instead of silently
            # passing because the frames it was written for no longer exist.
            picker_topdown_values.add(bool(frame.get("picker_topdown", True)))

            sources = frame.get("sources", [])
            objects = frame.get("objects", [])
            frame_source_records = {int(source.get("record", 0))
                                    for source in sources}
            shadow_opacity_values.add(int(frame.get("shadow_opacity_pct", 0)))
            shadow_softness_values.add(
                int(frame.get("shadow_softness_pct", 0)))
            light = frame.get("light", [0, 0])
            light_values.add((int(light[0]), int(light[1])))

            if int(frame.get("picker_flag", 0)):
                picker_flag_frames += 1
                if view == "authentic_picker":
                    picker_flag_topdown_frames += 1
                elif view == "enhanced":
                    picker_flag_enhanced_frames += 1
                    # The selector must still be painted onto the map square
                    # rather than billboarded while the picker is projected.
                    if any((int(obj.get("traits", 0)) & MAP_PLANE_TRAIT)
                           for obj in objects):
                        picker_flag_map_plane_frames += 1
            if previous_view == "authentic_picker" and \
                    view != "authentic_picker":
                picker_exit_frames.append({
                    "game_frame": int(frame.get("game_frame", 0)),
                    "view": view,
                    "separated_status": separated_status,
                    "separated_valid": separated_valid,
                    "effective": int(frame.get("effective", 0)),
                    "map_plane_object_count": sum(
                        (int(obj.get("traits", 0)) & 1) != 0
                        for obj in objects),
                })
            previous_view = view
            emitted = int(frame.get("emitted_oam_count", 0))
            claimed = int(frame.get("claimed_oam_count", 0))
            max_sources = max(max_sources, len(sources))
            max_objects = max(max_objects, len(objects))
            max_emitted = max(max_emitted, emitted)
            frame_atlas_valid = bool(frame.get("atlas_valid"))
            atlas_valid_frames += frame_atlas_valid
            object_atlas_valid = sum(bool(obj.get("atlas_valid"))
                                     for obj in objects)
            atlas_valid_count += object_atlas_valid
            atlas_invalid_count += len(objects) - object_atlas_valid
            priorities.update(int(obj["priority"]) for obj in objects)

            atlas_size = frame.get("atlas_size", [0, 0])
            atlas_used = frame.get("atlas_used", [0, 0])
            if len(atlas_size) != 2 or len(atlas_used) != 2:
                issue(line_number, "atlas size/used descriptor is malformed")
                atlas_width = atlas_height = used_width = used_height = 0
            else:
                atlas_width, atlas_height = map(int, atlas_size)
                used_width, used_height = map(int, atlas_used)
            max_atlas_used_width = max(max_atlas_used_width, used_width)
            max_atlas_used_height = max(max_atlas_used_height, used_height)
            if valid and not frame_atlas_valid:
                issue(line_number, "valid metadata frame has no valid atlas")
            if frame_atlas_valid and (atlas_width <= 0 or atlas_height <= 0 or
                                      used_width < 0 or used_height < 0 or
                                      used_width > atlas_width or
                                      used_height > atlas_height):
                issue(line_number, "atlas frame bounds are invalid")
            atlas_rects: list[tuple[int, int, int, int]] = []
            for object_index, obj in enumerate(objects):
                if not bool(obj.get("atlas_valid")):
                    if frame_atlas_valid:
                        issue(line_number,
                              f"object {object_index} lacks an atlas rect")
                    continue
                rect = [int(value) for value in obj.get("atlas", [])]
                local = [int(value) for value in
                         obj.get("local_bounds", [])]
                if len(rect) != 4 or len(local) != 4:
                    issue(line_number,
                          f"object {object_index} atlas descriptor malformed")
                    continue
                x, y, width, height = rect
                if width <= 0 or height <= 0 or x < 0 or y < 0 or \
                        x + width > used_width or y + height > used_height:
                    issue(line_number,
                          f"object {object_index} atlas rect out of used bounds")
                if local[2] - local[0] != width or \
                        local[3] - local[1] != height:
                    issue(line_number,
                          f"object {object_index} local/atlas size mismatch")
                for prior_index, (px, py, pw, ph) in enumerate(atlas_rects):
                    if x < px + pw and x + width > px and \
                            y < py + ph and y + height > py:
                        issue(line_number,
                              f"objects {prior_index}/{object_index} "
                              "overlap in atlas")
                atlas_rects.append((x, y, width, height))

            if int(frame.get("source_count", -1)) != len(sources):
                issue(line_number, "source_count does not match sources[]")
            if int(frame.get("object_count", -1)) != len(objects):
                issue(line_number, "object_count does not match objects[]")
            if emitted != claimed:
                issue(line_number, f"emitted {emitted} != claimed {claimed}")
            if sum(int(source["oam_count"]) for source in sources) != emitted:
                issue(line_number, "source OAM counts do not sum to emitted")
            if sum(int(obj["oam_count"]) for obj in objects) != emitted:
                issue(line_number, "object OAM counts do not sum to emitted")

            cursor = 0
            for source_index, source in enumerate(sources):
                first = int(source["oam_first"])
                count = int(source["oam_count"])
                fragment_first = int(source["fragment_first"])
                fragment_count = int(source["fragment_count"])
                if first != cursor:
                    issue(line_number,
                          f"source {source_index} starts {first}, expected {cursor}")
                cursor += count
                if fragment_first < 0 or fragment_count < 0 or \
                        fragment_first + fragment_count > len(objects):
                    issue(line_number,
                          f"source {source_index} fragment range is out of bounds")
                    continue
                fragments = objects[
                    fragment_first:fragment_first + fragment_count]
                if sum(int(obj["oam_count"]) for obj in fragments) != count:
                    issue(line_number,
                          f"source {source_index} fragment counts do not match")
                fragment_cursor = first
                for obj in fragments:
                    if int(obj["source_index"]) != source_index or \
                            int(obj["record"]) != int(source["record"]):
                        issue(line_number,
                              f"source {source_index} owns a foreign fragment")
                    if int(obj["oam_first"]) != fragment_cursor:
                        issue(line_number,
                              f"source {source_index} fragment has an OAM gap")
                    fragment_cursor += int(obj["oam_count"])
            if cursor != emitted:
                issue(line_number, f"final source cursor {cursor} != {emitted}")

            # D3c: the classification is a data table, so the replay only has
            # to prove that every published descriptor obeys its own policy.
            # Easing is only continuous between consecutive enhanced builds;
            # a picker, a fallback, or a build gap legitimately snaps.
            ramp_comparable = (view == "enhanced" and
                               previous_height_view == "enhanced" and
                               previous_height_serial is not None and
                               serial == previous_height_serial + 1)
            previous_heights = height_carry if ramp_comparable else {}
            frame_heights: dict[int, int] = {}

            # `virtual_height` is the eased presentation value, so it may sit
            # between planes while a record ramps. The classifier's own choice
            # is what has to obey the policy table.
            for object_index, obj in enumerate(objects):
                height_class = str(obj.get("height_class_name", "missing"))
                classified = int(obj.get("classified_height", 0))
                height = int(obj.get("virtual_height", 0))
                traits = int(obj.get("traits", 0))
                record = int(obj.get("record", 0))
                height_class_counts[height_class] = (
                    height_class_counts.get(height_class, 0) + 1)
                max_virtual_height = max(max_virtual_height, height)
                max_classified_height = max(max_classified_height, classified)
                if height:
                    lifted_object_count += 1
                if height < 0 or classified < 0:
                    issue(line_number,
                          f"object {object_index} has a negative height")
                if classified and height_class not in LIFTED_HEIGHT_CLASSES:
                    issue(line_number,
                          f"object {object_index} classifies {classified} px "
                          f"as {height_class}")
                if classified and (traits & MAP_PLANE_TRAIT):
                    issue(line_number,
                          f"object {object_index} is lifted and map-planed")
                # A ROM-positioned contact class must land exactly, never ease.
                if height_class in CONTACT_EXACT_HEIGHT_CLASSES and height:
                    issue(line_number,
                          f"object {object_index} eases a contact-exact "
                          f"{height_class} to {height} px")
                # The published height may only move toward the classified
                # plane, and only by the documented step.
                previous = previous_heights.get(record)
                contact_exact = height_class in CONTACT_EXACT_HEIGHT_CLASSES
                if previous is not None and height != previous:
                    step = abs(height - previous)
                    # Entering a contact class is a deliberate snap: the strike
                    # must be on the ground for its very first frame.
                    if step > HEIGHT_SLEW_STEP and not contact_exact:
                        height_slew_violations += 1
                        issue(line_number,
                              f"object {object_index} jumped {step} px "
                              f"({previous} -> {height})")
                    elif (height - previous) * (classified - previous) < 0:
                        issue(line_number,
                              f"object {object_index} eased away from its "
                              f"classified {classified} px plane")
                    else:
                        height_ramp_steps += 1
                frame_heights[record] = height
                if int(obj["tier"]) == 0 and (
                        classified or height or
                        height_class not in ("none", "map_plane")):
                    issue(line_number,
                          f"fixed object {object_index} entered the height "
                          f"system as {height_class}/{classified}")

                # D4a: recompute caster selection from the trait data rather
                # than trusting the flag, so a classification regression shows
                # up here instead of as a stray silhouette on the ground.
                casts = bool(obj.get("casts_shadow", False))
                expected_cast = (int(obj["tier"]) == 1 and
                                 bool(obj.get("atlas_valid", False)) and
                                 not (traits &
                                      (MAP_PLANE_TRAIT | NO_SHADOW_TRAIT)))
                if casts and not expected_cast:
                    issue(line_number,
                          f"object {object_index} casts a shadow despite "
                          f"tier/traits {int(obj['tier'])}/{traits}")
                if casts:
                    shadow_caster_count += 1
                    if height:
                        shadow_caster_lifted_count += 1
                    # A silhouette is drawn from the live object list only, so
                    # a caster must always still own a record this frame.
                    if record not in frame_source_records:
                        issue(line_number,
                              f"object {object_index} casts a shadow for "
                              f"record ${record:04X}, which this frame's "
                              f"source list no longer contains")

            height_carry = frame_heights
            previous_height_view = view
            previous_height_serial = serial

            world_objects = [obj for obj in objects if int(obj["tier"]) == 1]
            world_count = int(frame.get("world_oam_count", 0))
            if sum(int(obj["oam_count"]) for obj in world_objects) != world_count:
                issue(line_number, "world fragments do not equal world suffix count")
            if world_objects:
                world_first = int(frame["world_oam_first"])
                if int(world_objects[0]["oam_first"]) != world_first:
                    issue(line_number, "world suffix begins at the wrong slot")

    if error_count > len(errors):
        errors.append(f"D1 metadata: {error_count - len(errors)} more error(s)")
    summary = {
        "frame_count": frame_count,
        "valid_frame_count": valid_count,
        "invalid_frame_count": invalid_count,
        "picker_frame_count": picker_count,
        "fallback_frame_count": fallback_count,
        "first_build_serial": first_serial,
        "last_build_serial": last_serial,
        "requested": sorted(requested_features),
        "effective": sorted(effective_features),
        "integrity_flags": sorted(integrity_flags),
        "priority_bands": sorted(priorities),
        "max_source_count": max_sources,
        "max_object_count": max_objects,
        "max_emitted_oam_count": max_emitted,
        "atlas_valid_frame_count": atlas_valid_frames,
        "atlas_valid_object_count": atlas_valid_count,
        "atlas_invalid_object_count": atlas_invalid_count,
        "max_atlas_used_width": max_atlas_used_width,
        "max_atlas_used_height": max_atlas_used_height,
        "unique_framebuffer_hashes": len(hashes),
        "separated_valid_frame_count": separated_valid_count,
        "separated_ready_frame_count": separated_ready_count,
        "separated_statuses": sorted(separated_status_counts),
        "separated_status_counts": {
            SIM3D_CAPTURE_STATUS_NAMES.get(status, str(status)): count
            for status, count in sorted(separated_status_counts.items())
        },
        "height_class_counts": dict(sorted(height_class_counts.items())),
        "max_virtual_height": max_virtual_height,
        "max_classified_height": max_classified_height,
        "lifted_object_count": lifted_object_count,
        "height_ramp_step_count": height_ramp_steps,
        "height_slew_violation_count": height_slew_violations,
        "shadow_caster_count": shadow_caster_count,
        "shadow_caster_lifted_count": shadow_caster_lifted_count,
        "shadow_opacity_values": sorted(shadow_opacity_values),
        "shadow_softness_values": sorted(shadow_softness_values),
        "light_values": [list(value) for value in sorted(light_values)],
        "picker_topdown_build": (sorted(picker_topdown_values)[0]
                                 if len(picker_topdown_values) == 1 else None),
        "picker_flag_frame_count": picker_flag_frames,
        "picker_flag_topdown_frame_count": picker_flag_topdown_frames,
        "picker_flag_enhanced_frame_count": picker_flag_enhanced_frames,
        "picker_flag_map_plane_frame_count": picker_flag_map_plane_frames,
        "picker_exit_frames": picker_exit_frames,
        "separated_mismatch_pixels_total": separated_mismatch_total,
        "separated_mismatch_pixels_max": separated_mismatch_max,
        "unique_separated_hashes": len(separated_hashes),
        "projection_cameras": [list(camera)
                               for camera in sorted(projection_cameras)],
        "accounting_error_count": error_count,
    }
    return summary, errors


def validate_d1_summary(summary: dict, expected: dict) -> list[str]:
    errors: list[str] = []
    exact_fields = ("requested", "effective", "integrity_flags",
                    "separated_statuses", "projection_cameras")
    for field in exact_fields:
        if field in expected and summary[field] != expected[field]:
            errors.append(
                f"D1 {field}: expected {expected[field]!r}, "
                f"got {summary[field]!r}")
    minima = {
        "frame_count_min": "frame_count",
        "valid_frames_min": "valid_frame_count",
        "max_sources_min": "max_source_count",
        "max_objects_min": "max_object_count",
        "atlas_valid_frames_min": "atlas_valid_frame_count",
        "atlas_valid_objects_min": "atlas_valid_object_count",
        "unique_hashes_min": "unique_framebuffer_hashes",
        "separated_valid_frames_min": "separated_valid_frame_count",
        "separated_ready_frames_min": "separated_ready_frame_count",
        "unique_separated_hashes_min": "unique_separated_hashes",
        "lifted_objects_min": "lifted_object_count",
        "height_ramp_steps_min": "height_ramp_step_count",
        "shadow_casters_min": "shadow_caster_count",
        "shadow_lifted_casters_min": "shadow_caster_lifted_count",
    }
    for expected_field, summary_field in minima.items():
        if expected_field in expected and \
                summary[summary_field] < int(expected[expected_field]):
            errors.append(
                f"D1 {summary_field}: expected at least "
                f"{expected[expected_field]}, got {summary[summary_field]}")
    maxima = {
        "invalid_frames_max": "invalid_frame_count",
        "fallback_frames_max": "fallback_frame_count",
        "atlas_invalid_objects_max": "atlas_invalid_object_count",
        "accounting_errors_max": "accounting_error_count",
        "separated_mismatch_pixels_max": "separated_mismatch_pixels_max",
        "separated_mismatch_pixels_total_max":
            "separated_mismatch_pixels_total",
        "height_slew_violations_max": "height_slew_violation_count",
    }
    for expected_field, summary_field in maxima.items():
        if expected_field in expected and \
                summary[summary_field] > int(expected[expected_field]):
            errors.append(
                f"D1 {summary_field}: expected at most "
                f"{expected[expected_field]}, got {summary[summary_field]}")
    for field in ("max_virtual_height", "max_classified_height"):
        if field in expected and summary[field] != int(expected[field]):
            errors.append(
                f"D1 {field}: expected {expected[field]}, "
                f"got {summary[field]}")
    for field in ("shadow_softness_values", "light_values"):
        if field in expected and summary[field] != expected[field]:
            errors.append(
                f"D1 {field}: expected {expected[field]!r}, "
                f"got {summary[field]!r}")
    if "shadow_opacity_values" in expected and \
            summary["shadow_opacity_values"] != expected[
                "shadow_opacity_values"]:
        errors.append(
            f"D1 shadow_opacity_values: expected "
            f"{expected['shadow_opacity_values']!r}, "
            f"got {summary['shadow_opacity_values']!r}")
    census = summary.get("height_class_counts", {})
    for height_class, minimum in \
            expected.get("height_class_minimums", {}).items():
        if census.get(height_class, 0) < int(minimum):
            errors.append(
                f"D1 height class {height_class}: expected at least "
                f"{minimum} classified objects, got {census.get(height_class, 0)}")
    for height_class in expected.get("forbidden_height_classes", []):
        if census.get(height_class):
            errors.append(
                f"D1 height class {height_class}: expected none, "
                f"got {census[height_class]}")
    # The picker contract is asserted either way, but which contract applies is
    # a build-time property of the binary, not of the manifest.
    # A checkpoint that never opens a picker cannot exercise either picker
    # contract; asserting it there would only require every future replay to
    # include a picker for unrelated reasons. Default stays on so the existing
    # picker checkpoints keep their coverage.
    if not expected.get("picker_contract", True):
        return errors
    topdown = summary.get("picker_topdown_build")
    if topdown is None:
        errors.append("D1 picker_topdown_build: the trace disagrees with "
                      "itself about the compiled picker policy")
    elif not summary.get("picker_flag_frame_count"):
        errors.append("D1 picker_flag_frame_count: replay entered no picker, "
                      "so neither picker contract was exercised")
    elif topdown:
        if summary["picker_flag_enhanced_frame_count"]:
            errors.append(
                "D1 picker: AR_SIM3D_PICKER_TOPDOWN=1 build rendered "
                f"{summary['picker_flag_enhanced_frame_count']} enhanced "
                "frames while $7F:9215 was set")
    else:
        if summary["picker_flag_topdown_frame_count"]:
            errors.append(
                "D1 picker: AR_SIM3D_PICKER_TOPDOWN=0 build still forced "
                f"{summary['picker_flag_topdown_frame_count']} authentic "
                "picker frames")
        if not summary["picker_flag_enhanced_frame_count"]:
            errors.append(
                "D1 picker: AR_SIM3D_PICKER_TOPDOWN=0 build never kept the "
                "enhanced view through a picker")
        elif not summary["picker_flag_map_plane_frame_count"]:
            errors.append(
                "D1 picker: no projected picker frame painted its selector "
                "onto the map plane")

    exits = {int(frame["game_frame"]): frame
             for frame in summary.get("picker_exit_frames", [])}
    # Picker-exit expectations describe the top-down handoff; with the switch
    # compiled out there is no exit transition to make, and the enhanced-view
    # assertions above cover those frames instead.
    if topdown is False:
        return errors
    for game_frame in expected.get("ready_picker_exit_frames", []):
        actual = exits.get(int(game_frame))
        if not actual:
            errors.append(f"D1 picker exit gf={game_frame}: missing")
        elif not actual["separated_valid"] or \
                int(actual["separated_status"]) != SIM3D_CAPTURE_READY or \
                not (int(actual["effective"]) & 1):
            errors.append(
                f"D1 picker exit gf={game_frame}: expected an immediate "
                f"ready enhanced frame, got {actual!r}")
    for game_frame in expected.get("map_plane_picker_exit_frames", []):
        actual = exits.get(int(game_frame))
        if not actual:
            errors.append(f"D1 map-plane picker exit gf={game_frame}: missing")
        elif int(actual["map_plane_object_count"]) < 1:
            errors.append(
                f"D1 map-plane picker exit gf={game_frame}: selector object "
                f"was not classified onto the map plane")
    return errors


def compare_d1_framebuffer_hashes(enhanced_path: Path,
                                  authentic_path: Path) -> dict:
    compared = mismatches = 0
    first_mismatch = None
    with enhanced_path.open("r", encoding="utf-8") as enhanced, \
            authentic_path.open("r", encoding="utf-8") as authentic:
        for line_number, pair in enumerate(
                itertools.zip_longest(enhanced, authentic), 1):
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
            left_key = (int(left["host_frame"]), int(left["game_frame"]),
                        str(left["framebuffer_hash"]))
            right_key = (int(right["host_frame"]), int(right["game_frame"]),
                         str(right["framebuffer_hash"]))
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
        errors.append(
            f"D2 difference artifact has {nonzero_difference_bytes} nonzero byte(s)")
    if int(metadata.get("mismatch_pixels", -1)) != 0:
        errors.append(
            "D2 artifact metadata mismatch_pixels is not zero: "
            f"{metadata.get('mismatch_pixels')!r}")

    def png_chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    def write_png(path: Path, dimensions: list[int], pixels: bytes) -> None:
        width, height = dimensions
        scanlines = b"".join(
            b"\0" + pixels[y * width * 3:(y + 1) * width * 3]
            for y in range(height))
        data = (b"\x89PNG\r\n\x1a\n" +
                png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                                8, 2, 0, 0, 0)) +
                png_chunk(b"IDAT", zlib.compress(scanlines, 9)) +
                png_chunk(b"IEND", b""))
        path.write_bytes(data)

    png_paths = {
        "a_png": Path(f"{prefix}-A.png"),
        "b_png": Path(f"{prefix}-B.png"),
        "difference_png": Path(f"{prefix}-difference.png"),
    }
    write_png(png_paths["a_png"], dimensions_a, pixels_a)
    write_png(png_paths["b_png"], dimensions_b, pixels_b)
    write_png(png_paths["difference_png"], dimensions_difference,
              pixels_difference)
    paths.update(png_paths)
    return {
        "paths": {key: str(value) for key, value in paths.items()},
        "dimensions": dimensions_a,
        "game_frame": int(metadata.get("game_frame", 0)),
        "mismatch_pixels": int(metadata.get("mismatch_pixels", -1)),
        "nonzero_difference_bytes": nonzero_difference_bytes,
        "a_sha256": hashlib.sha256(paths["a"].read_bytes()).hexdigest(),
        "b_sha256": hashlib.sha256(paths["b"].read_bytes()).hexdigest(),
        "difference_sha256": hashlib.sha256(
            paths["difference"].read_bytes()).hexdigest(),
    }, errors


def validate_d3_artifact(output: Path, label: str, picker_topdown: bool,
                         max_differing_pixels: int | None = None
                         ) -> tuple[dict, list[str]]:
    """Retain and compare the two requested geometry-profile readbacks."""
    candidates = sorted(path for path in output.glob("runs/*/shot.ppm")
                        if path.parent.name != "latest")
    picker_candidates = sorted(
        path for path in output.glob("runs/*/shot_1000.ppm")
        if path.parent.name != "latest")
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
        if len(dimensions) != 2 or \
                len(pixels) != dimensions[0] * dimensions[1] * 3:
            raise ValueError(
                f"{label} screenshot dimensions/payload disagree: {path}")
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
    colors = {projected[index:index + 3]
              for index in range(0, len(projected), 3)}
    nonblack = sum(projected[index:index + 3] != b"\0\0\0"
                   for index in range(0, len(projected), 3))
    differing_pixels = sum(
        projected[index:index + 3] != authentic[index:index + 3]
        for index in range(0, min(len(projected), len(authentic)), 3))
    errors = []
    if authentic_dimensions != dimensions:
        errors.append(f"{label} B/A screenshot dimensions differ")
    if len(colors) < 16:
        errors.append(f"{label} screenshot has only {len(colors)} unique colors")
    if nonblack < dimensions[0] * dimensions[1] // 10:
        errors.append(f"{label} screenshot is predominantly black")
    minimum_difference = (dimensions[0] * dimensions[1] // 100
                          if label == "D3a" else 16)
    if differing_pixels < minimum_difference:
        errors.append(
            f"{label} B/A profiles changed only {differing_pixels} output pixels")
    # An upper bound is what distinguishes "this stage refined something" from
    # "this stage moved the scene". A blur that shifts geometry, or a shadow
    # pass that leaks past the ground, blows through it immediately.
    if max_differing_pixels is not None and \
            differing_pixels > max_differing_pixels:
        errors.append(
            f"{label} B/A profiles changed {differing_pixels} output pixels, "
            f"more than the {max_differing_pixels} this stage may touch")
    picker_differing_pixels = sum(
        picker_enhanced[index:index + 3] != picker_authentic[index:index + 3]
        for index in range(0, min(len(picker_enhanced),
                                  len(picker_authentic)), 3))
    if picker_dimensions != picker_auth_dimensions:
        errors.append(f"{label} enhanced/authentic picker dimensions differ")
    # With AR_SIM3D_PICKER_TOPDOWN=1 both profiles collapse to the same
    # authentic frame, so the picker screenshot must be pixel-identical. With
    # it compiled out the picker renders projected, and the two profiles differ
    # exactly where the stage under test differs -- which the B/A comparison
    # above already measures. Requiring equality there would assert the
    # opposite of the shipped behaviour.
    if picker_topdown and picker_differing_pixels:
        errors.append(
            f"{label} picker fallback differs by {picker_differing_pixels} pixels")
    if not picker_topdown and not picker_differing_pixels and \
            differing_pixels:
        errors.append(
            f"{label} projected picker frame is identical across profiles "
            "while ordinary frames differ; the picker may still be falling "
            "back to the authentic view")

    width, height = dimensions
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    def write_png(path: Path, pixels: bytes) -> None:
        scanlines = b"".join(
            b"\0" + pixels[y * width * 3:(y + 1) * width * 3]
            for y in range(height))
        path.write_bytes(
            b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                        8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(scanlines, 9)) +
            chunk(b"IEND", b""))

    projected_png = output / f"{label}-B.png"
    authentic_png = output / f"{label}-A.png"
    difference_png = output / f"{label}-difference.png"
    picker_png = output / f"{label}-picker-top-down.png"
    difference = bytes(abs(projected[i] - authentic[i])
                       for i in range(min(len(projected), len(authentic))))
    write_png(projected_png, projected)
    write_png(authentic_png, authentic)
    if len(difference) == len(projected):
        write_png(difference_png, difference)
    if picker_dimensions == dimensions:
        write_png(picker_png, picker_enhanced)
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


def command_for(checkpoint: dict, binary: Path, rom: Path,
                config: Path) -> list[str]:
    return [str(binary), str(rom), "--config", str(config)]


def check_stage_pinning(name: str, checkpoint: dict) -> None:
    """Every env block that pins any SIM 3D stage must pin them all.

    A block that pins only some stages silently inherits the shipped default
    for the rest, so the checkpoint's meaning changes the day a new stage
    lands -- which is exactly how six checkpoints broke at once when soft
    shadows shipped.
    """
    for block_name in ("env", "baseline_env"):
        block = checkpoint.get(block_name) or {}
        pinned = [key for key in SIM3D_STAGE_ENV if key in block]
        if pinned and len(pinned) != len(SIM3D_STAGE_ENV):
            missing = [key for key in SIM3D_STAGE_ENV if key not in block]
            raise ValueError(
                f"checkpoint {name} {block_name} pins "
                f"{len(pinned)} of {len(SIM3D_STAGE_ENV)} SIM 3D stages; "
                f"unpinned stages inherit the shipped default and will change "
                f"under this checkpoint when a stage lands. Missing: "
                f"{', '.join(missing)}")


def run_suite(args: argparse.Namespace, checkpoints: dict) -> int:
    for name, checkpoint in checkpoints.items():
        check_stage_pinning(name, checkpoint)

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    suite_root = args.artifact_root / f"{stamp}-suite"
    if not args.dry_run:
        suite_root.mkdir(parents=True, exist_ok=False)

    reports = []
    failed = []
    for name in checkpoints:
        command = [
            sys.executable, str(Path(__file__).resolve()),
            "--manifest", str(args.manifest),
            "--checkpoint", name,
            "--binary", str(args.binary),
            "--rom", str(args.rom),
            "--config", str(args.config),
            "--artifact-root", str(suite_root),
        ]
        if args.dry_run:
            command.append("--dry-run")
        result = subprocess.run(command, cwd=ROOT, check=False)
        if result.returncode != 0:
            failed.append(name)
            continue
        if args.dry_run:
            continue
        candidates = sorted(suite_root.glob(f"*-{name}/summary.json"))
        if not candidates:
            failed.append(name)
            continue
        reports.append(json.loads(candidates[-1].read_text(encoding="utf-8")))

    if args.dry_run:
        return 1 if failed else 0

    coverage = {
        "schema": "actraiser-sim3d-checkpoint-suite-v1",
        "checkpoint_count": len(checkpoints),
        "passed": [report["checkpoint"] for report in reports],
        "failed": failed,
        "towns": sorted({town for report in reports
                         for town in report["summary"]["towns"]}),
        "picker_operations": sorted({operation for report in reports
                                     for operation in report["summary"][
                                         "picker_operations"]}),
        "miracle_kinds": sorted({kind for report in reports
                                 for kind in report["summary"][
                                     "miracle_kinds"]}),
        "object_evidence": sorted({category for report in reports
                                   for category in report["summary"][
                                       "object_evidence"]}),
        "reports": [str(candidate) for report in reports
                    for candidate in sorted(
                        suite_root.glob(
                            f"*-{report['checkpoint']}/summary.json"))[-1:]],
    }
    coverage_path = suite_root / "coverage.json"
    coverage_path.write_text(json.dumps(coverage, indent=2) + "\n",
                             encoding="utf-8")
    print(json.dumps(coverage, indent=2))
    print(f"[suite] {'PASS' if not failed else 'FAIL'} -> {coverage_path}")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--checkpoint")
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build" / "ActRaiserRecomp")
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--config", type=Path, default=ROOT / "config.ini")
    parser.add_argument("--artifact-root", type=Path,
                        default=ROOT / "runs" / "sim3d-checkpoints")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    checkpoints = manifest["checkpoints"]
    if args.list:
        for name, item in checkpoints.items():
            print(f"{name:24} {item['description']}")
        return 0
    if args.all and args.checkpoint:
        parser.error("--all and --checkpoint are mutually exclusive")
    if args.all:
        return run_suite(args, checkpoints)
    if not args.checkpoint:
        parser.error("--checkpoint or --all is required unless --list is used")
    if args.checkpoint not in checkpoints:
        parser.error(f"unknown checkpoint {args.checkpoint!r}")

    checkpoint = checkpoints[args.checkpoint]
    check_stage_pinning(args.checkpoint, checkpoint)
    binary = args.binary.resolve()
    rom = args.rom.resolve()
    config = args.config.resolve()
    replay = resolve(ROOT, checkpoint["replay"]).resolve()
    sram_key = "sram_base64" if checkpoint.get("sram_base64") else "sram"
    sram = resolve(ROOT, checkpoint[sram_key]).resolve()
    settings = (resolve(ROOT, checkpoint["settings"]).resolve()
                if checkpoint.get("settings") else None)
    for label, path in (("binary", binary), ("ROM", rom),
                        ("config", config), ("replay", replay),
                        ("SRAM seed", sram)):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")
    if settings is not None and not settings.is_file():
        raise FileNotFoundError(f"settings fixture not found: {settings}")
    if sram_key == "sram_base64":
        try:
            encoded = "".join(sram.read_text(encoding="ascii").split())
            sram_bytes = base64.b64decode(encoded, validate=True)
        except (ValueError, UnicodeError) as error:
            raise ValueError(f"invalid base64 SRAM seed {sram}: {error}") from error
    else:
        sram_bytes = sram.read_bytes()
    if len(sram_bytes) != 8192:
        raise ValueError(
            f"SRAM seed must decode to 8192 bytes, got {len(sram_bytes)} from {sram}")
    sram_sha256 = hashlib.sha256(sram_bytes).hexdigest()
    expected_sram_sha256 = checkpoint.get("sram_sha256")
    if expected_sram_sha256 and sram_sha256 != expected_sram_sha256:
        raise ValueError(
            f"SRAM seed changed: expected {expected_sram_sha256}, "
            f"got {sram_sha256} from {sram}")

    for block_name in ("env", "baseline_env"):
        block = checkpoint.get(block_name) or {}
        pinned = [name for name in SIM3D_STAGE_ENV if name in block]
        if pinned and len(pinned) != len(SIM3D_STAGE_ENV):
            missing = [name for name in SIM3D_STAGE_ENV if name not in block]
            raise ValueError(
                f"checkpoint {args.checkpoint} {block_name} pins "
                f"{len(pinned)} of {len(SIM3D_STAGE_ENV)} SIM 3D stages; "
                f"unpinned stages inherit the shipped default and will change "
                f"under this checkpoint when a stage lands. Missing: "
                f"{', '.join(missing)}")

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = args.artifact_root / f"{stamp}-{args.checkpoint}"
    trace = output / "state.jsonl"
    function_trace = output / "picker-routines.jsonl"
    d1_trace = output / "d1-metadata.jsonl"
    d1_authentic_trace = output / "d1-authentic-metadata.jsonl"
    console = output / "console.log"
    d1_authentic_console = output / "d1-authentic-console.log"
    routine_console = output / "picker-routines-console.log"
    isolated_sram = output / "seed.srm"
    d1_authentic_sram = output / "authentic-seed.srm"
    summary_path = output / "summary.json"
    d3_visual = bool(checkpoint.get("d3a_visual") or
                     checkpoint.get("d3b_visual") or
                     checkpoint.get("d3c_visual") or
                     checkpoint.get("d4a_visual") or
                     checkpoint.get("d4b_visual") or
                     checkpoint.get("d4c_visual") or
                     checkpoint.get("d5a_visual"))
    d3_label = ("D5a" if checkpoint.get("d5a_visual")
                else "D4c" if checkpoint.get("d4c_visual")
                else "D4b" if checkpoint.get("d4b_visual")
                else "D4a" if checkpoint.get("d4a_visual")
                else "D3c" if checkpoint.get("d3c_visual")
                else "D3b" if checkpoint.get("d3b_visual") else "D3a")
    metadata_checkpoint = bool(
        checkpoint.get("d1_metadata") or checkpoint.get("d2_flat") or
        d3_visual)
    # A manifest that declares metadata expectations without enabling the
    # metadata pass would silently validate nothing and report PASS.
    if checkpoint.get("expect", {}).get("d1_metadata") and \
            not metadata_checkpoint:
        raise ValueError(
            f"checkpoint {args.checkpoint} expects d1_metadata but enables no "
            "metadata pass; set d1_metadata/d2_flat/d3*_visual/d5a_visual")
    d2_artifact_prefix = (output / "D2-flat").resolve()

    state_env = os.environ.copy()
    state_env.update({
        "AR_HEADLESS": "1",
        "AR_INPUT_REPLAY": str(replay),
        "AR_QUIT_FRAMES": str(int(checkpoint["quit_frames"])),
        "AR_SIM3D_TRACE": str(trace.resolve()),
        "AR_SAVE_NATIVE_PATH": str(isolated_sram.resolve()),
        # Disable the ambient anomaly ring without enabling detailed tracing in
        # the visual-state pass. Old input streams can take a different menu
        # transition path when the much heavier function trace is active.
        "AR_TRACE": str((output / "disabled-general-trace.jsonl").resolve()),
        "AR_TRACE_HF_LO": "999999",
        "AR_TRACE_HF_HI": "999999",
    })
    if settings is not None:
        state_env["AR_SETTINGS_PATH"] = str(settings)
    state_env.update({str(key): str(value)
                      for key, value in checkpoint.get("env", {}).items()})
    if metadata_checkpoint:
        state_env["AR_SIM3D_D1_TRACE"] = str(d1_trace.resolve())
    if checkpoint.get("d2_flat") or d3_visual or \
            checkpoint.get("headless_video"):
        state_env.update({
            "AR_HEADLESS_VIDEO": "1",
            "SDL_VIDEODRIVER": "dummy",
            "SDL_RENDER_DRIVER": "software",
        })
    if checkpoint.get("d2_flat"):
        state_env.update({
            "AR_SIM3D_D2_DUMP_PREFIX": str(d2_artifact_prefix),
            "AR_SIM3D_D2_DUMP_AT_GF": str(
                int(checkpoint.get("d2_dump_game_frame", 0))),
        })
    if d3_visual:
        state_env["AR_SHOT_AT_GF"] = str(
            int(checkpoint.get("d3a_shot_game_frame", 700)))
        state_env["AR_SHOT_EVERY"] = "1"
        state_env["AR_SHOT_FROM"] = "1000"
        state_env["AR_SHOT_TO"] = "1000"
        state_env["AR_SIM3D_D2_DUMP_PREFIX"] = str(
            (output / f"{d3_label}-planes").resolve())
        state_env["AR_SIM3D_D2_DUMP_AT_GF"] = str(
            int(checkpoint.get("d3a_shot_game_frame", 700)))
    state_env.pop("AR_TRACE_WATCH", None)

    routine_env = state_env.copy()
    routine_env.pop("AR_SIM3D_TRACE", None)
    routine_env.pop("AR_SIM3D_D1_TRACE", None)
    routine_env.pop("AR_SIMCAT", None)
    routine_env.pop("AR_SHOT_AT_GF", None)
    routine_env.pop("AR_SHOT_EVERY", None)
    routine_env.pop("AR_SHOT_FROM", None)
    routine_env.pop("AR_SHOT_TO", None)
    routine_env.update({
        # A separate evidence pass captures only bank-$01 function entries
        # beginning with 9. Filtering that stream to the three proven picker
        # routines distinguishes Direct the People / Building Direction from
        # targeted miracles,
        # even though both stage pending world type $0009.
        "AR_TRACE": str(function_trace.resolve()),
        "AR_TRACE_HF_LO": "0",
        "AR_TRACE_HF_HI": str(int(checkpoint["quit_frames"])),
        "AR_TRACE_CH": "func",
        "AR_TRACE_FUNC": "bank_01_9",
    })
    command = command_for(checkpoint, binary, rom, config)

    if args.dry_run:
        print(json.dumps({
            "checkpoint": args.checkpoint,
            "command": command,
            "output": str(output),
            "working_directory": str(output),
            "state_env": {key: state_env[key] for key in sorted(state_env)
                          if key.startswith("AR_")},
            "routine_env": {key: routine_env[key] for key in sorted(routine_env)
                            if key.startswith("AR_")},
        }, indent=2))
        return 0

    output.mkdir(parents=True, exist_ok=False)
    isolated_sram.write_bytes(sram_bytes)
    print(f"[{args.checkpoint}] running bounded replay -> {output}")
    result = subprocess.run(
        command, cwd=output, env=state_env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
    console.write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        raise RuntimeError(
            f"checkpoint process exited {result.returncode}; see {console}")
    if not trace.is_file():
        raise RuntimeError(f"checkpoint produced no state trace; see {console}")

    if metadata_checkpoint:
        d1_authentic_sram.write_bytes(sram_bytes)
        authentic_env = state_env.copy()
        authentic_env.pop("AR_SIM3D_TRACE", None)
        authentic_env.pop("AR_SIMCAT", None)
        authentic_env.update({
            "AR_SIM3D": "1" if d3_visual else "0",
            "AR_SIM3D_D1_TRACE": str(d1_authentic_trace.resolve()),
            "AR_SAVE_NATIVE_PATH": str(d1_authentic_sram.resolve()),
        })
        if d3_visual:
            # The visual gate compares the checkpoint's profile against the
            # stage below it, rendered by a second run of the same replay with
            # that stage switched off by name. There is no in-frame A/B view:
            # one frame renders one profile, and `baseline_env` is what makes
            # the comparison run differ.
            authentic_env.update(
                {str(key): str(value)
                 for key, value in checkpoint.get("baseline_env", {}).items()})
        authentic_result = subprocess.run(
            command, cwd=output, env=authentic_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
        d1_authentic_console.write_text(
            authentic_result.stdout, encoding="utf-8")
        if authentic_result.returncode != 0:
            print(authentic_result.stdout, file=sys.stderr)
            raise RuntimeError(
                "D1 authentic comparison pass exited "
                f"{authentic_result.returncode}; see {d1_authentic_console}")
        if not d1_authentic_trace.is_file():
            raise RuntimeError(
                "D1 authentic comparison produced no metadata trace; see "
                f"{d1_authentic_console}")

    expected_operations = checkpoint.get("expect", {}).get(
        "picker_operations", [])
    if expected_operations or checkpoint.get("trace_picker_routines"):
        routine_result = subprocess.run(
            command, cwd=output, env=routine_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
        routine_console.write_text(routine_result.stdout, encoding="utf-8")
        if routine_result.returncode != 0:
            print(routine_result.stdout, file=sys.stderr)
            raise RuntimeError(
                "picker routine pass exited "
                f"{routine_result.returncode}; see {routine_console}")

    frames = read_trace(trace)
    routine_calls = read_picker_routine_calls(function_trace)
    object_evidence = read_object_evidence(console)
    summary = summarize(frames, routine_calls, object_evidence)
    errors = validate(summary, checkpoint.get("expect", {}))
    d1_summary = None
    if metadata_checkpoint:
        if not d1_trace.is_file():
            raise RuntimeError(
                f"checkpoint produced no D1 metadata trace; see {console}")
        d1_summary, d1_errors = read_d1_metadata(d1_trace)
        errors.extend(d1_errors)
        errors.extend(validate_d1_summary(
            d1_summary, checkpoint.get("expect", {}).get("d1_metadata", {})))
        hash_compare = compare_d1_framebuffer_hashes(
            d1_trace, d1_authentic_trace)
        if hash_compare["mismatch_count"]:
            errors.append(
                "D1 authentic framebuffer comparison: "
                f"{hash_compare['mismatch_count']} mismatch(es); first="
                f"{hash_compare['first_mismatch']!r}")
        d1_summary["authentic_hash_compare"] = hash_compare
        summary["d1_metadata"] = d1_summary
    if checkpoint.get("d2_flat"):
        d2_artifacts, d2_errors = validate_d2_artifacts(d2_artifact_prefix)
        errors.extend(d2_errors)
        summary["d2_flat"] = d2_artifacts
    if d3_visual:
        picker_topdown = bool((d1_summary or {}).get("picker_topdown_build",
                                                     True))
        d3_artifact, d3_errors = validate_d3_artifact(output, d3_label,
                                                      picker_topdown,
                                                      checkpoint.get(
                                                          "max_differing_pixels"))
        errors.extend(d3_errors)
        summary[f"{d3_label.lower()}_visual"] = d3_artifact
    report = {
        "schema": "actraiser-sim3d-checkpoint-result-v1",
        "checkpoint": args.checkpoint,
        "description": checkpoint["description"],
        "command": command,
        "sram_seed": {
            "source_path": str(sram),
            "isolated_path": str(isolated_sram),
            "sha256": sram_sha256,
        },
        "settings_fixture": ({
            "path": str(settings),
            "sha256": hashlib.sha256(settings.read_bytes()).hexdigest(),
        } if settings is not None else None),
        "summary": summary,
        "validation_errors": errors,
    }
    summary_path.write_text(json.dumps(report, indent=2) + "\n",
                            encoding="utf-8")
    print(json.dumps(report, indent=2))
    if errors:
        print(f"[{args.checkpoint}] FAIL -> {summary_path}", file=sys.stderr)
        return 1
    print(f"[{args.checkpoint}] PASS -> {summary_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError,
            subprocess.TimeoutExpired) as error:
        print(f"sim3d_demo.py: {error}", file=sys.stderr)
        raise SystemExit(2)
