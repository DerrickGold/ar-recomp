"""SIM checkpoint trace: evidence and validation independent of command dispatch."""

from __future__ import annotations
import json
from pathlib import Path
import re

PICKER_ROUTINES = {
    "0193DC": "type_0b_position_picker",
    "01972F": "direct_people",
    "019754": "targeted_miracle",
}


def picker_ranges(frames: list[dict]) -> list[list[int]]:
    values = [int(frame["game_frame"]) for frame in frames if frame.get("picker_active")]
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
        pending_types = sorted({int(frame.get("pending_world_type", 0)) for frame in group})
        if pending_types == [0x000B]:
            type_evidence = "type_0b_position_picker"
        elif pending_types == [0x0009]:
            type_evidence = "type_09_picker_ambiguous"
        else:
            type_evidence = "unknown_or_mixed"
        first = group[0]
        last = group[-1]
        transitions.append(
            {
                "start_game_frame": int(first["game_frame"]),
                "end_game_frame": int(last["game_frame"]),
                "duration_frames": len(group),
                "pending_world_types": pending_types,
                "type_evidence": type_evidence,
                "aimed_cell_start": [int(first["aimed_cell_x"]), int(first["aimed_cell_y"])],
                "aimed_cell_end": [int(last["aimed_cell_x"]), int(last["aimed_cell_y"])],
                "sim_target_start": [int(first["sim_target_x"]), int(first["sim_target_y"])],
                "sim_target_end": [int(last["sim_target_x"]), int(last["sim_target_y"])],
            }
        )
    return transitions


def summarize(frames: list[dict], routine_calls: list[dict], object_evidence: dict) -> dict:
    color_states = {
        (frame["ppu"]["windowsel"], frame["ppu"]["cgwsel"], frame["ppu"]["cgadsub"])
        for frame in frames
        if frame.get("ppu") is not None
    }
    visible_town_frames = [
        frame
        for frame in frames
        if frame.get("town")
        and frame.get("ppu") is not None
        and int(frame["ppu"].get("inidisp", 0x80)) != 0x80
    ]
    return {
        "frame_count": len(frames),
        "towns": sorted({int(frame["map"]) for frame in frames if frame.get("town")}),
        "views": sorted({str(frame["expected_view"]) for frame in frames}),
        "picker_frame_count": sum(bool(frame.get("picker_active")) for frame in frames),
        "picker_ranges": picker_ranges(frames),
        "picker_transitions": picker_transitions(frames),
        "picker_pending_world_types": sorted(
            {
                int(frame.get("pending_world_type", 0))
                for frame in frames
                if frame.get("picker_active")
            }
        ),
        "picker_routine_calls": routine_calls,
        "picker_operations": [call["operation"] for call in routine_calls],
        "miracle_kinds": sorted(
            {
                int(frame.get("miracle_kind", 0))
                for frame in frames
                if int(frame.get("miracle_kind", 0)) != 0
            }
        ),
        "object_evidence": object_evidence,
        "ppu_modes": sorted(
            {int(frame["ppu"]["bgmode"]) for frame in frames if frame.get("ppu") is not None}
        ),
        "color_state_count": len(color_states),
        "first_game_frame": int(frames[0]["game_frame"]) if frames else None,
        "last_game_frame": int(frames[-1]["game_frame"]) if frames else None,
        "visible_town_camera_x_min": min(
            (int(frame["camera_x"]) for frame in visible_town_frames), default=None
        ),
        "visible_town_camera_x_max": max(
            (int(frame["camera_x"]) for frame in visible_town_frames), default=None
        ),
    }


def validate(summary: dict, expected: dict) -> list[str]:
    errors: list[str] = []
    for field in ("towns", "views", "ppu_modes"):
        if field in expected and summary[field] != expected[field]:
            errors.append(f"{field}: expected {expected[field]!r}, got {summary[field]!r}")
    for field in (
        "picker_ranges",
        "picker_pending_world_types",
        "picker_operations",
        "miracle_kinds",
    ):
        if field in expected and summary[field] != expected[field]:
            errors.append(f"{field}: expected {expected[field]!r}, got {summary[field]!r}")
    for field in ("visible_town_camera_x_min", "visible_town_camera_x_max"):
        if field in expected and summary[field] != expected[field]:
            errors.append(f"{field}: expected {expected[field]!r}, got {summary[field]!r}")
    minimum = int(expected.get("picker_frames_min", 0))
    if summary["picker_frame_count"] < minimum:
        errors.append(
            f"picker_frame_count: expected at least {minimum}, got {summary['picker_frame_count']}"
        )
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
            calls.append(
                {
                    "operation": operation,
                    "routine": f"${pc[:2]}:{pc[2:]}",
                    "host_frame": int(event["hf"]),
                    "game_frame": int(event["gf"]),
                }
            )
    return calls


def read_object_evidence(path: Path) -> dict:
    categories: dict[str, list[dict]] = {}
    if not path.is_file():
        return categories
    for line in path.read_text(errors="replace").splitlines():
        if "[simcat]" not in line:
            continue
        fields = dict(re.findall(r"([a-zA-Z0-9_]+)=([^ ]+)", line))
        try:
            frame = int(fields["frame"], 16)
            tier = fields["tier"]
            identity = int(fields["type" if tier == "W" else "list"], 16)
            event = {
                "game_frame": int(fields["gf"], 10),
                "composition": frame,
                "record": int(fields["rec"], 16),
                "type": identity,
            }
        except (KeyError, ValueError):
            continue
        if tier == "F" and frame in (0xE9CC, 0xEA27, 0xEA82, 0xEAEC):
            category = "town_creation_lightning"
        elif tier != "W":
            continue
        elif frame in (0xE1BD, 0xE209, 0xE255):
            category = "blue_dragon_lightning"
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
            "compositions": [
                f"${value:04X}" for value in sorted({event["composition"] for event in events})
            ],
            "record_types": [
                f"${value:02X}" for value in sorted({event["type"] for event in events})
            ],
        }
    return evidence
