"""Accumulate and report SIM metadata evidence, and compare checkpoint expectations."""

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


class MetadataStatistics:
    """Cross-frame counters and censuses; frame checks own the acceptance rules."""

    def __init__(self):
        self.frame_count = self.valid_count = self.invalid_count = self.inactive_count = 0
        self.picker_count = self.fallback_count = 0
        self.atlas_valid_count = self.atlas_invalid_count = self.atlas_valid_frames = 0
        self.separated_valid_count = self.separated_ready_count = 0
        self.separated_mismatch_total = self.separated_mismatch_max = 0
        self.max_atlas_used_width = self.max_atlas_used_height = 0
        self.max_sources = self.max_objects = self.max_emitted = self.max_synthetic_parts = 0
        self.max_world_record_occupancy = 0
        self.synthetic_overflow_total = self.synthetic_overflow_max = 0
        self.requested_features: set[int] = set()
        self.effective_features: set[int] = set()
        self.integrity_flags: set[int] = set()
        self.priorities: set[int] = set()
        self.hashes: set[str] = set()
        self.separated_hashes: set[str] = set()
        self.projection_cameras: set[tuple[int, int, int]] = set()
        self.separated_status_counts: dict[int, int] = {}
        self.height_class_counts: dict[str, int] = {}
        self.max_virtual_height = self.max_classified_height = self.lifted_object_count = 0
        self.height_ramp_steps = self.height_slew_violations = 0
        self.shadow_caster_count = self.shadow_caster_lifted_count = 0
        self.shadow_opacity_values: set[int] = set()
        self.shadow_softness_values: set[int] = set()
        self.light_values: set[tuple[int, int]] = set()
        self.picker_topdown_values: set[bool] = set()
        self.picker_flag_frames = 0
        self.picker_flag_topdown_frames = self.picker_flag_enhanced_frames = 0
        self.picker_flag_map_plane_frames = 0
        self.picker_exit_frames: list[dict] = []
        self.first_serial = self.last_serial = None
        self.effect_frame_count = self.visible_effect_frame_count = self.max_effects = 0
        self.visible_effect_instance_count = 0
        self.effect_metadata_invalid_frames = 0
        self.effect_overflow_total = self.effect_overflow_max = 0
        self.effect_kind_counts: dict[str, int] = {}
        self.effect_phase_counts: dict[str, dict[str, int]] = {}
        self.effect_color_counts: dict[str, dict[str, int]] = {}
        self.effect_compositions: dict[str, set[int]] = {}
        self.effect_generations: dict[str, set[int]] = {}
        self.ballistic_effect_instances = self.ballistic_object_instances = 0


class MetadataIssues:
    """Keep diagnostics bounded while counting every contract violation."""

    def __init__(self):
        self.count = 0
        self.errors = []

    def add(self, line_number: int, message: str) -> None:
        self.count += 1
        if len(self.errors) < 20:
            self.errors.append(f"D1 metadata line {line_number}: {message}")

    def messages(self) -> list[str]:
        extra = (
            [f"D1 metadata: {self.count - len(self.errors)} more error(s)"]
            if self.count > len(self.errors)
            else []
        )
        return self.errors + extra


def summarize_metadata(stats: MetadataStatistics, error_count: int) -> dict:
    return {
        "frame_count": stats.frame_count,
        "valid_frame_count": stats.valid_count,
        "invalid_frame_count": stats.invalid_count,
        "inactive_frame_count": stats.inactive_count,
        "picker_frame_count": stats.picker_count,
        "fallback_frame_count": stats.fallback_count,
        "first_build_serial": stats.first_serial,
        "last_build_serial": stats.last_serial,
        "requested": sorted(stats.requested_features),
        "effective": sorted(stats.effective_features),
        "integrity_flags": sorted(stats.integrity_flags),
        "priority_bands": sorted(stats.priorities),
        "max_source_count": stats.max_sources,
        "max_object_count": stats.max_objects,
        "max_emitted_oam_count": stats.max_emitted,
        "max_synthetic_part_count": stats.max_synthetic_parts,
        "max_world_record_occupancy": stats.max_world_record_occupancy,
        "synthetic_part_overflow_count_total": stats.synthetic_overflow_total,
        "synthetic_part_overflow_count_max": stats.synthetic_overflow_max,
        "effect_frame_count": stats.effect_frame_count,
        "visible_effect_frame_count": stats.visible_effect_frame_count,
        "visible_effect_instance_count": stats.visible_effect_instance_count,
        "max_effect_count": stats.max_effects,
        "effect_metadata_invalid_frame_count": stats.effect_metadata_invalid_frames,
        "effect_overflow_count_total": stats.effect_overflow_total,
        "effect_overflow_count_max": stats.effect_overflow_max,
        "effect_kind_counts": stats.effect_kind_counts,
        "effect_phase_counts": {
            kind: dict(sorted(counts.items()))
            for kind, counts in sorted(stats.effect_phase_counts.items())
        },
        "effect_color_counts": {
            kind: dict(sorted(counts.items()))
            for kind, counts in sorted(stats.effect_color_counts.items())
        },
        "effect_generation_counts": {
            kind: len(generations) for kind, generations in sorted(stats.effect_generations.items())
        },
        "effect_compositions": {
            kind: [f"${value:04X}" for value in sorted(values)]
            for kind, values in sorted(stats.effect_compositions.items())
        },
        "atlas_valid_frame_count": stats.atlas_valid_frames,
        "atlas_valid_object_count": stats.atlas_valid_count,
        "atlas_invalid_object_count": stats.atlas_invalid_count,
        "max_atlas_used_width": stats.max_atlas_used_width,
        "max_atlas_used_height": stats.max_atlas_used_height,
        "unique_framebuffer_hashes": len(stats.hashes),
        "separated_valid_frame_count": stats.separated_valid_count,
        "separated_ready_frame_count": stats.separated_ready_count,
        "separated_statuses": sorted(stats.separated_status_counts),
        "separated_status_counts": {
            SIM3D_CAPTURE_STATUS_NAMES.get(status, str(status)): count
            for status, count in sorted(stats.separated_status_counts.items())
        },
        "height_class_counts": dict(sorted(stats.height_class_counts.items())),
        "max_virtual_height": stats.max_virtual_height,
        "max_classified_height": stats.max_classified_height,
        "lifted_object_count": stats.lifted_object_count,
        "height_ramp_step_count": stats.height_ramp_steps,
        "height_slew_violation_count": stats.height_slew_violations,
        "ballistic_effect_instance_count": stats.ballistic_effect_instances,
        "ballistic_object_instance_count": stats.ballistic_object_instances,
        "shadow_caster_count": stats.shadow_caster_count,
        "shadow_caster_lifted_count": stats.shadow_caster_lifted_count,
        "shadow_opacity_values": sorted(stats.shadow_opacity_values),
        "shadow_softness_values": sorted(stats.shadow_softness_values),
        "light_values": [list(value) for value in sorted(stats.light_values)],
        "picker_topdown_build": (
            sorted(stats.picker_topdown_values)[0]
            if len(stats.picker_topdown_values) == 1
            else None
        ),
        "picker_flag_frame_count": stats.picker_flag_frames,
        "picker_flag_topdown_frame_count": stats.picker_flag_topdown_frames,
        "picker_flag_enhanced_frame_count": stats.picker_flag_enhanced_frames,
        "picker_flag_map_plane_frame_count": stats.picker_flag_map_plane_frames,
        "picker_exit_frames": stats.picker_exit_frames,
        "separated_mismatch_pixels_total": stats.separated_mismatch_total,
        "separated_mismatch_pixels_max": stats.separated_mismatch_max,
        "unique_separated_hashes": len(stats.separated_hashes),
        "projection_cameras": [list(camera) for camera in sorted(stats.projection_cameras)],
        "accounting_error_count": error_count,
    }


def validate_expectations(summary: dict, expected: dict) -> list[str]:
    errors: list[str] = []
    exact_fields = (
        "requested",
        "effective",
        "integrity_flags",
        "separated_statuses",
        "projection_cameras",
        "effect_frame_count",
        "visible_effect_frame_count",
        "visible_effect_instance_count",
        "max_effect_count",
        "effect_kind_counts",
        "effect_phase_counts",
        "effect_color_counts",
        "effect_generation_counts",
        "effect_compositions",
    )
    for field in exact_fields:
        if field in expected and summary[field] != expected[field]:
            errors.append(f"D1 {field}: expected {expected[field]!r}, got {summary[field]!r}")
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
        "effect_frames_min": "effect_frame_count",
        "max_effects_min": "max_effect_count",
    }
    for expected_field, summary_field in minima.items():
        if expected_field in expected and summary[summary_field] < int(expected[expected_field]):
            errors.append(
                f"D1 {summary_field}: expected at least "
                f"{expected[expected_field]}, got {summary[summary_field]}"
            )
    maxima = {
        "invalid_frames_max": "invalid_frame_count",
        "inactive_frames_max": "inactive_frame_count",
        "fallback_frames_max": "fallback_frame_count",
        "atlas_invalid_objects_max": "atlas_invalid_object_count",
        "accounting_errors_max": "accounting_error_count",
        "separated_mismatch_pixels_max": "separated_mismatch_pixels_max",
        "separated_mismatch_pixels_total_max": "separated_mismatch_pixels_total",
        "height_slew_violations_max": "height_slew_violation_count",
        "effect_metadata_invalid_frames_max": "effect_metadata_invalid_frame_count",
        "effect_overflow_total_max": "effect_overflow_count_total",
        "effect_overflow_max": "effect_overflow_count_max",
    }
    for expected_field, summary_field in maxima.items():
        if expected_field in expected and summary[summary_field] > int(expected[expected_field]):
            errors.append(
                f"D1 {summary_field}: expected at most "
                f"{expected[expected_field]}, got {summary[summary_field]}"
            )
    for field in ("max_virtual_height", "max_classified_height"):
        if field in expected and summary[field] != int(expected[field]):
            errors.append(f"D1 {field}: expected {expected[field]}, got {summary[field]}")
    for field in ("shadow_softness_values", "light_values"):
        if field in expected and summary[field] != expected[field]:
            errors.append(f"D1 {field}: expected {expected[field]!r}, got {summary[field]!r}")
    if (
        "shadow_opacity_values" in expected
        and summary["shadow_opacity_values"] != expected["shadow_opacity_values"]
    ):
        errors.append(
            f"D1 shadow_opacity_values: expected "
            f"{expected['shadow_opacity_values']!r}, "
            f"got {summary['shadow_opacity_values']!r}"
        )
    census = summary.get("height_class_counts", {})
    for height_class, minimum in expected.get("height_class_minimums", {}).items():
        if census.get(height_class, 0) < int(minimum):
            errors.append(
                f"D1 height class {height_class}: expected at least "
                f"{minimum} classified objects, got {census.get(height_class, 0)}"
            )
    for height_class in expected.get("forbidden_height_classes", []):
        if census.get(height_class):
            errors.append(
                f"D1 height class {height_class}: expected none, got {census[height_class]}"
            )
    for kind, minimum in expected.get("effect_kind_minimums", {}).items():
        actual = summary.get("effect_kind_counts", {}).get(kind, 0)
        if actual < int(minimum):
            errors.append(f"D1 effect kind {kind}: expected at least {minimum}, got {actual}")
    allowed_effect_kinds = set(expected.get("allowed_effect_kinds", []))
    if allowed_effect_kinds:
        unexpected = set(summary.get("effect_kind_counts", {})) - allowed_effect_kinds
        if unexpected:
            errors.append(f"D1 unexpected effect kinds: {sorted(unexpected)!r}")
    for kind, kind_expected in expected.get("effect_expectations", {}).items():
        actual_count = summary.get("effect_kind_counts", {}).get(kind, 0)
        if "instance_ticks" in kind_expected and actual_count != int(
            kind_expected["instance_ticks"]
        ):
            errors.append(
                f"D1 effect {kind} instance_ticks: expected "
                f"{kind_expected['instance_ticks']}, got {actual_count}"
            )
        actual_phases = summary.get("effect_phase_counts", {}).get(kind, {})
        if "phase_counts" in kind_expected and actual_phases != kind_expected["phase_counts"]:
            errors.append(
                f"D1 effect {kind} phase_counts: expected "
                f"{kind_expected['phase_counts']!r}, got {actual_phases!r}"
            )
        actual_colors = summary.get("effect_color_counts", {}).get(kind, {})
        if "color_counts" in kind_expected and actual_colors != kind_expected["color_counts"]:
            errors.append(
                f"D1 effect {kind} color_counts: expected "
                f"{kind_expected['color_counts']!r}, got {actual_colors!r}"
            )
        actual_generations = summary.get("effect_generation_counts", {}).get(kind, 0)
        if "generation_count" in kind_expected and actual_generations != int(
            kind_expected["generation_count"]
        ):
            errors.append(
                f"D1 effect {kind} generation_count: expected "
                f"{kind_expected['generation_count']}, "
                f"got {actual_generations}"
            )
        actual_compositions = summary.get("effect_compositions", {}).get(kind, [])
        if "compositions" in kind_expected and actual_compositions != kind_expected["compositions"]:
            errors.append(
                f"D1 effect {kind} compositions: expected "
                f"{kind_expected['compositions']!r}, "
                f"got {actual_compositions!r}"
            )
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
        errors.append(
            "D1 picker_topdown_build: the trace disagrees with "
            "itself about the compiled picker policy"
        )
    elif not summary.get("picker_flag_frame_count"):
        errors.append(
            "D1 picker_flag_frame_count: replay entered no picker, "
            "so neither picker contract was exercised"
        )
    elif topdown:
        if summary["picker_flag_enhanced_frame_count"]:
            errors.append(
                "D1 picker: AR_SIM3D_PICKER_TOPDOWN=1 build rendered "
                f"{summary['picker_flag_enhanced_frame_count']} enhanced "
                "frames while $7F:9215 was set"
            )
    else:
        if summary["picker_flag_topdown_frame_count"]:
            errors.append(
                "D1 picker: AR_SIM3D_PICKER_TOPDOWN=0 build still forced "
                f"{summary['picker_flag_topdown_frame_count']} authentic "
                "picker frames"
            )
        if not summary["picker_flag_enhanced_frame_count"]:
            errors.append(
                "D1 picker: AR_SIM3D_PICKER_TOPDOWN=0 build never kept the "
                "enhanced view through a picker"
            )
        elif not summary["picker_flag_map_plane_frame_count"]:
            errors.append(
                "D1 picker: no projected picker frame painted its selector onto the map plane"
            )

    exits = {int(frame["game_frame"]): frame for frame in summary.get("picker_exit_frames", [])}
    # Picker-exit expectations describe the top-down handoff; with the switch
    # compiled out there is no exit transition to make, and the enhanced-view
    # assertions above cover those frames instead.
    if topdown is False:
        return errors
    for game_frame in expected.get("ready_picker_exit_frames", []):
        actual = exits.get(int(game_frame))
        if not actual:
            errors.append(f"D1 picker exit gf={game_frame}: missing")
        elif (
            not actual["separated_valid"]
            or int(actual["separated_status"]) != SIM3D_CAPTURE_READY
            or not (int(actual["effective"]) & 1)
        ):
            errors.append(
                f"D1 picker exit gf={game_frame}: expected an immediate "
                f"ready enhanced frame, got {actual!r}"
            )
    for game_frame in expected.get("map_plane_picker_exit_frames", []):
        actual = exits.get(int(game_frame))
        if not actual:
            errors.append(f"D1 map-plane picker exit gf={game_frame}: missing")
        elif int(actual["map_plane_object_count"]) < 1:
            errors.append(
                f"D1 map-plane picker exit gf={game_frame}: selector object "
                f"was not classified onto the map plane"
            )
    return errors
