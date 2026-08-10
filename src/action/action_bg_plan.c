#include "action_bg_plan.h"

#include <stddef.h>
#include <string.h>

enum {
  kAuthenticHeight = 224,
  kWorldPagePixels = 256,
  kWideWorldMinimum = 512,
  kMapGroupFirst = 1,
  kMapGroupLast = 7,
  kBloodpool = 2,
  kAitos = 4,
  kNorthwall = 6,
  kDeathHeim = 7,
  kBloodpoolFirstMap = 1,
  kBloodpoolLastMoonWaterMap = 2,
  kBloodpoolWaterStartY = 136,
  kDeathHeimHub = 1,
  kDeathHeimFirstBoss = 2,
  kDeathHeimLastBoss = 7,
  kDeathHeimFinalBoss = 8,
  kDeathHeimFogStartY = 144,
  kDeathHeimFinalProgress = 7,
  kDeathHeimSkySettled = 3,
  kEndingSkyBg1Page = 0x64,
  kEndingSkyBg2Page = 0x74,
  kBgscPageMask = 0xFC,
};

static const uint8_t kLastMapByGroup[kMapGroupLast + 1] = {
  [1] = 4,
  [2] = 8,
  [3] = 6,
  [4] = 7,
  [5] = 8,
  [6] = 8,
  [7] = 8,
};

_Static_assert(kActionBgPlanLayerCount == 2,
               "action background plan owns BG1/BG2 only");
_Static_assert(kActionBgMaxBands >= 1,
               "current mixed BG2 policy needs one override band");

static bool ValidMap(uint8_t group, uint8_t map) {
  return group >= kMapGroupFirst && group <= kMapGroupLast && map > 0 &&
      map <= kLastMapByGroup[group];
}

static bool ValidLayer(const ActionBgLayerState *layer) {
  return layer && layer->world_width && layer->world_height &&
      !(layer->world_width % kWorldPagePixels) &&
      !(layer->world_height % kWorldPagePixels);
}

static bool WorldRingEligible(const ActionBgLayerState *layer) {
  const uint16_t ppu_base = (uint16_t)(layer->bgsc & kBgscPageMask) << 8;
  return (layer->bgsc & 3u) == 3u && layer->tilemap_base == ppu_base;
}

static ActionBgHorizontalExtent AvailableHorizontalExtent(void) {
  return (ActionBgHorizontalExtent) {
    .mode = kActionBgExtent_Available,
  };
}

static ActionBgHorizontalExtent FixedHorizontalExtent(uint16_t left,
                                                       uint16_t right) {
  return (ActionBgHorizontalExtent) {
    .mode = kActionBgExtent_Fixed,
    .left = left,
    .right = right,
  };
}

static ActionBgVerticalExtent AvailableVerticalExtent(void) {
  return (ActionBgVerticalExtent) {
    .mode = kActionBgExtent_Available,
  };
}

static void ClearBands(ActionBgLayerPlan *layer) {
  layer->band_count = 0;
  memset(layer->bands, 0, sizeof(layer->bands));
}

static ActionBgLayerPlan BaseLayerPlan(const ActionBgLayerState *state,
                                       ActionBgLayerRole role) {
  const bool world = WorldRingEligible(state);
  return (ActionBgLayerPlan) {
    .valid = true,
    .role = role,
    .source = world ? kActionBgSource_WorldMap
                    : kActionBgSource_NativeTilemap,
    .default_edge = world ? kActionBgEdge_LiveWorld
                          : kActionBgEdge_RawWrap,
    .world_width = state->world_width,
    .world_height = state->world_height,
    .horizontal_extent = AvailableHorizontalExtent(),
    .vertical_extent = AvailableVerticalExtent(),
  };
}

static bool UsesCyclicBg2(uint8_t group, uint8_t map) {
  return (group == kAitos && map >= 1 && map <= 3) ||
      (group == kNorthwall && ((map >= 1 && map <= 5) || map == 8)) ||
      (group == kDeathHeim && map >= kDeathHeimFirstBoss &&
       map <= kDeathHeimLastBoss);
}

static void ClassifyNarrowBg2(const ActionBgFrameState *state,
                              ActionBgLayerPlan *bg2) {
  if (state->layer[1].world_width >= kWideWorldMinimum) return;
  bg2->source = kActionBgSource_AuthenticViewport;
  if (!state->decorative_padding_enabled) {
    bg2->default_edge = kActionBgEdge_Clamp;
    return;
  }
  bg2->default_edge = UsesCyclicBg2(state->map_group, state->map_number)
      ? kActionBgEdge_Repeat : kActionBgEdge_Mirror;
  /* A cyclic backdrop is authored to continue. A mirrored backdrop is the
   * finite/unique-art class: mirroring it farther eventually reintroduces a
   * moon, landmark, or hard silhouette. Keep that family authentic-width and
   * let the independently wider playfield remain visible around it. */
  if (bg2->default_edge == kActionBgEdge_Mirror)
    bg2->horizontal_extent = FixedHorizontalExtent(0, 0);
  if (state->map_group == kBloodpool &&
      state->map_number >= kBloodpoolFirstMap &&
      state->map_number <= kBloodpoolLastMoonWaterMap) {
    bg2->bands[0] = (ActionBgBand) {
      .y0 = kBloodpoolWaterStartY,
      .y1 = kAuthenticHeight,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = AvailableHorizontalExtent(),
    };
    bg2->band_count = 1;
  }
}

static bool DeathHeimEndingSky(const ActionBgFrameState *state) {
  const bool sky_pages =
      (state->layer[0].bgsc & kBgscPageMask) == kEndingSkyBg1Page &&
      (state->layer[1].bgsc & kBgscPageMask) == kEndingSkyBg2Page;
  return state->death_heim_progress >= kDeathHeimFinalProgress &&
      (sky_pages ||
       state->death_heim_ending_state >= kDeathHeimSkySettled);
}

static void ClassifyDeathHeim(const ActionBgFrameState *state,
                              ActionBgPlan *plan) {
  if (state->map_number == kDeathHeimFinalBoss) {
    plan->bound_canvas_to_world = false;
    for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
      plan->layer[layer].role = kActionBgLayerRole_Backdrop;
      plan->layer[layer].source = kActionBgSource_NativeTilemap;
      plan->layer[layer].default_edge = kActionBgEdge_RawWrap;
      /* 0708 is an authored two-plane raster scene. Its 256px native maps
       * intentionally wrap across the complete wide canvas, so discard any
       * narrow-decorative cap applied before this special-room override. */
      plan->layer[layer].horizontal_extent = AvailableHorizontalExtent();
      plan->layer[layer].vertical_extent = AvailableVerticalExtent();
      ClearBands(&plan->layer[layer]);
    }
    plan->layer[0].role = kActionBgLayerRole_Scene;
    return;
  }
  if (state->map_number != kDeathHeimHub) return;
  plan->bound_canvas_to_world = false;

  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    plan->layer[layer].source = kActionBgSource_AuthenticViewport;
    plan->layer[layer].default_edge = kActionBgEdge_Clamp;
    ClearBands(&plan->layer[layer]);
  }
  plan->layer[1].horizontal_extent = FixedHorizontalExtent(0, 0);
  if (DeathHeimEndingSky(state)) {
    plan->layer[1].default_edge = kActionBgEdge_Mirror;
  } else {
    plan->layer[1].bands[0] = (ActionBgBand) {
      .y0 = kDeathHeimFogStartY,
      .y1 = kAuthenticHeight,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = AvailableHorizontalExtent(),
    };
    plan->layer[1].band_count = 1;
  }
}

bool ActionBgPlan_Build(const ActionBgFrameState *state, ActionBgPlan *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!state || !out || !ValidMap(state->map_group, state->map_number))
    return false;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++)
    if (!ValidLayer(&state->layer[layer])) return false;

  ActionBgPlan built = {
    .valid = true,
    .bound_canvas_to_world = true,
  };
  built.layer[0] = BaseLayerPlan(
      &state->layer[0], kActionBgLayerRole_Playfield);
  built.layer[1] = BaseLayerPlan(
      &state->layer[1], kActionBgLayerRole_Backdrop);
  ClassifyNarrowBg2(state, &built.layer[1]);
  if (state->map_group == kDeathHeim)
    ClassifyDeathHeim(state, &built);
  if (!ActionBgPlan_Validate(&built)) return false;
  *out = built;
  return true;
}

static bool ValidSource(ActionBgSourceKind source) {
  return source >= kActionBgSource_NativeTilemap &&
      source <= kActionBgSource_AuthenticViewport;
}

static bool ValidRole(ActionBgLayerRole role) {
  return role >= kActionBgLayerRole_Unclassified &&
      role <= kActionBgLayerRole_Backdrop;
}

static bool ValidEdge(ActionBgEdgeMode edge) {
  return edge >= kActionBgEdge_Transparent && edge <= kActionBgEdge_RawWrap;
}

static bool ValidExtentMode(ActionBgExtentMode mode, bool allow_inherit) {
  return (allow_inherit && mode == kActionBgExtent_Inherit) ||
      mode == kActionBgExtent_Available || mode == kActionBgExtent_Fixed;
}

static bool ValidHorizontalExtent(const ActionBgHorizontalExtent *extent,
                                  bool allow_inherit) {
  if (!extent || !ValidExtentMode(extent->mode, allow_inherit)) return false;
  /* Canonicalize modes that do not consume numeric caps. This prevents stale
   * draft values from surviving mode changes and keeps value-record memcmp
   * meaningful at the immutable frame handoff. */
  return extent->mode == kActionBgExtent_Fixed ||
      (!extent->left && !extent->right);
}

static bool ValidVerticalExtent(const ActionBgVerticalExtent *extent) {
  if (!extent || !ValidExtentMode(extent->mode, false)) return false;
  return extent->mode == kActionBgExtent_Fixed ||
      (!extent->top && !extent->bottom);
}

bool ActionBgLayerPlan_Validate(const ActionBgLayerPlan *layer) {
  if (!layer || !layer->valid || !ValidRole(layer->role) ||
      !ValidSource(layer->source) ||
      !ValidEdge(layer->default_edge) ||
      !ValidHorizontalExtent(&layer->horizontal_extent, false) ||
      !ValidVerticalExtent(&layer->vertical_extent) ||
      layer->band_count > kActionBgMaxBands)
    return false;

  uint16_t preceding_y1 = 0;
  for (unsigned i = 0; i < layer->band_count; i++) {
    const ActionBgBand *band = &layer->bands[i];
    if (band->y0 >= band->y1 || band->y1 > kAuthenticHeight ||
        (i && band->y0 < preceding_y1) || !ValidEdge(band->edge) ||
        !ValidHorizontalExtent(&band->horizontal_extent, true))
      return false;
    preceding_y1 = band->y1;
  }
  return true;
}

bool ActionBgPlan_Validate(const ActionBgPlan *plan) {
  if (!plan || !plan->valid) return false;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++)
    if (!ActionBgLayerPlan_Validate(&plan->layer[layer])) return false;
  return true;
}

bool ActionBgLayerPlan_ResolveRow(const ActionBgLayerPlan *layer,
                                  int authentic_y,
                                  ActionBgRowPolicy *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!out || !ActionBgLayerPlan_Validate(layer)) return false;
  ActionBgRowPolicy resolved = {
    .edge = layer->default_edge,
    .horizontal_extent = layer->horizontal_extent,
  };
  for (unsigned i = 0; i < layer->band_count; i++) {
    const ActionBgBand *band = &layer->bands[i];
    if (authentic_y < (int)band->y0 || authentic_y >= (int)band->y1)
      continue;
    resolved.edge = band->edge;
    if (band->horizontal_extent.mode != kActionBgExtent_Inherit)
      resolved.horizontal_extent = band->horizontal_extent;
    break;
  }
  *out = resolved;
  return true;
}

bool ActionBgPlan_CompilePresentation(
    const ActionBgPlan *plan, ActionBgPresentationPolicy *out) {
  if (out) {
    memset(out, 0, sizeof(*out));
    out->repeat_band_layer = -1;
  }
  if (!plan || !out || !ActionBgPlan_Validate(plan)) return false;

  ActionBgPresentationPolicy built = { .repeat_band_layer = -1 };
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    const ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    if (!layer_plan->valid || layer_plan->band_count > kActionBgMaxBands)
      return false;
    const uint8_t mask = (uint8_t)(1u << layer);
    switch (layer_plan->default_edge) {
      case kActionBgEdge_Clamp:  built.clamp_layers |= mask; break;
      case kActionBgEdge_Mirror: built.mirror_layers |= mask; break;
      case kActionBgEdge_Repeat: built.repeat_layers |= mask; break;
      case kActionBgEdge_Transparent:
      case kActionBgEdge_LiveWorld:
      case kActionBgEdge_RawWrap:
        break;
      default:
        return false;
    }
    for (unsigned band = 0; band < layer_plan->band_count; band++) {
      const ActionBgBand *entry = &layer_plan->bands[band];
      if (entry->edge != kActionBgEdge_Repeat ||
          entry->y0 >= entry->y1 || entry->y1 > kAuthenticHeight ||
          built.repeat_band_layer >= 0)
        return false;
      built.repeat_band_layer = (int8_t)layer;
      built.repeat_band_y0 = (uint8_t)entry->y0;
      built.repeat_band_y1 = (uint8_t)entry->y1;
    }
  }
  built.bound_canvas_to_world = plan->bound_canvas_to_world;
  *out = built;
  return true;
}

void ActionBgPlan_InitNative(ActionBgPlan *out) {
  if (!out) return;
  *out = (ActionBgPlan){ .valid = true };
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    out->layer[layer].valid = true;
    out->layer[layer].source = kActionBgSource_NativeTilemap;
    out->layer[layer].default_edge = kActionBgEdge_RawWrap;
    out->layer[layer].horizontal_extent = AvailableHorizontalExtent();
    out->layer[layer].vertical_extent = AvailableVerticalExtent();
  }
}

bool ActionBgPlan_ApplyPresentationPolicy(
    ActionBgPlan *plan, const ActionBgPresentationPolicy *policy) {
  if (!policy || !ActionBgPlan_Validate(plan)) return false;
  const uint8_t owned_mask = (1u << kActionBgPlanLayerCount) - 1u;
  const uint8_t clamp = policy->clamp_layers & owned_mask;
  const uint8_t mirror = policy->mirror_layers & owned_mask;
  const uint8_t repeat = policy->repeat_layers & owned_mask;
  if ((clamp & mirror) || (clamp & repeat) || (mirror & repeat) ||
      policy->repeat_band_layer < -1 ||
      policy->repeat_band_layer >= kActionBgPlanLayerCount ||
      (policy->repeat_band_layer >= 0 &&
       (policy->repeat_band_y0 >= policy->repeat_band_y1 ||
        policy->repeat_band_y1 > kAuthenticHeight)))
    return false;

  ActionBgPlan built = *plan;
  built.bound_canvas_to_world = policy->bound_canvas_to_world;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    ActionBgLayerPlan *layer_plan = &built.layer[layer];
    const uint8_t mask = (uint8_t)(1u << layer);
    layer_plan->valid = true;
    ClearBands(layer_plan);
    layer_plan->horizontal_extent = AvailableHorizontalExtent();
    layer_plan->vertical_extent = AvailableVerticalExtent();
    if (clamp & mask)
      layer_plan->default_edge = kActionBgEdge_Clamp;
    else if (mirror & mask)
      layer_plan->default_edge = kActionBgEdge_Mirror;
    else if (repeat & mask)
      layer_plan->default_edge = kActionBgEdge_Repeat;
    else
      layer_plan->default_edge = kActionBgEdge_RawWrap;
  }
  if (policy->repeat_band_layer >= 0) {
    ActionBgLayerPlan *layer_plan =
        &built.layer[policy->repeat_band_layer];
    layer_plan->bands[0] = (ActionBgBand) {
      .y0 = policy->repeat_band_y0,
      .y1 = policy->repeat_band_y1,
      .edge = kActionBgEdge_Repeat,
    };
    layer_plan->band_count = 1;
  }
  if (!ActionBgPlan_Validate(&built)) return false;
  *plan = built;
  return true;
}

uint8_t ActionBgPlan_ClampUnboundWorldLayers(
    ActionBgPlan *plan, uint8_t bound_layers, uint8_t visible_layers) {
  if (!ActionBgPlan_Validate(plan)) return 0;
  uint8_t clamp_layers = 0;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    const uint8_t layer_mask = (uint8_t)(1u << layer);
    if (!layer_plan->valid ||
        layer_plan->source != kActionBgSource_WorldMap ||
        !(visible_layers & layer_mask) ||
        (bound_layers & layer_mask))
      continue;
    clamp_layers |= layer_mask;
    layer_plan->source = kActionBgSource_AuthenticViewport;
    layer_plan->default_edge = kActionBgEdge_Clamp;
    layer_plan->horizontal_extent = AvailableHorizontalExtent();
    layer_plan->vertical_extent = AvailableVerticalExtent();
    ClearBands(layer_plan);
  }
  return clamp_layers;
}

static int FindPrimaryRoleLayer(const ActionBgPlan *plan, bool allow_scene) {
  int primary = -1;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    const ActionBgLayerRole role = plan->layer[layer].role;
    if (role != kActionBgLayerRole_Playfield &&
        !(allow_scene && role == kActionBgLayerRole_Scene))
      continue;
    if (primary >= 0) return -1;
    primary = (int)layer;
  }
  return primary;
}

int ActionBgPlan_PlayfieldLayer(const ActionBgPlan *plan) {
  return ActionBgPlan_Validate(plan)
      ? FindPrimaryRoleLayer(plan, false) : -1;
}

int ActionBgPlan_PrimaryLayer(const ActionBgPlan *plan) {
  return ActionBgPlan_Validate(plan)
      ? FindPrimaryRoleLayer(plan, true) : -1;
}

int ActionBgPlan_CanvasOwner(const ActionBgPlan *plan) {
  if (!ActionBgPlan_Validate(plan) || !plan->bound_canvas_to_world) return -1;
  const int owner = FindPrimaryRoleLayer(plan, false);
  if (owner < 0 || plan->layer[owner].source != kActionBgSource_WorldMap)
    return -1;
  return owner;
}

const char *ActionBgSourceKind_Name(ActionBgSourceKind source) {
  switch (source) {
    case kActionBgSource_NativeTilemap: return "native";
    case kActionBgSource_WorldMap: return "world";
    case kActionBgSource_AuthenticViewport: return "viewport";
    default: return "unknown";
  }
}

const char *ActionBgLayerRole_Name(ActionBgLayerRole role) {
  switch (role) {
    case kActionBgLayerRole_Unclassified: return "unclassified";
    case kActionBgLayerRole_Playfield: return "playfield";
    case kActionBgLayerRole_Scene: return "scene";
    case kActionBgLayerRole_Backdrop: return "backdrop";
    default: return "unknown";
  }
}

const char *ActionBgEdgeMode_Name(ActionBgEdgeMode edge) {
  switch (edge) {
    case kActionBgEdge_Transparent: return "transparent";
    case kActionBgEdge_LiveWorld: return "world";
    case kActionBgEdge_Clamp: return "clamp";
    case kActionBgEdge_Mirror: return "mirror";
    case kActionBgEdge_Repeat: return "repeat";
    case kActionBgEdge_RawWrap: return "raw";
    default: return "unknown";
  }
}

const char *ActionBgExtentMode_Name(ActionBgExtentMode mode) {
  switch (mode) {
    case kActionBgExtent_Inherit: return "inherit";
    case kActionBgExtent_Available: return "available";
    case kActionBgExtent_Fixed: return "fixed";
    default: return "unknown";
  }
}
