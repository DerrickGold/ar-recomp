#include "actraiser/actraiser_localization_compose_state.h"

#include <stdio.h>
#include <string.h>

#include "actraiser/actraiser_localization_name_entry.h"
#include "actraiser/actraiser_localization_style.h"
#include "localization/language_contract.h"
#include "actraiser/actraiser_localization_speed_text.h"

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
  state->message_speed_maximum = 9;
}

bool ActRaiserLocalizationComposeState_SetMessageSpeedMaximum(
    ActRaiserLocalizationComposeState *state, unsigned maximum) {
  if (!IsValid(state) || (maximum != 7 && maximum != 9)) return false;
  state->message_speed_maximum = (uint8_t)maximum;
  return true;
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

/* Which of the game's fixed cell menus a route draws, if any. Everything the
 * renderer needs about that menu's shape is derived from this in
 * actraiser_localization_grid.c; the identity itself never leaves the game. */
static ActRaiserLocalizationMenu MenuForSemanticId(const char *semantic_id) {
  /* The enum explicitly maps content shapes to this game's geometry. Route
   * classification is generated, not a second prefix-based registry. */
  return (ActRaiserLocalizationMenu)ArLanguageRowShape_ForRoute(semantic_id);
}

/* The name-entry keyboard reserves runs of ASCII blanks between its keys, so
 * the selector can be sized to the room the font actually leaves. Stating it
 * here keeps the renderer free of the convention. */
static const char *KeySeparatorForSemanticId(const char *semantic_id) {
  return !strncmp(semantic_id, "name_entry.prompt_and_", 22) ? " " : NULL;
}

static ArLocalizationTextLayoutKind LayoutForSemanticId(
    const char *semantic_id) {
  if (MenuForSemanticId(semantic_id) != kActRaiserLocalizationMenu_None)
    return kArLocalizationTextLayout_Grid;
  ArLanguagePresentationContract presentation;
  if (ArLanguageContract_Presentation(semantic_id, &presentation)) {
    if (!strcmp(presentation.layout, "centered_label"))
      return kArLocalizationTextLayout_CenteredLabel;
    if (!strcmp(presentation.layout, "single_line_label"))
      return kArLocalizationTextLayout_SingleLineLabel;
    if (!strcmp(presentation.layout, "centered_block"))
      return kArLocalizationTextLayout_CenteredBlock;
  }
  return kArLocalizationTextLayout_Flow;
}

static bool ResolveSnapshot(
    ActRaiserLocalizationComposeSnapshot *resolved,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (!resolved || !resolve_text || !resolved->semantic_id[0]) return false;
  memset(&resolved->text, 0, sizeof(resolved->text));
  if (!resolve_text(resolve_context, resolved->semantic_id, &resolved->text,
                    error, error_capacity) ||
      !ArLocalizationTextLanguage_IsValid(&resolved->text.language) ||
      resolved->text.utf8_bytes >= sizeof(resolved->text.utf8) ||
      resolved->text.utf8[resolved->text.utf8_bytes] != 0 ||
      !ArLocalizationTextField_IsValid(&resolved->text.live_field,
                                       resolved->text.utf8,
                                       resolved->text.utf8_bytes) ||
      !ArTextBidiSpans_FitSource(&resolved->text.bidi, resolved->text.utf8,
                                 resolved->text.utf8_bytes) ||
      (!resolved->text.utf8_bytes &&
       (resolved->text.cluster_count || resolved->text.inline_object_count)) ||
      (resolved->text.utf8_bytes && !resolved->text.cluster_count) ||
      !resolved->text.source_revision ||
      resolved->text.inline_object_count >
          kArLocalizationFrameInlineObjectCapacity) {
    if (error && error_capacity && !error[0])
      snprintf(error, error_capacity,
               "%s could not produce a fixed-text snapshot",
               resolved->semantic_id);
    return false;
  }
  if ((resolved->menu == kActRaiserLocalizationMenu_MessageSpeed ||
       resolved->menu == kActRaiserLocalizationMenu_MessageSpeedJP) &&
      !ActRaiserLocalizationSpeedText_Project(&resolved->text,
          resolved->menu == kActRaiserLocalizationMenu_MessageSpeedJP ? 7 : 9)) {
    if (error && error_capacity) snprintf(error, error_capacity, "incompatible message-speed numeric row");
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
  if (ActRaiserLocalizationRoute_InScope(state->map_group, state->map_number))
    InvalidateForDestination(state, observation->destination,
                             observation->serial);
  /* The selector stream erases the optional Professional row when moving
   * back to Continue/New Game. The arrow cells themselves remain native. */
  if (!state->map_group && !state->map_number &&
      observation->destination == 0x110c &&
      (observation->source_pc24 == 0x02aa34 ||
       observation->source_pc24 == 0x02aa4a))
    ClearSurface(state, 15);
  /* Sound test closes by composing spaces, not via the general text erase
   * routine. Release even a native-only/missing-translation generation, so a
   * later font/language change cannot resurrect the closed modal. */
  if (observation->source_pc24 == UINT32_C(0x029896) &&
      observation->caller_pc24 == UINT32_C(0x029860) &&
      observation->destination == UINT16_C(0x080B))
    ClearSurface(state, kActRaiserLocalizationSoundTestSurface);
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
      .menu = MenuForSemanticId(route->semantic_id),
  };
  if (resolved.menu == kActRaiserLocalizationMenu_MessageSpeed && state->message_speed_maximum == 7)
    resolved.menu = kActRaiserLocalizationMenu_MessageSpeedJP;
  if (resolved.menu != kActRaiserLocalizationMenu_None &&
      !ActRaiserLocalizationGrid_Build(resolved.menu, resolved.region,
                                       &resolved.grid))
    return false;
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
  if (slot->active && slot->text.source_revision == expected_source_revision)
    return true;
  ActRaiserLocalizationComposeSnapshot refreshed = *slot;
  if (!ResolveSnapshot(&refreshed, resolve_text, resolve_context, error,
                       error_capacity) ||
      refreshed.text.source_revision != expected_source_revision) {
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
  if (!ResolveSnapshot(&refreshed, resolve_text, resolve_context,
                       error, error_capacity)) {
    slot->active = false;
    return false;
  }
  /* Resolution may change annotations without changing the visible bytes.
   * The presenter owns content caching; publish the complete result here. */
  refreshed.active = true;
  *slot = refreshed;
  return true;
}

bool ActRaiserLocalizationComposeState_AppendFrame(
    const ActRaiserLocalizationComposeState *state, ArLocalizationFrame *frame,
    ArTextCellDestination destination, const ActRaiserTextPalette *palette) {
  if (!IsValid(state) || !frame || !palette) return false;
  const uint8_t first_snapshot = frame->snapshot_count;
  bool complete = true;
  for (size_t index = 0;
       index < kActRaiserLocalizationComposeSurfaceCapacity; ++index) {
    const ActRaiserLocalizationComposeSnapshot *slot =
        &state->surfaces[index];
    if (!slot->active) continue;
    if (!ArLocalizationTextLanguage_IsValid(&slot->text.language) ||
        !ArTextBidiSpans_FitSource(&slot->text.bidi, slot->text.utf8,
                                   slot->text.utf8_bytes) ||
        slot->text.bidi.count > kArTextMaximumBidiSpans - frame->bidi.count) {
      complete = false;
      continue;
    }
    if (!strcmp(slot->semantic_id, "title.save_choice.labels")) {
      /* Two choices, two fixed arrow slots. Ignore ROM padding/blank spacer
       * rows; an omitted choice still owns an empty replacement so native
       * lettering cannot leak through. Extra authored rows cannot escape. */
      size_t start = 0;
      for (unsigned choice = 0; choice < 2; ++choice) {
        size_t first = slot->text.utf8_bytes, end = first;
        while (start < slot->text.utf8_bytes) {
          end = start;
          while (end < slot->text.utf8_bytes && slot->text.utf8[end] != '\n')
            ++end;
          first = start;
          while (first < end && slot->text.utf8[first] == ' ')
            ++first;
          start = end < slot->text.utf8_bytes ? end + 1 : end;
          if (first < end) break;
        }
        ArTextCellRegion region = slot->region;
        region.row += choice * 2;
        region.rows = 1;
        const uint32_t clusters = first < end ? slot->text.cluster_count : 0;
        ArTextBidiSpans bidi = {0};
        for (uint16_t i = 0; i < slot->text.bidi.count; ++i) {
          ArTextBidiSpan s = slot->text.bidi.spans[i];
          if (s.end <= first || s.start >= end) continue;
          s.start = s.start > first ? s.start - (uint32_t)first : 0;
          s.end = (s.end < end ? s.end : (uint32_t)end) - (uint32_t)first;
          bidi.spans[bidi.count++] = s;
        }
        complete &=
            ArLocalizationFrame_AddTextWithObjectsAndLayout(
                frame, 140 + choice, destination, region,
                slot->text.utf8 + first, end - first, clusters, clusters,
                slot->text.source_revision, slot->text.language.direction,
                slot->native_font_pixels,
                kArLocalizationTextLayout_SingleLineLabel, NULL, 0, NULL, 0) &&
            ArLocalizationFrame_SetTextLanguage(frame, &slot->text.language) &&
            ArLocalizationFrame_SetTextBidiSpans(frame, &bidi) &&
            ActRaiserTextStyle_Publish(&slot->text.styles, (uint32_t)first,
                                       palette, frame);
      }
      continue;
    }
    if (slot->menu != kActRaiserLocalizationMenu_None) {
      /* Rows the grid reserves are the game's own artwork; claim them as
       * native preserves so the divider under a report's headings is stated
       * once, by the geometry, rather than repeated here as a row number. */
      ArTextCellRegion preserves[kArLocalizationFrameNativePreserveCapacity];
      uint8_t preserve_count = 0;
      for (uint8_t index = 0; index < slot->grid.rule_count; ++index) {
        const ArLocalizationTextRowRule *rule = &slot->grid.rules[index];
        if (!rule->native_reserved ||
            preserve_count >= kArLocalizationFrameNativePreserveCapacity)
          continue;
        const uint16_t rows =
            (uint16_t)(rule->last_line - rule->first_line + 1u);
        if (rule->first_line >= slot->region.rows) continue;
        preserves[preserve_count++] = (ArTextCellRegion){
            slot->region.column,
            (uint16_t)(slot->region.row + rule->first_line),
            slot->region.columns,
            (uint16_t)(rule->first_line + rows <= slot->region.rows
                           ? rows : slot->region.rows - rule->first_line)};
      }
      if (!ArLocalizationFrame_AddTextWithGrid(
              frame, slot->surface_id, destination, slot->region,
              slot->text.utf8, slot->text.utf8_bytes, slot->text.cluster_count,
              slot->text.cluster_count, slot->text.source_revision,
              slot->text.language.direction, slot->native_font_pixels,
              &slot->grid, slot->text.structural_boundaries,
              preserve_count ? preserves : NULL, preserve_count,
              slot->text.inline_objects, slot->text.inline_object_count) ||
          !ArLocalizationFrame_SetTextLanguage(frame, &slot->text.language) ||
          !ArLocalizationFrame_SetTextBidiSpans(frame, &slot->text.bidi) ||
          !ActRaiserTextStyle_Publish(&slot->text.styles, 0, palette, frame))
        complete = false;
      continue;
    }
    if (!ArLocalizationFrame_AddTextWithObjectsAndLayout(
            frame, slot->surface_id, destination, slot->region, slot->text.utf8,
            slot->text.utf8_bytes, slot->text.cluster_count,
            slot->text.cluster_count, slot->text.source_revision,
            slot->text.language.direction, slot->native_font_pixels,
            slot->layout, NULL, 0, slot->text.inline_objects,
            slot->text.inline_object_count) ||
        !ArLocalizationFrame_SetTextLanguage(frame, &slot->text.language) ||
        !ArLocalizationFrame_SetTextBidiSpans(frame, &slot->text.bidi) ||
        !ActRaiserTextStyle_Publish(&slot->text.styles, 0, palette, frame)) {
      complete = false;
      continue;
    }
    const char *separator = KeySeparatorForSemanticId(slot->semantic_id);
    if (!strncmp(slot->semantic_id, "city.", 5) &&
        slot->layout == kArLocalizationTextLayout_SingleLineLabel)
      frame->snapshots[frame->snapshot_count - 1u].top_inset_pixels = 1;
    if (separator &&
        !ArLocalizationFrame_SetKeySeparator(frame, separator,
                                             strlen(separator)))
      complete = false;
    /* The retail keyboard is a fixed cell grid: every key owns two tiles, one
     * for the selector and one for its glyph. State that shape so the renderer
     * can keep the columns aligned between rows, which a proportional font
     * cannot do on its own. */
    if (separator &&
        !ArLocalizationFrame_SetKeyGrid(
            frame, kActRaiserLocalizationNameEntryColumns,
            kActRaiserLocalizationNameEntryRows,
            kActRaiserLocalizationNameEntryKeyCellColumns))
      complete = false;
    /* The name keeps the native field's tiles while it is typed: letters sit
     * in fixed cells and never join, and typing rebuilds only the name rather
     * than the whole keyboard page. */
    if (slot->text.live_field.utf8_bytes &&
        !ArLocalizationFrame_SetLiveLine(
            frame, slot->text.live_field.utf8_offset,
            slot->text.live_field.utf8_bytes, slot->text.live_field.cells))
      complete = false;
  }
  for (uint8_t i = first_snapshot; i < frame->snapshot_count; ++i)
    ActRaiserLocalizationStyle_Ordinary(&frame->snapshots[i],
                                        palette->dialogue);
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
