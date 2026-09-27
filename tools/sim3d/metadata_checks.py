"""Validate capture, atlas, OAM, effect lifecycle, height and shadow contracts."""

from .metadata_report import (
    MetadataIssues,
    MetadataStatistics,
    SIM3D_CAPTURE_READY,
    summarize_metadata,
)

# Only these D3c presentation classes may lift a billboard off the ground.
LIFTED_HEIGHT_CLASSES = ("flying", "flying_projectile", "semi_grounded")
# ROM-positioned contact classes must land exactly, never ease into place.
CONTACT_EXACT_HEIGHT_CLASSES = ("ground_effect", "ground_strike")
MAP_PLANE_TRAIT = 1 << 0
NO_SHADOW_TRAIT = 1 << 2
SIM_EFFECT_VISIBLE = 1 << 0
SIM_EFFECT_VOLCANO_FIREBALL = 7
# kSimHeightSlewStep in sim_render_metadata.h.
HEIGHT_SLEW_STEP = 4


def is_inactive_capture(frame: dict) -> bool:
    """A pre-town/navigation frame has no SIM capture to invalidate.

    Require the complete inactive contract so a broken active frame or one
    carrying partial actors/effects cannot disappear from failure counts.
    """
    return (frame.get("view") == "none" and frame.get("separated_status") == 0
            and frame.get("master_enabled") is False
            and not frame.get("metadata_valid") and not frame.get("effective")
            and not frame.get("sources") and not frame.get("objects")
            and not frame.get("effects") and not frame.get("integrity_flags"))


def is_volcano_fireball_source(source: dict) -> bool:
    """Audited packed identity and airborne art, not arbitrary projectiles.

    This is the explicit exception in CaptureEffectInstances/ApplyHeightSlew:
    eruption presentation supplies a ballistic position instead of easing
    toward the classifier's constant 24-pixel flight plane.
    """
    return (
        source.get("tier") == 1
        and source.get("type") == 0x0E01
        and source.get("composition") in (0xE7D0, 0xE7A6)
    )


def is_ballistic_effect(effect: dict, source: dict) -> bool:
    return (
        is_volcano_fireball_source(source)
        and effect.get("kind") == SIM_EFFECT_VOLCANO_FIREBALL
        and effect.get("kind_name") == "volcano_fireball"
        and effect.get("record") == source.get("record")
        and effect.get("composition") == source.get("composition")
    )


def effect_anchor_valid(effect: dict, source: dict) -> bool:
    world = effect.get("world", [])
    if (
        not isinstance(world, list)
        or len(world) != 2
        or any(type(v) is not int or not 0 <= v <= 0xFFFF for v in world)
    ):
        return False
    return is_ballistic_effect(effect, source) or world == [source.get("x"), source.get("y")]


def is_ballistic_object(obj: dict, sources: list[dict]) -> bool:
    index = obj.get("source_index", -1)
    if type(index) is not int or not 0 <= index < len(sources):
        return False
    source = sources[index]
    return (
        is_volcano_fireball_source(source)
        and obj.get("tier") == 1
        and obj.get("record") == source.get("record")
        and obj.get("composition") == source.get("composition")
    )


class MetadataValidator:
    """One streaming validation session; temporal state never leaks between runs."""

    def __init__(self, allowed_unpacked_atlas_objects: list[dict] | None = None):
        self.stats = MetadataStatistics()
        self.issues = MetadataIssues()
        self.height_carry: dict[int, int] = {}
        self.previous_height_view = None
        self.previous_height_serial = None
        self.previous_view = None
        self.effect_carry: dict[tuple[str, int, int], dict] = {}
        self.allowed_unpacked = {
            (int(item["game_frame"]), int(item["record"]), int(item["composition"]))
            for item in (allowed_unpacked_atlas_objects or [])
        }
        self.observed_unpacked: set[tuple[int, int, int]] = set()

    def observe(self, frame: dict, line_number: int) -> None:
        self._check_capture(frame, line_number)
        self._check_picker(frame, line_number)
        self._check_counts(frame, line_number)
        self._check_atlas(frame, line_number)
        self._check_declared_counts(frame, line_number)
        self._check_effects(frame, line_number)
        self._check_oam(frame, line_number)
        self._check_height_and_shadows(frame, line_number)
        self._check_world_suffix(frame, line_number)

    def finish(self) -> tuple[dict, list[str]]:
        for descriptor in sorted(self.allowed_unpacked - self.observed_unpacked):
            self.issues.add(0, f"allowed unpacked atlas object was not observed: {descriptor!r}")
        return summarize_metadata(self.stats, self.issues.count), self.issues.messages()

    def _check_capture(self, frame: dict, line_number: int) -> None:
        valid = bool(frame.get("metadata_valid"))
        serial = int(frame.get("build_serial", 0))
        separated_status = int(frame.get("separated_status", 0))
        separated_valid = bool(frame.get("separated_valid"))
        flags = int(frame.get("integrity_flags", 0))
        self.stats.frame_count += 1
        self.stats.valid_count += valid
        self.stats.inactive_count += is_inactive_capture(frame)
        self.stats.invalid_count += not valid and not is_inactive_capture(frame)
        self.stats.picker_count += frame.get("view") == "authentic_picker"
        self.stats.fallback_count += frame.get("view") == "authentic_fallback"
        if self.stats.first_serial is None:
            self.stats.first_serial = serial
        self.stats.last_serial = serial

        self.stats.requested_features.add(int(frame.get("requested", 0)))
        self.stats.effective_features.add(int(frame.get("effective", 0)))
        effective_separated = int(frame.get("effective", 0)) & 1
        self.stats.integrity_flags.add(flags)
        self.stats.hashes.add(str(frame.get("framebuffer_hash", "")))
        separated_mismatch = int(frame.get("separated_mismatch_pixels", 0))
        self.stats.separated_valid_count += separated_valid
        self.stats.separated_ready_count += separated_status == SIM3D_CAPTURE_READY
        self.stats.separated_mismatch_total += separated_mismatch
        self.stats.separated_mismatch_max = max(
            self.stats.separated_mismatch_max, separated_mismatch
        )
        self.stats.separated_status_counts[separated_status] = (
            self.stats.separated_status_counts.get(separated_status, 0) + 1
        )
        separated_hash = str(frame.get("separated_hash", ""))
        camera = frame.get("projection_camera", [0, 0, 0])
        if len(camera) == 3:
            self.stats.projection_cameras.add(tuple(int(value) for value in camera))
        if separated_valid:
            self.stats.separated_hashes.add(separated_hash)
            if separated_status != SIM3D_CAPTURE_READY:
                self.issues.add(line_number, "separated_valid frame does not report ready")
            if separated_mismatch:
                self.issues.add(line_number, "separated_valid frame has pixel mismatches")
        if separated_status == SIM3D_CAPTURE_READY and not separated_valid:
            self.issues.add(line_number, "ready capture is not separated_valid")
        if effective_separated and not separated_valid:
            self.issues.add(line_number, "separated feature is effective without a valid capture")

    def _check_picker(self, frame: dict, line_number: int) -> None:
        view = str(frame.get("view", ""))
        objects = frame.get("objects", [])
        separated_status = int(frame.get("separated_status", 0))
        separated_valid = bool(frame.get("separated_valid"))
        # The binary reports its compiled picker policy so the checkpoint
        # asserts the contract that actually shipped, instead of silently
        # passing because the frames it was written for no longer exist.
        self.stats.picker_topdown_values.add(bool(frame.get("picker_topdown", True)))

        self.stats.shadow_opacity_values.add(int(frame.get("shadow_opacity_pct", 0)))
        self.stats.shadow_softness_values.add(int(frame.get("shadow_softness_pct", 0)))
        light = frame.get("light", [0, 0])
        self.stats.light_values.add((int(light[0]), int(light[1])))

        if int(frame.get("picker_flag", 0)):
            self.stats.picker_flag_frames += 1
            if view == "authentic_picker":
                self.stats.picker_flag_topdown_frames += 1
            elif view == "enhanced":
                self.stats.picker_flag_enhanced_frames += 1
                # The selector must still be painted onto the map square
                # rather than billboarded while the picker is projected.
                if any((int(obj.get("traits", 0)) & MAP_PLANE_TRAIT) for obj in objects):
                    self.stats.picker_flag_map_plane_frames += 1
        if self.previous_view == "authentic_picker" and view != "authentic_picker":
            self.stats.picker_exit_frames.append(
                {
                    "game_frame": int(frame.get("game_frame", 0)),
                    "view": view,
                    "separated_status": separated_status,
                    "separated_valid": separated_valid,
                    "effective": int(frame.get("effective", 0)),
                    "map_plane_object_count": sum(
                        (int(obj.get("traits", 0)) & 1) != 0 for obj in objects
                    ),
                }
            )
        self.previous_view = view

    def _check_counts(self, frame: dict, line_number: int) -> None:
        sources = frame.get("sources", [])
        objects = frame.get("objects", [])
        effects = frame.get("effects", [])
        emitted = int(frame.get("emitted_oam_count", 0))
        synthetic_parts = int(frame.get("synthetic_part_count", 0))
        synthetic_overflow = int(frame.get("synthetic_part_overflow_count", 0))
        self.stats.max_sources = max(self.stats.max_sources, len(sources))
        self.stats.max_objects = max(self.stats.max_objects, len(objects))
        self.stats.max_emitted = max(self.stats.max_emitted, emitted)
        self.stats.max_synthetic_parts = max(self.stats.max_synthetic_parts, synthetic_parts)
        self.stats.max_world_record_occupancy = max(
            self.stats.max_world_record_occupancy, int(frame.get("world_record_occupancy", 0))
        )
        self.stats.synthetic_overflow_total += synthetic_overflow
        self.stats.synthetic_overflow_max = max(
            self.stats.synthetic_overflow_max, synthetic_overflow
        )
        self.stats.max_effects = max(self.stats.max_effects, len(effects))
        self.stats.effect_frame_count += bool(effects)
        frame_visible_effects = sum(
            (int(effect.get("flags", 0)) & SIM_EFFECT_VISIBLE) != 0 for effect in effects
        )
        self.stats.visible_effect_frame_count += frame_visible_effects > 0
        self.stats.visible_effect_instance_count += frame_visible_effects
        if int(frame.get("effect_visible_count", -1)) != frame_visible_effects:
            self.issues.add(line_number, "effect_visible_count does not match visible flags")
        effect_metadata_valid = bool(frame.get("effect_metadata_valid", False))
        self.stats.effect_metadata_invalid_frames += (
            not effect_metadata_valid and not is_inactive_capture(frame))
        effect_overflow = int(frame.get("effect_overflow_count", -1))
        if effect_overflow < 0:
            self.issues.add(line_number, "effect_overflow_count is missing")
            effect_overflow = 0
        self.stats.effect_overflow_total += effect_overflow
        self.stats.effect_overflow_max = max(self.stats.effect_overflow_max, effect_overflow)
        if effect_overflow and effect_metadata_valid:
            self.issues.add(line_number, "effect overflow did not invalidate effect metadata")

    def _check_atlas(self, frame: dict, line_number: int) -> None:
        valid = bool(frame.get("metadata_valid"))
        objects = frame.get("objects", [])
        frame_atlas_valid = bool(frame.get("atlas_valid"))
        self.stats.atlas_valid_frames += frame_atlas_valid
        object_atlas_valid = sum(bool(obj.get("atlas_valid")) for obj in objects)
        self.stats.atlas_valid_count += object_atlas_valid
        self.stats.atlas_invalid_count += len(objects) - object_atlas_valid
        self.stats.priorities.update(int(obj["priority"]) for obj in objects)

        atlas_size = frame.get("atlas_size", [0, 0])
        atlas_used = frame.get("atlas_used", [0, 0])
        if len(atlas_size) != 2 or len(atlas_used) != 2:
            self.issues.add(line_number, "atlas size/used descriptor is malformed")
            atlas_width = atlas_height = used_width = used_height = 0
        else:
            atlas_width, atlas_height = map(int, atlas_size)
            used_width, used_height = map(int, atlas_used)
        self.stats.max_atlas_used_width = max(self.stats.max_atlas_used_width, used_width)
        self.stats.max_atlas_used_height = max(self.stats.max_atlas_used_height, used_height)
        if valid and not frame_atlas_valid:
            self.issues.add(line_number, "valid metadata frame has no valid atlas")
        if frame_atlas_valid and (
            atlas_width <= 0
            or atlas_height <= 0
            or used_width < 0
            or used_height < 0
            or used_width > atlas_width
            or used_height > atlas_height
        ):
            self.issues.add(line_number, "atlas frame bounds are invalid")
        atlas_rects: list[tuple[int, int, int, int]] = []
        for object_index, obj in enumerate(objects):
            if not bool(obj.get("atlas_valid")):
                if frame_atlas_valid:
                    descriptor = (
                        int(frame.get("game_frame", -1)),
                        int(obj.get("record", -1)),
                        int(obj.get("composition", -1)),
                    )
                    if descriptor in self.allowed_unpacked:
                        self.observed_unpacked.add(descriptor)
                    else:
                        self.issues.add(
                            line_number,
                            f"object {object_index} lacks an atlas rect: {descriptor!r}",
                        )
                continue
            rect = [int(value) for value in obj.get("atlas", [])]
            local = [int(value) for value in obj.get("local_bounds", [])]
            if len(rect) != 4 or len(local) != 4:
                self.issues.add(line_number, f"object {object_index} atlas descriptor malformed")
                continue
            x, y, width, height = rect
            if (
                width <= 0
                or height <= 0
                or x < 0
                or y < 0
                or x + width > used_width
                or y + height > used_height
            ):
                self.issues.add(line_number, f"object {object_index} atlas rect out of used bounds")
            if local[2] - local[0] != width or local[3] - local[1] != height:
                self.issues.add(line_number, f"object {object_index} local/atlas size mismatch")
            for prior_index, (px, py, pw, ph) in enumerate(atlas_rects):
                if x < px + pw and x + width > px and y < py + ph and y + height > py:
                    self.issues.add(
                        line_number, f"objects {prior_index}/{object_index} overlap in atlas"
                    )
            atlas_rects.append((x, y, width, height))

    def _check_declared_counts(self, frame: dict, line_number: int) -> None:
        sources = frame.get("sources", [])
        objects = frame.get("objects", [])
        effects = frame.get("effects", [])
        if int(frame.get("source_count", -1)) != len(sources):
            self.issues.add(line_number, "source_count does not match sources[]")
        if int(frame.get("object_count", -1)) != len(objects):
            self.issues.add(line_number, "object_count does not match objects[]")
        if int(frame.get("effect_count", -1)) != len(effects):
            self.issues.add(line_number, "effect_count does not match effects[]")

    def _check_effects(self, frame: dict, line_number: int) -> None:
        serial = int(frame.get("build_serial", 0))
        sources = frame.get("sources", [])
        effects = frame.get("effects", [])
        for effect_index, effect in enumerate(effects):
            kind = str(effect.get("kind_name", "missing"))
            phase = str(effect.get("phase_name", "missing"))
            self.stats.effect_kind_counts[kind] = self.stats.effect_kind_counts.get(kind, 0) + 1
            phase_counts = self.stats.effect_phase_counts.setdefault(kind, {})
            phase_counts[phase] = phase_counts.get(phase, 0) + 1
            color = str(effect.get("color_name", "missing"))
            color_counts = self.stats.effect_color_counts.setdefault(kind, {})
            color_counts[color] = color_counts.get(color, 0) + 1
            self.stats.effect_compositions.setdefault(kind, set()).add(
                int(effect.get("composition", 0))
            )
            generation = int(effect.get("generation", 0))
            self.stats.effect_generations.setdefault(kind, set()).add(generation)
            source_index = int(effect.get("source_index", -1))
            if source_index < 0 or source_index >= len(sources):
                self.issues.add(line_number, f"effect {effect_index} source index is out of bounds")
                continue
            source = sources[source_index]
            geometry = effect.get("geometry", {})
            if int(effect.get("record", -1)) != int(source["record"]) or int(
                effect.get("composition", -1)
            ) != int(source["composition"]):
                self.issues.add(line_number, f"effect {effect_index} does not match its source")
            self.stats.ballistic_effect_instances += is_ballistic_effect(effect, source)
            if not effect_anchor_valid(effect, source):
                self.issues.add(line_number, f"effect {effect_index} world anchor drifted")
            if generation <= 0:
                self.issues.add(line_number, f"effect {effect_index} has invalid generation")
            geometry_kind = str(geometry.get("kind_name", "missing"))
            geometry_space = str(geometry.get("space_name", "missing"))
            if geometry_kind not in {"point", "segment", "area", "scene"}:
                self.issues.add(
                    line_number, f"effect {effect_index} has unknown geometry {geometry_kind!r}"
                )
            if geometry_space not in {"record_local", "world_local", "screen"}:
                self.issues.add(
                    line_number, f"effect {effect_index} has unknown space {geometry_space!r}"
                )
            if geometry_kind == "point" and len(geometry.get("point", [])) != 3:
                self.issues.add(line_number, f"effect {effect_index} has malformed point geometry")
            if phase in {"missing", "unknown", "none"}:
                self.issues.add(line_number, f"effect {effect_index} has invalid phase {phase!r}")

            key = (kind, int(effect.get("record", -1)), generation)
            current = {
                "serial": serial,
                "age": int(effect.get("age_ticks", -1)),
                "phase": phase,
                "phase_ticks": int(effect.get("phase_ticks", -1)),
                "pulse_generation": int(effect.get("pulse_generation", -1)),
                "pulse_ticks": int(effect.get("pulse_ticks", -1)),
            }
            previous = self.effect_carry.get(key)
            if (
                current["age"] < 0
                or current["phase_ticks"] < 0
                or current["pulse_generation"] < 0
                or current["pulse_ticks"] < 0
            ):
                self.issues.add(line_number, f"effect {effect_index} has malformed lifecycle ages")
            if previous and serial == previous["serial"] + 1:
                if current["age"] != min(previous["age"] + 1, 0xFFFF):
                    self.issues.add(line_number, f"effect {effect_index} lifecycle age skipped")
                expected_phase_ticks = (
                    min(previous["phase_ticks"] + 1, 0xFFFF) if phase == previous["phase"] else 0
                )
                if current["phase_ticks"] != expected_phase_ticks:
                    self.issues.add(line_number, f"effect {effect_index} phase age skipped")
            self.effect_carry[key] = current

    def _check_oam(self, frame: dict, line_number: int) -> None:
        sources = frame.get("sources", [])
        objects = frame.get("objects", [])
        emitted = int(frame.get("emitted_oam_count", 0))
        claimed = int(frame.get("claimed_oam_count", 0))
        if emitted != claimed:
            self.issues.add(line_number, f"emitted {emitted} != claimed {claimed}")
        if sum(int(source["oam_count"]) for source in sources) != emitted:
            self.issues.add(line_number, "source OAM counts do not sum to emitted")
        if sum(int(obj["oam_count"]) for obj in objects) != emitted:
            self.issues.add(line_number, "object OAM counts do not sum to emitted")

        cursor = 0
        for source_index, source in enumerate(sources):
            first = int(source["oam_first"])
            count = int(source["oam_count"])
            palette_mask = int(source.get("obj_palette_mask", -1))
            fragment_first = int(source["fragment_first"])
            fragment_count = int(source["fragment_count"])
            if palette_mask < 0 or palette_mask > 0xFF or (count and not palette_mask):
                self.issues.add(
                    line_number,
                    f"source {source_index} has invalid OBJ palette mask {palette_mask}",
                )
            if first != cursor:
                self.issues.add(
                    line_number, f"source {source_index} starts {first}, expected {cursor}"
                )
            cursor += count
            if (
                fragment_first < 0
                or fragment_count < 0
                or fragment_first + fragment_count > len(objects)
            ):
                self.issues.add(
                    line_number, f"source {source_index} fragment range is out of bounds"
                )
                continue
            fragments = objects[fragment_first : fragment_first + fragment_count]
            if sum(int(obj["oam_count"]) for obj in fragments) != count:
                self.issues.add(line_number, f"source {source_index} fragment counts do not match")
            fragment_cursor = first
            for obj in fragments:
                if int(obj["source_index"]) != source_index or int(obj["record"]) != int(
                    source["record"]
                ):
                    self.issues.add(line_number, f"source {source_index} owns a foreign fragment")
                if int(obj["oam_first"]) != fragment_cursor:
                    self.issues.add(line_number, f"source {source_index} fragment has an OAM gap")
                fragment_cursor += int(obj["oam_count"])
        if cursor != emitted:
            self.issues.add(line_number, f"final source cursor {cursor} != {emitted}")

    def _check_height_and_shadows(self, frame: dict, line_number: int) -> None:
        view = str(frame.get("view", ""))
        serial = int(frame.get("build_serial", 0))
        sources = frame.get("sources", [])
        objects = frame.get("objects", [])
        frame_source_records = {int(source.get("record", 0)) for source in sources}
        # D3c: the classification is a data table, so the replay only has
        # to prove that every published descriptor obeys its own policy.
        # Easing is only continuous between consecutive enhanced builds;
        # a picker, a fallback, or a build gap legitimately snaps.
        ramp_comparable = (
            view == "enhanced"
            and self.previous_height_view == "enhanced"
            and self.previous_height_serial is not None
            and serial == self.previous_height_serial + 1
        )
        previous_heights = self.height_carry if ramp_comparable else {}
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
            self.stats.height_class_counts[height_class] = (
                self.stats.height_class_counts.get(height_class, 0) + 1
            )
            self.stats.max_virtual_height = max(self.stats.max_virtual_height, height)
            self.stats.max_classified_height = max(self.stats.max_classified_height, classified)
            if height:
                self.stats.lifted_object_count += 1
            if height < 0 or classified < 0:
                self.issues.add(line_number, f"object {object_index} has a negative height")
            if classified and height_class not in LIFTED_HEIGHT_CLASSES:
                self.issues.add(
                    line_number,
                    f"object {object_index} classifies {classified} px as {height_class}",
                )
            if classified and (traits & MAP_PLANE_TRAIT):
                self.issues.add(line_number, f"object {object_index} is lifted and map-planed")
            # A ROM-positioned contact class must land exactly, never ease.
            if height_class in CONTACT_EXACT_HEIGHT_CLASSES and height:
                self.issues.add(
                    line_number,
                    f"object {object_index} eases a contact-exact {height_class} to {height} px",
                )
            # The published height may only move toward the classified
            # plane, and only by the documented step.
            previous = previous_heights.get(record)
            contact_exact = height_class in CONTACT_EXACT_HEIGHT_CLASSES
            ballistic = is_ballistic_object(obj, sources)
            self.stats.ballistic_object_instances += ballistic
            if previous is not None and height != previous and not ballistic:
                step = abs(height - previous)
                # Entering a contact class is a deliberate snap: the strike
                # must be on the ground for its very first frame.
                if step > HEIGHT_SLEW_STEP and not contact_exact:
                    self.stats.height_slew_violations += 1
                    self.issues.add(
                        line_number,
                        f"object {object_index} jumped {step} px ({previous} -> {height})",
                    )
                elif (height - previous) * (classified - previous) < 0:
                    self.issues.add(
                        line_number,
                        f"object {object_index} eased away from its "
                        f"classified {classified} px plane",
                    )
                else:
                    self.stats.height_ramp_steps += 1
            frame_heights[record] = height
            if int(obj["tier"]) == 0 and (
                classified or height or height_class not in ("none", "map_plane")
            ):
                self.issues.add(
                    line_number,
                    f"fixed object {object_index} entered the height "
                    f"system as {height_class}/{classified}",
                )

            # D4a: recompute caster selection from the trait data rather
            # than trusting the flag, so a classification regression shows
            # up here instead of as a stray silhouette on the ground.
            casts = bool(obj.get("casts_shadow", False))
            expected_cast = (
                int(obj["tier"]) == 1
                and bool(obj.get("atlas_valid", False))
                and not (traits & (MAP_PLANE_TRAIT | NO_SHADOW_TRAIT))
            )
            if casts and not expected_cast:
                self.issues.add(
                    line_number,
                    f"object {object_index} casts a shadow despite "
                    f"tier/traits {int(obj['tier'])}/{traits}",
                )
            if casts:
                self.stats.shadow_caster_count += 1
                if height:
                    self.stats.shadow_caster_lifted_count += 1
                # A silhouette is drawn from the live object list only, so
                # a caster must always still own a record this frame.
                if record not in frame_source_records:
                    self.issues.add(
                        line_number,
                        f"object {object_index} casts a shadow for "
                        f"record ${record:04X}, which this frame's "
                        f"source list no longer contains",
                    )

        self.height_carry = frame_heights
        self.previous_height_view = view
        self.previous_height_serial = serial

    def _check_world_suffix(self, frame: dict, line_number: int) -> None:
        objects = frame.get("objects", [])
        world_objects = [obj for obj in objects if int(obj["tier"]) == 1]
        world_count = int(frame.get("world_oam_count", 0))
        if sum(int(obj["oam_count"]) for obj in world_objects) != world_count:
            self.issues.add(line_number, "world fragments do not equal world suffix count")
        if world_objects:
            world_first = int(frame["world_oam_first"])
            if int(world_objects[0]["oam_first"]) != world_first:
                self.issues.add(line_number, "world suffix begins at the wrong slot")
