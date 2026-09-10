#include "actraiser/actraiser_localization_compose_state.h"

#include <stdio.h>
#include <string.h>

static bool IsValid(const ActRaiserLocalizationComposeState *state) {
  return state && state->struct_size >= sizeof(*state) &&
      state->abi_version ==
          ACTRAISER_LOCALIZATION_COMPOSE_STATE_ABI_VERSION;
}

static ActRaiserLocalizationComposeSnapshot *Slot(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id) {
  if (!IsValid(state) ||
      surface_id < kActRaiserLocalizationComposeSurfaceFirst ||
      surface_id > kActRaiserLocalizationComposeSurfaceLast)
    return NULL;
  return &state->surfaces[
      surface_id - kActRaiserLocalizationComposeSurfaceFirst];
}

static void ClearSurface(ActRaiserLocalizationComposeState *state,
                         uint32_t surface_id) {
  ActRaiserLocalizationComposeSnapshot *slot = Slot(state, surface_id);
  if (slot) memset(slot, 0, sizeof(*slot));
}

void ActRaiserLocalizationComposeState_Init(
    ActRaiserLocalizationComposeState *state) {
  if (!state) return;
  memset(state, 0, sizeof(*state));
  state->struct_size = sizeof(*state);
  state->abi_version = ACTRAISER_LOCALIZATION_COMPOSE_STATE_ABI_VERSION;
}

void ActRaiserLocalizationComposeState_Clear(
    ActRaiserLocalizationComposeState *state) {
  if (!IsValid(state)) return;
  memset(state->surfaces, 0, sizeof(state->surfaces));
  state->dialogue_replacement_serial = 0;
}

void ActRaiserLocalizationComposeState_SetScene(
    ActRaiserLocalizationComposeState *state,
    uint8_t map_group, uint8_t map_number) {
  if (!IsValid(state)) return;
  if (!state->scene_valid || state->map_group != map_group ||
      state->map_number != map_number) {
    ActRaiserLocalizationComposeState_Clear(state);
    state->map_group = map_group;
    state->map_number = map_number;
    state->scene_valid = true;
  }
}

bool ActRaiserLocalizationComposeState_ReleaseSurface(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id) {
  ActRaiserLocalizationComposeSnapshot *slot = Slot(state, surface_id);
  if (!slot) return false;
  memset(slot, 0, sizeof(*slot));
  return true;
}

static void MarkDialogueReplacement(
    ActRaiserLocalizationComposeState *state, uint64_t serial) {
  if (serial > state->dialogue_replacement_serial)
    state->dialogue_replacement_serial = serial;
}

/* These rules mirror native UI generations. In particular, destination
 * $0512 closes report/choice/selection generations even though those regions
 * do not all intersect the new menu heading geometrically. */
static void InvalidateForDestination(
    ActRaiserLocalizationComposeState *state, uint16_t destination,
    uint64_t serial) {
  switch (destination) {
    case UINT16_C(0x0512):
      ClearSurface(state, 2);
      ClearSurface(state, 3);
      ClearSurface(state, 6);
      ClearSurface(state, 7);
      ClearSurface(state, 8);
      ClearSurface(state, 9);
      MarkDialogueReplacement(state, serial);
      break;
    case UINT16_C(0x0A12):
      ClearSurface(state, 3);
      break;
    case UINT16_C(0x0106):
      ClearSurface(state, 4);
      break;
    case UINT16_C(0x0703):
      ActRaiserLocalizationComposeState_Clear(state);
      MarkDialogueReplacement(state, serial);
      break;
    case UINT16_C(0x060A):
    case UINT16_C(0x0603):
      ClearSurface(state, 3);
      ClearSurface(state, 6);
      ClearSurface(state, 7);
      ClearSurface(state, 8);
      ClearSurface(state, 9);
      MarkDialogueReplacement(state, serial);
      break;
    case UINT16_C(0x0B17):
      ClearSurface(state, 8);
      break;
    case UINT16_C(0x0C12):
      ClearSurface(state, 9);
      break;
    default:
      break;
  }
}

static void ClearIntersections(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id,
    ArTextCellRegion region) {
  for (uint32_t candidate = kActRaiserLocalizationComposeSurfaceFirst;
       candidate <= kActRaiserLocalizationComposeSurfaceLast; ++candidate) {
    ActRaiserLocalizationComposeSnapshot *slot = Slot(state, candidate);
    if (candidate != surface_id && slot && slot->observed &&
        ArTextCellRegionsIntersect(slot->region, region))
      memset(slot, 0, sizeof(*slot));
  }
}

static ArLocalizationTextLayoutKind LayoutForSemanticId(
    const char *semantic_id) {
  const size_t length = strlen(semantic_id);
  if (!strncmp(semantic_id, "city.", 5) && length > 10 &&
      !strcmp(semantic_id + length - 5, ".name"))
    return kArLocalizationTextLayout_SingleLineLabel;
  if (!strcmp(semantic_id, "status.report.cities_report"))
    return kArLocalizationTextLayout_StatusCities;
  if (!strcmp(semantic_id, "status.report.score_report"))
    return kArLocalizationTextLayout_StatusScore;
  if (!strcmp(semantic_id, "status.report.master_report"))
    return kArLocalizationTextLayout_StatusMaster;
  if (!strcmp(semantic_id, "system.message_speed.scale_labels"))
    return kArLocalizationTextLayout_MessageSpeed;
  if (!strcmp(semantic_id, "system.choice.yes_no") ||
      !strncmp(semantic_id, "sky.menu.", 9) ||
      !strncmp(semantic_id, "sim.menu.", 9))
    return kArLocalizationTextLayout_FixedRows;
  return kArLocalizationTextLayout_Flow;
}

static bool ResolveSnapshot(
    ActRaiserLocalizationComposeSnapshot *resolved,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (!resolved || !resolve_text || !resolved->semantic_id[0]) return false;
  if (!resolve_text(resolve_context, resolved->semantic_id,
                    resolved->utf8, sizeof(resolved->utf8),
                    &resolved->utf8_bytes, &resolved->cluster_count,
                    &resolved->source_revision, resolved->inline_objects,
                    kArLocalizationFrameInlineObjectCapacity,
                    &resolved->inline_object_count, error, error_capacity) ||
      resolved->utf8_bytes >= sizeof(resolved->utf8) ||
      resolved->utf8[resolved->utf8_bytes] != 0 ||
      (!resolved->utf8_bytes &&
       (resolved->cluster_count || resolved->inline_object_count)) ||
      (resolved->utf8_bytes && !resolved->cluster_count) ||
      !resolved->source_revision ||
      resolved->inline_object_count >
          kArLocalizationFrameInlineObjectCapacity) {
    if (error && error_capacity && !error[0])
      snprintf(error, error_capacity,
               "%s could not produce a fixed-text snapshot",
               resolved->semantic_id);
    return false;
  }
  return true;
}

bool ActRaiserLocalizationComposeState_Process(
    ActRaiserLocalizationComposeState *state,
    const ActRaiserLocalizationComposeObservation *observation,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  if (!IsValid(state) || !observation || !state->scene_valid ||
      observation->struct_size < sizeof(*observation) ||
      observation->abi_version !=
          ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION ||
      observation->map_group != state->map_group ||
      observation->map_number != state->map_number)
    return false;

  /* Invalidation is driven by the native destination even when the source is
   * unrouted or the selected pack cannot provide a replacement. */
  if (observation->clear_row_count) {
    if (!observation->clear_column_count ||
        observation->clear_first_column + observation->clear_column_count > 32 ||
        observation->clear_first_row + observation->clear_row_count > 32)
      return false;
    ClearIntersections(state, 0, (ArTextCellRegion){
        observation->clear_first_column, observation->clear_first_row,
        observation->clear_column_count, observation->clear_row_count});
    if (observation->clears_dialogue)
      MarkDialogueReplacement(state, observation->serial);
    return true;
  }
  InvalidateForDestination(state, observation->destination,
                           observation->serial);
  const ActRaiserLocalizationComposeRoute *route =
      ActRaiserLocalizationRoute_ResolveCompose(observation);
  if (!route || !resolve_text) return true;
  ActRaiserLocalizationComposeSnapshot resolved = {
      .observed = true,
      .surface_id = route->surface_id,
      .generation_serial = observation->serial,
      .region = route->region,
      .native_destination = route->destination,
      .native_font_pixels = route->native_font_pixels,
      .layout = LayoutForSemanticId(route->semantic_id),
  };
  const size_t semantic_bytes = strlen(route->semantic_id);
  if (!semantic_bytes || semantic_bytes >= sizeof(resolved.semantic_id)) {
    if (error && error_capacity)
      snprintf(error, error_capacity, "fixed-text semantic ID is too long");
    return false;
  }
  memcpy(resolved.semantic_id, route->semantic_id, semantic_bytes + 1u);
  ActRaiserLocalizationComposeSnapshot *slot =
      Slot(state, route->surface_id);
  if (!slot) return false;
  ClearIntersections(state, route->surface_id, route->region);
  resolved.active = ResolveSnapshot(&resolved, resolve_text, resolve_context,
                                    error, error_capacity);
  *slot = resolved;
  return resolved.active;
}

bool ActRaiserLocalizationComposeState_Refresh(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id,
    uint64_t expected_source_revision,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  ActRaiserLocalizationComposeSnapshot *slot = Slot(state, surface_id);
  if (!slot || !slot->observed || !expected_source_revision || !resolve_text)
    return false;
  if (slot->active && slot->source_revision == expected_source_revision)
    return true;
  ActRaiserLocalizationComposeSnapshot refreshed = *slot;
  refreshed.utf8[0] = 0;
  refreshed.utf8_bytes = 0;
  refreshed.cluster_count = 0;
  refreshed.source_revision = 0;
  refreshed.inline_object_count = 0;
  if (!ResolveSnapshot(&refreshed, resolve_text, resolve_context,
                       error, error_capacity) ||
      refreshed.source_revision != expected_source_revision) {
    slot->active = false;
    if (error && error_capacity && !error[0])
      snprintf(error, error_capacity,
               "%s changed while its fixed text was refreshed",
               refreshed.semantic_id);
    return false;
  }
  refreshed.active = true;
  *slot = refreshed;
  return true;
}

bool ActRaiserLocalizationComposeState_RefreshLatest(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  ActRaiserLocalizationComposeSnapshot *slot = Slot(state, surface_id);
  if (!slot || !slot->observed || !resolve_text) return false;
  ActRaiserLocalizationComposeSnapshot refreshed = *slot;
  refreshed.utf8[0] = 0;
  refreshed.utf8_bytes = 0;
  refreshed.cluster_count = 0;
  refreshed.source_revision = 0;
  refreshed.inline_object_count = 0;
  if (!ResolveSnapshot(&refreshed, resolve_text, resolve_context,
                       error, error_capacity)) {
    slot->active = false;
    return false;
  }
  if (slot->active && refreshed.source_revision == slot->source_revision &&
      refreshed.utf8_bytes == slot->utf8_bytes &&
      refreshed.inline_object_count == slot->inline_object_count &&
      !memcmp(refreshed.utf8, slot->utf8, refreshed.utf8_bytes + 1u) &&
      !memcmp(refreshed.inline_objects, slot->inline_objects,
              (size_t)refreshed.inline_object_count *
                  sizeof(refreshed.inline_objects[0])))
    return true;
  refreshed.active = true;
  *slot = refreshed;
  return true;
}

bool ActRaiserLocalizationComposeState_AppendFrame(
    const ActRaiserLocalizationComposeState *state,
    ArLocalizationFrame *frame, ArTextCellDestination destination,
    ArTextDirection direction) {
  if (!IsValid(state) || !frame) return false;
  bool complete = true;
  for (size_t index = 0;
       index < kActRaiserLocalizationComposeSurfaceCapacity; ++index) {
    const ActRaiserLocalizationComposeSnapshot *slot =
        &state->surfaces[index];
    if (!slot->active) continue;
    const bool report_rule =
        slot->layout == kArLocalizationTextLayout_StatusCities ||
        slot->layout == kArLocalizationTextLayout_StatusScore;
    const ArTextCellRegion divider = {
        slot->region.column, (uint16_t)(slot->region.row + 5),
        slot->region.columns, 1};
    if (!ArLocalizationFrame_AddTextWithObjectsAndLayout(
            frame, slot->surface_id, destination, slot->region,
            slot->utf8, slot->utf8_bytes,
            slot->cluster_count, slot->cluster_count,
            slot->source_revision, direction, slot->native_font_pixels,
            slot->layout,
            report_rule ? &divider : NULL, report_rule ? 1 : 0,
            slot->inline_objects, slot->inline_object_count))
      complete = false;
  }
  return complete;
}

bool ActRaiserLocalizationComposeState_DialogueWasReplaced(
    const ActRaiserLocalizationComposeState *state,
    uint64_t terminal_compose_serial) {
  return IsValid(state) && state->dialogue_replacement_serial >
      terminal_compose_serial;
}

size_t ActRaiserLocalizationComposeState_ActiveCount(
    const ActRaiserLocalizationComposeState *state) {
  if (!IsValid(state)) return 0;
  size_t count = 0;
  for (size_t index = 0;
       index < kActRaiserLocalizationComposeSurfaceCapacity; ++index)
    count += state->surfaces[index].active ? 1u : 0u;
  return count;
}

const ActRaiserLocalizationComposeSnapshot *
ActRaiserLocalizationComposeState_Find(
    const ActRaiserLocalizationComposeState *state, uint32_t surface_id) {
  const ActRaiserLocalizationComposeSnapshot *slot =
      ActRaiserLocalizationComposeState_FindObserved(state, surface_id);
  return slot && slot->active ? slot : NULL;
}

const ActRaiserLocalizationComposeSnapshot *
ActRaiserLocalizationComposeState_FindObserved(
    const ActRaiserLocalizationComposeState *state, uint32_t surface_id) {
  if (!IsValid(state) ||
      surface_id < kActRaiserLocalizationComposeSurfaceFirst ||
      surface_id > kActRaiserLocalizationComposeSurfaceLast)
    return NULL;
  const ActRaiserLocalizationComposeSnapshot *slot =
      &state->surfaces[
          surface_id - kActRaiserLocalizationComposeSurfaceFirst];
  return slot->observed ? slot : NULL;
}
