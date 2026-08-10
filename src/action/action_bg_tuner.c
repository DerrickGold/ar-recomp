#include "action_bg_tuner.h"

#include <stdio.h>
#include <string.h>

enum { kExtentStep = 4, kAuthenticWidth = 256, kAuthenticHeight = 224 };

typedef struct ActionBgBandDraft {
  bool used;
  uint16_t y0, y1;
  bool horizontal_set;
  ActionBgHorizontalExtent horizontal;
} ActionBgBandDraft;

typedef struct ActionBgLayerDraft {
  bool edge_set;
  ActionBgEdgeMode edge;
  /* Debug A/B over the normal extent policy. Kept separate from the authored
   * caps so switching it off restores the exact layer and band values the
   * tuner was holding instead of destroying that work. */
  bool ignore_side_bounds;
  bool horizontal_set;
  ActionBgHorizontalExtent horizontal;
  bool vertical_set;
  ActionBgVerticalExtent vertical;
  ActionBgBandDraft bands[kActionBgMaxBands];
} ActionBgLayerDraft;

typedef struct ActionBgTunerState {
  bool live;
  bool room_initialized;
  bool draft_enabled;
  bool guides_enabled;
  uint8_t map_group, map_number;
  int selected_layer;
  ActionBgPlan canonical;
  ActionBgTunerLimits limits;
  ActionBgLayerDraft layer[kActionBgPlanLayerCount];
} ActionBgTunerState;

static ActionBgTunerState s_tuner = { .selected_layer = -1 };

static void ClearDraft(void) {
  memset(s_tuner.layer, 0, sizeof(s_tuner.layer));
  s_tuner.draft_enabled = false;
}

void ActionBgTuner_ResetSession(void) {
  s_tuner = (ActionBgTunerState){ .selected_layer = -1 };
}

void ActionBgTuner_BeginFrame(void) {
  s_tuner.live = false;
}

static bool CanonicalHasBand(const ActionBgLayerPlan *layer,
                             uint16_t y0, uint16_t y1) {
  if (!layer) return false;
  for (unsigned i = 0; i < layer->band_count; i++)
    if (layer->bands[i].y0 == y0 && layer->bands[i].y1 == y1) return true;
  return false;
}

bool ActionBgTuner_ObservePlan(uint8_t map_group, uint8_t map_number,
                              const ActionBgPlan *canonical,
                              ActionBgTunerLimits limits) {
  if (!map_group || !map_number || !ActionBgPlan_Validate(canonical))
    return false;
  const bool changed_room = !s_tuner.room_initialized ||
      map_group != s_tuner.map_group || map_number != s_tuner.map_number;
  if (changed_room) {
    ClearDraft();
    s_tuner.selected_layer = -1;
  } else {
    /* Bands are keyed by their canonical row interval, not by array position.
     * Drop overrides whose interval disappeared so a dynamic room cannot
     * exhaust the fixed sparse table with stale keys. */
    for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
      for (unsigned i = 0; i < kActionBgMaxBands; i++) {
        ActionBgBandDraft *draft = &s_tuner.layer[layer].bands[i];
        if (draft->used && !CanonicalHasBand(
                &canonical->layer[layer], draft->y0, draft->y1))
          *draft = (ActionBgBandDraft){0};
      }
    }
  }
  s_tuner.map_group = map_group;
  s_tuner.map_number = map_number;
  s_tuner.canonical = *canonical;
  s_tuner.limits = limits;
  s_tuner.live = true;
  s_tuner.room_initialized = true;
  return true;
}

static ActionBgBandDraft *FindBandDraft(int layer, uint16_t y0, uint16_t y1,
                                        bool create) {
  if (layer < 0 || layer >= kActionBgPlanLayerCount) return NULL;
  ActionBgBandDraft *free_slot = NULL;
  for (unsigned i = 0; i < kActionBgMaxBands; i++) {
    ActionBgBandDraft *draft = &s_tuner.layer[layer].bands[i];
    if (draft->used && draft->y0 == y0 && draft->y1 == y1) return draft;
    if (!draft->used && !free_slot) free_slot = draft;
  }
  if (!create || !free_slot) return NULL;
  free_slot->used = true;
  free_slot->y0 = y0;
  free_slot->y1 = y1;
  return free_slot;
}

static bool BuildEffectivePlan(ActionBgPlan *out, bool apply_draft) {
  if (!out || !s_tuner.live || !ActionBgPlan_Validate(&s_tuner.canonical))
    return false;
  ActionBgPlan built = s_tuner.canonical;
  if (apply_draft) {
    for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
      ActionBgLayerPlan *dst = &built.layer[layer];
      const ActionBgLayerDraft *draft = &s_tuner.layer[layer];
      if (draft->edge_set) dst->default_edge = draft->edge;
      if (draft->horizontal_set)
        dst->horizontal_extent = draft->horizontal;
      if (draft->vertical_set) dst->vertical_extent = draft->vertical;
      for (unsigned band = 0; band < dst->band_count; band++) {
        ActionBgBandDraft *band_draft = FindBandDraft(
            (int)layer, dst->bands[band].y0, dst->bands[band].y1, false);
        if (band_draft && band_draft->horizontal_set)
          dst->bands[band].horizontal_extent = band_draft->horizontal;
      }
      /* Resolve the convenience toggle last. A layer can contain fixed row
       * bands even when its default extent is available (Bloodpool/Death Heim),
       * so ignoring the side boundary means removing every horizontal cap on
      * that plane, not just changing the layer default. Source/edge rules still
      * decide whether pixels actually exist outside the authentic viewport. */
      if (draft->ignore_side_bounds) {
        const ActionBgHorizontalExtent available = {
          .mode = kActionBgExtent_Available,
        };
        dst->horizontal_extent = available;
        for (unsigned band = 0; band < dst->band_count; band++)
          dst->bands[band].horizontal_extent = available;
      }
    }
  }
  if (!ActionBgPlan_Validate(&built)) return false;
  *out = built;
  return true;
}

bool ActionBgTuner_ApplyDraft(ActionBgPlan *plan) {
  if (!plan || !s_tuner.live || !ActionBgPlan_Validate(plan)) return false;
  if (!s_tuner.draft_enabled) return true;
  ActionBgPlan built;
  if (!BuildEffectivePlan(&built, true)) return false;
  /* Role/source/world metadata must belong to the canonical plan the caller
   * just observed. A stale caller cannot replace a different room's plan. */
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    if (plan->layer[layer].role != s_tuner.canonical.layer[layer].role ||
        plan->layer[layer].source != s_tuner.canonical.layer[layer].source ||
        plan->layer[layer].world_width !=
            s_tuner.canonical.layer[layer].world_width ||
        plan->layer[layer].world_height !=
            s_tuner.canonical.layer[layer].world_height)
      return false;
  }
  *plan = built;
  return true;
}

bool ActionBgTuner_IsLive(void) { return s_tuner.live; }
bool ActionBgTuner_DraftEnabled(void) { return s_tuner.draft_enabled; }
bool ActionBgTuner_GuidesEnabled(void) {
  return s_tuner.live && s_tuner.guides_enabled;
}

static ActionBgTunerRow *PushRow(ActionBgTunerRow *out, int capacity,
                                 int *count, ActionBgTunerRowKind kind,
                                 int layer, int band) {
  if (!out || !count || *count >= capacity) return NULL;
  ActionBgTunerRow *row = &out[(*count)++];
  memset(row, 0, sizeof(*row));
  row->kind = kind;
  row->layer = (int8_t)layer;
  row->band = (int8_t)band;
  row->selectable = kind != kActionBgTunerRow_Header &&
      kind != kActionBgTunerRow_BandHeader;
  return row;
}

static const char *UpperEdge(ActionBgEdgeMode edge) {
  static const char *const names[] = {
    "TRANSPARENT", "LIVE WORLD", "CLAMP", "MIRROR", "REPEAT", "RAW WRAP",
  };
  return edge >= kActionBgEdge_Transparent && edge <= kActionBgEdge_RawWrap
      ? names[edge] : "UNKNOWN";
}

static const char *UpperExtent(ActionBgExtentMode mode) {
  switch (mode) {
    case kActionBgExtent_Inherit: return "INHERIT";
    case kActionBgExtent_Available: return "AVAILABLE";
    case kActionBgExtent_Fixed: return "FIXED";
    default: return "UNKNOWN";
  }
}

static const char *UpperSource(ActionBgSourceKind source) {
  switch (source) {
    case kActionBgSource_NativeTilemap: return "NATIVE";
    case kActionBgSource_WorldMap: return "WORLD";
    case kActionBgSource_AuthenticViewport: return "VIEWPORT";
    default: return "UNKNOWN";
  }
}

static const char *UpperRole(ActionBgLayerRole role) {
  switch (role) {
    case kActionBgLayerRole_Playfield: return "PLAYFIELD";
    case kActionBgLayerRole_Scene: return "SCENE";
    case kActionBgLayerRole_Backdrop: return "BACKDROP";
    case kActionBgLayerRole_Unclassified: return "UNCLASSIFIED";
    default: return "UNKNOWN";
  }
}

static void SetRowText(ActionBgTunerRow *row, const char *key,
                       const char *label, const char *value) {
  if (!row) return;
  snprintf(row->key, sizeof(row->key), "%s", key ? key : "");
  snprintf(row->label, sizeof(row->label), "%s", label ? label : "");
  snprintf(row->value, sizeof(row->value), "%s", value ? value : "");
}

static ActionBgHorizontalExtent EffectiveHorizontal(int layer) {
  const ActionBgLayerDraft *draft = &s_tuner.layer[layer];
  return draft->horizontal_set ? draft->horizontal
      : s_tuner.canonical.layer[layer].horizontal_extent;
}

static ActionBgVerticalExtent EffectiveVertical(int layer) {
  const ActionBgLayerDraft *draft = &s_tuner.layer[layer];
  return draft->vertical_set ? draft->vertical
      : s_tuner.canonical.layer[layer].vertical_extent;
}

static ActionBgHorizontalExtent EffectiveBandHorizontal(int layer, int band) {
  const ActionBgBand *canonical = &s_tuner.canonical.layer[layer].bands[band];
  ActionBgBandDraft *draft = FindBandDraft(
      layer, canonical->y0, canonical->y1, false);
  return draft && draft->horizontal_set
      ? draft->horizontal : canonical->horizontal_extent;
}

static void PushLayerRows(ActionBgTunerRow *out, int capacity, int *count,
                          int layer) {
  char key[48], value[24];
  const ActionBgLayerPlan *canonical = &s_tuner.canonical.layer[layer];
  const ActionBgLayerDraft *draft = &s_tuner.layer[layer];
  ActionBgTunerRow *row = PushRow(
      out, capacity, count, kActionBgTunerRow_Layer, layer, -1);
  if (!row) return;
  snprintf(key, sizeof(key), "bg%d", layer + 1);
  snprintf(value, sizeof(value), "%s %s%s",
           UpperRole(canonical->role),
           UpperSource(canonical->source),
           s_tuner.selected_layer == layer ? " OPEN" : "");
  SetRowText(row, key, layer ? "BG2" : "BG1", value);
  if (s_tuner.selected_layer != layer) return;

  ActionBgEdgeMode edge = draft->edge_set
      ? draft->edge : canonical->default_edge;
  row = PushRow(out, capacity, count, kActionBgTunerRow_Edge, layer, -1);
  if (!row) return;
  snprintf(key, sizeof(key), "bg%d.edge", layer + 1);
  SetRowText(row, key, "edge strategy", UpperEdge(edge));
  row->nested = true;

  row = PushRow(out, capacity, count,
                kActionBgTunerRow_IgnoreSideBounds, layer, -1);
  if (!row) return;
  snprintf(key, sizeof(key), "bg%d.ignore_side_bounds", layer + 1);
  SetRowText(row, key, "ignore side bounds",
             draft->ignore_side_bounds ? "ON" : "OFF");
  row->nested = true;

  ActionBgHorizontalExtent horizontal = EffectiveHorizontal(layer);
  row = PushRow(out, capacity, count,
                kActionBgTunerRow_HorizontalMode, layer, -1);
  if (!row) return;
  snprintf(key, sizeof(key), "bg%d.horizontal", layer + 1);
  SetRowText(row, key, "horizontal cap", UpperExtent(horizontal.mode));
  row->nested = true;
  if (horizontal.mode == kActionBgExtent_Fixed) {
    row = PushRow(out, capacity, count, kActionBgTunerRow_Left, layer, -1);
    if (!row) return;
    snprintf(key, sizeof(key), "bg%d.left", layer + 1);
    snprintf(value, sizeof(value), "%u px", horizontal.left);
    SetRowText(row, key, "left", value);
    row->nested = true;
    row = PushRow(out, capacity, count, kActionBgTunerRow_Right, layer, -1);
    if (!row) return;
    snprintf(key, sizeof(key), "bg%d.right", layer + 1);
    snprintf(value, sizeof(value), "%u px", horizontal.right);
    SetRowText(row, key, "right", value);
    row->nested = true;
  }

  ActionBgVerticalExtent vertical = EffectiveVertical(layer);
  row = PushRow(out, capacity, count,
                kActionBgTunerRow_VerticalMode, layer, -1);
  if (!row) return;
  snprintf(key, sizeof(key), "bg%d.vertical", layer + 1);
  SetRowText(row, key, "vertical cap", UpperExtent(vertical.mode));
  row->nested = true;
  if (vertical.mode == kActionBgExtent_Fixed) {
    row = PushRow(out, capacity, count, kActionBgTunerRow_Top, layer, -1);
    if (!row) return;
    snprintf(key, sizeof(key), "bg%d.top", layer + 1);
    snprintf(value, sizeof(value), "%u px", vertical.top);
    SetRowText(row, key, "top", value);
    row->nested = true;
    row = PushRow(out, capacity, count, kActionBgTunerRow_Bottom, layer, -1);
    if (!row) return;
    snprintf(key, sizeof(key), "bg%d.bottom", layer + 1);
    snprintf(value, sizeof(value), "%u px", vertical.bottom);
    SetRowText(row, key, "bottom", value);
    row->nested = true;
  }

  for (unsigned band = 0; band < canonical->band_count; band++) {
    const ActionBgBand *canonical_band = &canonical->bands[band];
    row = PushRow(out, capacity, count,
                  kActionBgTunerRow_BandHeader, layer, (int)band);
    if (!row) return;
    snprintf(value, sizeof(value), "%s", UpperEdge(canonical_band->edge));
    snprintf(key, sizeof(key), "bg%d.band%u", layer + 1, band);
    char label[32];
    snprintf(label, sizeof(label), "rows %u-%u",
             canonical_band->y0, canonical_band->y1 - 1);
    SetRowText(row, key, label, value);
    row->nested = true;

    ActionBgHorizontalExtent band_extent =
        EffectiveBandHorizontal(layer, (int)band);
    row = PushRow(out, capacity, count,
                  kActionBgTunerRow_BandMode, layer, (int)band);
    if (!row) return;
    snprintf(key, sizeof(key), "bg%d.band%u.horizontal", layer + 1, band);
    SetRowText(row, key, "band cap", UpperExtent(band_extent.mode));
    row->nested = true;
    if (band_extent.mode == kActionBgExtent_Fixed) {
      row = PushRow(out, capacity, count,
                    kActionBgTunerRow_BandLeft, layer, (int)band);
      if (!row) return;
      snprintf(key, sizeof(key), "bg%d.band%u.left", layer + 1, band);
      snprintf(value, sizeof(value), "%u px", band_extent.left);
      SetRowText(row, key, "band left", value);
      row->nested = true;
      row = PushRow(out, capacity, count,
                    kActionBgTunerRow_BandRight, layer, (int)band);
      if (!row) return;
      snprintf(key, sizeof(key), "bg%d.band%u.right", layer + 1, band);
      snprintf(value, sizeof(value), "%u px", band_extent.right);
      SetRowText(row, key, "band right", value);
      row->nested = true;
    }
  }
}

int ActionBgTuner_BuildRows(ActionBgTunerRow *out, int capacity) {
  if (!out || capacity <= 0) return 0;
  int count = 0;
  ActionBgTunerRow *row = PushRow(
      out, capacity, &count, kActionBgTunerRow_Header, -1, -1);
  if (!row) return count;
  if (!s_tuner.live) {
    SetRowText(row, "", "Enter an action stage to tune", "");
    return count;
  }
  char value[24];
  snprintf(value, sizeof(value), "%02X/%02X %s",
           s_tuner.map_group, s_tuner.map_number,
           s_tuner.draft_enabled ? "DRAFT" : "CANON");
  SetRowText(row, "", "Live action room", value);

  row = PushRow(out, capacity, &count, kActionBgTunerRow_Apply, -1, -1);
  if (!row) return count;
  SetRowText(row, "bg_tuner.apply", "apply draft",
             s_tuner.draft_enabled ? "ON" : "OFF");
  row = PushRow(out, capacity, &count, kActionBgTunerRow_Guides, -1, -1);
  if (!row) return count;
  SetRowText(row, "bg_tuner.guides", "extent guides",
             s_tuner.guides_enabled ? "ON" : "OFF");
  for (int layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    PushLayerRows(out, capacity, &count, layer);
    if (count >= capacity) return count;
  }
  row = PushRow(out, capacity, &count, kActionBgTunerRow_Print, -1, -1);
  if (!row) return count;
  SetRowText(row, "bg_tuner.print", "print draft", "LOG");
  row->separator_before = true;
  row = PushRow(out, capacity, &count, kActionBgTunerRow_Reset, -1, -1);
  if (!row) return count;
  SetRowText(row, "bg_tuner.reset", "reset draft", "RESET");
  return count;
}

static int StepExtent(int value, int direction, int maximum) {
  int next = value + (direction < 0 ? -kExtentStep : kExtentStep);
  if (next < 0) next = 0;
  if (next > maximum) next = maximum;
  return next;
}

static ActionBgHorizontalExtent *EditableHorizontal(int layer) {
  ActionBgLayerDraft *draft = &s_tuner.layer[layer];
  if (!draft->horizontal_set) {
    draft->horizontal = s_tuner.canonical.layer[layer].horizontal_extent;
    draft->horizontal_set = true;
  }
  return &draft->horizontal;
}

static ActionBgVerticalExtent *EditableVertical(int layer) {
  ActionBgLayerDraft *draft = &s_tuner.layer[layer];
  if (!draft->vertical_set) {
    draft->vertical = s_tuner.canonical.layer[layer].vertical_extent;
    draft->vertical_set = true;
  }
  return &draft->vertical;
}

static ActionBgHorizontalExtent *EditableBandHorizontal(int layer, int band) {
  const ActionBgBand *canonical = &s_tuner.canonical.layer[layer].bands[band];
  ActionBgBandDraft *draft = FindBandDraft(
      layer, canonical->y0, canonical->y1, true);
  if (!draft) return NULL;
  if (!draft->horizontal_set) {
    draft->horizontal = canonical->horizontal_extent;
    draft->horizontal_set = true;
  }
  return &draft->horizontal;
}

static ActionBgTunerResult ChangeMode(ActionBgExtentMode *mode,
                                      bool allow_inherit, int direction) {
  int first = allow_inherit ? kActionBgExtent_Inherit
                            : kActionBgExtent_Available;
  int count = allow_inherit ? 3 : 2;
  int index = (int)*mode - first;
  index = (index + (direction < 0 ? -1 : 1) + count) % count;
  *mode = (ActionBgExtentMode)(first + index);
  return kActionBgTunerResult_Changed;
}

ActionBgTunerResult ActionBgTuner_Change(const ActionBgTunerRow *row,
                                        int direction) {
  if (!row || !row->selectable || !s_tuner.live || !direction)
    return kActionBgTunerResult_Unchanged;
  if (row->kind == kActionBgTunerRow_Apply) {
    s_tuner.draft_enabled = !s_tuner.draft_enabled;
    return kActionBgTunerResult_Changed;
  }
  if (row->kind == kActionBgTunerRow_Guides) {
    s_tuner.guides_enabled = !s_tuner.guides_enabled;
    return kActionBgTunerResult_Changed;
  }
  if (row->kind == kActionBgTunerRow_Layer) {
    s_tuner.selected_layer = row->layer;
    return kActionBgTunerResult_Changed;
  }
  if (row->layer < 0 || row->layer >= kActionBgPlanLayerCount)
    return kActionBgTunerResult_Unchanged;
  ActionBgLayerDraft *draft = &s_tuner.layer[row->layer];
  switch (row->kind) {
    case kActionBgTunerRow_Edge: {
      ActionBgEdgeMode edge = draft->edge_set
          ? draft->edge
          : s_tuner.canonical.layer[row->layer].default_edge;
      int count = kActionBgEdge_RawWrap - kActionBgEdge_Transparent + 1;
      int index = (int)edge + (direction < 0 ? -1 : 1);
      index = (index + count) % count;
      draft->edge = (ActionBgEdgeMode)index;
      draft->edge_set = true;
      return kActionBgTunerResult_Changed;
    }
    case kActionBgTunerRow_IgnoreSideBounds:
      draft->ignore_side_bounds = !draft->ignore_side_bounds;
      return kActionBgTunerResult_Changed;
    case kActionBgTunerRow_HorizontalMode: {
      ActionBgHorizontalExtent *extent = EditableHorizontal(row->layer);
      ActionBgTunerResult result = ChangeMode(&extent->mode, false, direction);
      if (extent->mode == kActionBgExtent_Fixed &&
          !extent->left && !extent->right) {
        extent->left = s_tuner.limits.left;
        extent->right = s_tuner.limits.right;
      } else if (extent->mode != kActionBgExtent_Fixed) {
        extent->left = extent->right = 0;
      }
      return result;
    }
    case kActionBgTunerRow_Left: {
      ActionBgHorizontalExtent *extent = EditableHorizontal(row->layer);
      int next = StepExtent(extent->left, direction, s_tuner.limits.left);
      if (next == extent->left) return kActionBgTunerResult_AtLimit;
      extent->left = (uint16_t)next;
      return kActionBgTunerResult_Changed;
    }
    case kActionBgTunerRow_Right: {
      ActionBgHorizontalExtent *extent = EditableHorizontal(row->layer);
      int next = StepExtent(extent->right, direction, s_tuner.limits.right);
      if (next == extent->right) return kActionBgTunerResult_AtLimit;
      extent->right = (uint16_t)next;
      return kActionBgTunerResult_Changed;
    }
    case kActionBgTunerRow_VerticalMode: {
      ActionBgVerticalExtent *extent = EditableVertical(row->layer);
      ActionBgTunerResult result = ChangeMode(&extent->mode, false, direction);
      if (extent->mode == kActionBgExtent_Fixed &&
          !extent->top && !extent->bottom) {
        extent->top = s_tuner.limits.top;
        extent->bottom = s_tuner.limits.bottom;
      } else if (extent->mode != kActionBgExtent_Fixed) {
        extent->top = extent->bottom = 0;
      }
      return result;
    }
    case kActionBgTunerRow_Top: {
      ActionBgVerticalExtent *extent = EditableVertical(row->layer);
      int next = StepExtent(extent->top, direction, s_tuner.limits.top);
      if (next == extent->top) return kActionBgTunerResult_AtLimit;
      extent->top = (uint16_t)next;
      return kActionBgTunerResult_Changed;
    }
    case kActionBgTunerRow_Bottom: {
      ActionBgVerticalExtent *extent = EditableVertical(row->layer);
      int next = StepExtent(extent->bottom, direction, s_tuner.limits.bottom);
      if (next == extent->bottom) return kActionBgTunerResult_AtLimit;
      extent->bottom = (uint16_t)next;
      return kActionBgTunerResult_Changed;
    }
    case kActionBgTunerRow_BandMode: {
      if (row->band < 0 ||
          row->band >= s_tuner.canonical.layer[row->layer].band_count)
        return kActionBgTunerResult_Unchanged;
      ActionBgHorizontalExtent *extent =
          EditableBandHorizontal(row->layer, row->band);
      if (!extent) return kActionBgTunerResult_Unchanged;
      ActionBgTunerResult result = ChangeMode(&extent->mode, true, direction);
      if (extent->mode == kActionBgExtent_Fixed &&
          !extent->left && !extent->right) {
        extent->left = s_tuner.limits.left;
        extent->right = s_tuner.limits.right;
      } else if (extent->mode != kActionBgExtent_Fixed) {
        extent->left = extent->right = 0;
      }
      return result;
    }
    case kActionBgTunerRow_BandLeft:
    case kActionBgTunerRow_BandRight: {
      if (row->band < 0 ||
          row->band >= s_tuner.canonical.layer[row->layer].band_count)
        return kActionBgTunerResult_Unchanged;
      ActionBgHorizontalExtent *extent =
          EditableBandHorizontal(row->layer, row->band);
      if (!extent) return kActionBgTunerResult_Unchanged;
      uint16_t *side = row->kind == kActionBgTunerRow_BandLeft
          ? &extent->left : &extent->right;
      int maximum = row->kind == kActionBgTunerRow_BandLeft
          ? s_tuner.limits.left : s_tuner.limits.right;
      int next = StepExtent(*side, direction, maximum);
      if (next == *side) return kActionBgTunerResult_AtLimit;
      *side = (uint16_t)next;
      return kActionBgTunerResult_Changed;
    }
    default:
      return kActionBgTunerResult_Unchanged;
  }
}

static void PrintDraft(void) {
  ActionBgPlan plan;
  if (!BuildEffectivePlan(&plan, true)) return;
  fprintf(stderr, "[action-bg-tuner] room=%02X/%02X apply=%d guides=%d\n",
          s_tuner.map_group, s_tuner.map_number,
          s_tuner.draft_enabled, s_tuner.guides_enabled);
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    const ActionBgLayerPlan *p = &plan.layer[layer];
    fprintf(stderr,
            "[action-bg-tuner] BG%u role=%s source=%s edge=%s "
            "horizontal=%s:%u,%u vertical=%s:%u,%u\n",
            layer + 1, ActionBgLayerRole_Name(p->role),
            ActionBgSourceKind_Name(p->source),
            ActionBgEdgeMode_Name(p->default_edge),
            ActionBgExtentMode_Name(p->horizontal_extent.mode),
            p->horizontal_extent.left, p->horizontal_extent.right,
            ActionBgExtentMode_Name(p->vertical_extent.mode),
            p->vertical_extent.top, p->vertical_extent.bottom);
    for (unsigned band = 0; band < p->band_count; band++) {
      const ActionBgBand *b = &p->bands[band];
      fprintf(stderr,
              "[action-bg-tuner] BG%u band=%u..%u edge=%s "
              "horizontal=%s:%u,%u\n",
              layer + 1, b->y0, b->y1,
              ActionBgEdgeMode_Name(b->edge),
              ActionBgExtentMode_Name(b->horizontal_extent.mode),
              b->horizontal_extent.left, b->horizontal_extent.right);
    }
  }
}

ActionBgTunerResult ActionBgTuner_Activate(const ActionBgTunerRow *row) {
  if (!row || !row->selectable || !s_tuner.live)
    return kActionBgTunerResult_Unchanged;
  if (row->kind == kActionBgTunerRow_Print) {
    PrintDraft();
    return kActionBgTunerResult_Printed;
  }
  if (row->kind == kActionBgTunerRow_Reset) {
    ClearDraft();
    s_tuner.selected_layer = -1;
    return kActionBgTunerResult_Reset;
  }
  if (row->kind == kActionBgTunerRow_Layer) {
    s_tuner.selected_layer = s_tuner.selected_layer == row->layer
        ? -1 : row->layer;
    return kActionBgTunerResult_Changed;
  }
  return ActionBgTuner_Change(row, +1);
}

ActionBgTunerResult ActionBgTuner_ResetRow(const ActionBgTunerRow *row) {
  if (!row || !row->selectable || !s_tuner.live)
    return kActionBgTunerResult_Unchanged;
  if (row->kind == kActionBgTunerRow_Reset)
    return ActionBgTuner_Activate(row);
  if (row->kind == kActionBgTunerRow_Apply) {
    s_tuner.draft_enabled = false;
    return kActionBgTunerResult_Changed;
  }
  if (row->kind == kActionBgTunerRow_Guides) {
    s_tuner.guides_enabled = false;
    return kActionBgTunerResult_Changed;
  }
  if (row->layer < 0 || row->layer >= kActionBgPlanLayerCount)
    return kActionBgTunerResult_Unchanged;
  ActionBgLayerDraft *draft = &s_tuner.layer[row->layer];
  switch (row->kind) {
    case kActionBgTunerRow_Layer:
      memset(draft, 0, sizeof(*draft));
      return kActionBgTunerResult_Reset;
    case kActionBgTunerRow_Edge:
      draft->edge_set = false;
      return kActionBgTunerResult_Reset;
    case kActionBgTunerRow_IgnoreSideBounds:
      draft->ignore_side_bounds = false;
      return kActionBgTunerResult_Reset;
    case kActionBgTunerRow_HorizontalMode:
    case kActionBgTunerRow_Left:
    case kActionBgTunerRow_Right:
      draft->horizontal_set = false;
      return kActionBgTunerResult_Reset;
    case kActionBgTunerRow_VerticalMode:
    case kActionBgTunerRow_Top:
    case kActionBgTunerRow_Bottom:
      draft->vertical_set = false;
      return kActionBgTunerResult_Reset;
    case kActionBgTunerRow_BandMode:
    case kActionBgTunerRow_BandLeft:
    case kActionBgTunerRow_BandRight: {
      if (row->band < 0 ||
          row->band >= s_tuner.canonical.layer[row->layer].band_count)
        return kActionBgTunerResult_Unchanged;
      const ActionBgBand *band =
          &s_tuner.canonical.layer[row->layer].bands[row->band];
      ActionBgBandDraft *band_draft = FindBandDraft(
          row->layer, band->y0, band->y1, false);
      if (band_draft) band_draft->horizontal_set = false;
      return kActionBgTunerResult_Reset;
    }
    default:
      return kActionBgTunerResult_Unchanged;
  }
}

const char *ActionBgTuner_RowHelp(const ActionBgTunerRow *row) {
  if (!row) return "";
  switch (row->kind) {
    case kActionBgTunerRow_Apply:
      return "A/B the sparse draft against the canonical room plan. Drafts "
             "start off and are never persisted.";
    case kActionBgTunerRow_Guides:
      return "Draw the active BG1/BG2 extent boundaries over the game output.";
    case kActionBgTunerRow_Layer:
      return "Open this background layer's source, edge, and per-side extent "
             "controls. Reset clears only this layer's draft.";
    case kActionBgTunerRow_Edge:
      return "Choose how this layer supplies pixels outside the authentic "
             "viewport. The extent remains an independent maximum.";
    case kActionBgTunerRow_IgnoreSideBounds:
      return "Let this background use every horizontally available canvas "
             "pixel past its Diorama side guides. The shared canvas and edge "
             "strategy still apply; stored caps return unchanged when off.";
    case kActionBgTunerRow_HorizontalMode:
    case kActionBgTunerRow_VerticalMode:
      return "Available uses every pixel the source and canvas can supply. "
             "Fixed adds an independent per-side presentation cap.";
    case kActionBgTunerRow_Left:
    case kActionBgTunerRow_Right:
    case kActionBgTunerRow_Top:
    case kActionBgTunerRow_Bottom:
      return "Maximum pixels this layer may contribute beyond that authentic "
             "viewport edge. It cannot manufacture unavailable source pixels.";
    case kActionBgTunerRow_BandMode:
      return "Override the layer's horizontal cap only on this canonical row "
             "band. Inherit follows the layer; Available removes its cap.";
    case kActionBgTunerRow_BandLeft:
    case kActionBgTunerRow_BandRight:
      return "Maximum horizontal extension for this row band on the named side.";
    case kActionBgTunerRow_Print:
      return "Print the fully resolved draft to stderr for transcription into "
             "the canonical action background policy.";
    case kActionBgTunerRow_Reset:
      return "Discard every sparse override for this room and disable applying "
             "the draft. Guide visibility is left unchanged.";
    case kActionBgTunerRow_BandHeader:
      return "Canonical scanline band and edge strategy; its extent can be "
             "tuned independently below.";
    case kActionBgTunerRow_Header:
    default:
      return "Session-local action background extent authoring.";
  }
}

static void PushGuide(ActionBgTunerGuide *out, int capacity, int *count,
                      int layer, int x0, int y0, int x1, int y1) {
  if (!out || !count || *count >= capacity) return;
  out[(*count)++] = (ActionBgTunerGuide) {
    .x0 = (int16_t)x0,
    .y0 = (int16_t)y0,
    .x1 = (int16_t)x1,
    .y1 = (int16_t)y1,
    .layer = (uint8_t)layer,
  };
}

int ActionBgTuner_BuildGuides(const ActionBgPlan *plan,
                              ActionBgTunerGuide *out, int capacity) {
  if (!out || capacity <= 0 || !ActionBgPlan_Validate(plan)) return 0;
  int count = 0;
  for (int layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    const ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    for (int y = 0; y < kAuthenticHeight;) {
      ActionBgRowPolicy row;
      if (!ActionBgLayerPlan_ResolveRow(layer_plan, y, &row)) return 0;
      int y1 = y + 1;
      for (; y1 < kAuthenticHeight; y1++) {
        ActionBgRowPolicy next;
        if (!ActionBgLayerPlan_ResolveRow(layer_plan, y1, &next) ||
            next.horizontal_extent.mode != row.horizontal_extent.mode ||
            next.horizontal_extent.left != row.horizontal_extent.left ||
            next.horizontal_extent.right != row.horizontal_extent.right)
          break;
      }
      if (row.horizontal_extent.mode == kActionBgExtent_Fixed) {
        PushGuide(out, capacity, &count, layer,
                  -(int)row.horizontal_extent.left, y,
                  -(int)row.horizontal_extent.left, y1);
        PushGuide(out, capacity, &count, layer,
                  kAuthenticWidth + row.horizontal_extent.right, y,
                  kAuthenticWidth + row.horizontal_extent.right, y1);
      }
      y = y1;
    }
    if (layer_plan->vertical_extent.mode == kActionBgExtent_Fixed) {
      const ActionBgHorizontalExtent *horizontal =
          &layer_plan->horizontal_extent;
      int x0 = horizontal->mode == kActionBgExtent_Fixed
          ? -(int)horizontal->left : 0;
      int x1 = horizontal->mode == kActionBgExtent_Fixed
          ? kAuthenticWidth + horizontal->right : kAuthenticWidth;
      PushGuide(out, capacity, &count, layer,
                x0, -(int)layer_plan->vertical_extent.top,
                x1, -(int)layer_plan->vertical_extent.top);
      PushGuide(out, capacity, &count, layer,
                x0, kAuthenticHeight + layer_plan->vertical_extent.bottom,
                x1, kAuthenticHeight + layer_plan->vertical_extent.bottom);
    }
  }
  return count;
}
