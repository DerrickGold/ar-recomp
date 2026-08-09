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
  kBloodpoolAct1Map = 1,
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

static ActionBgLayerPlan BaseLayerPlan(const ActionBgLayerState *state) {
  const bool world = WorldRingEligible(state);
  return (ActionBgLayerPlan) {
    .valid = true,
    .source = world ? kActionBgSource_WorldMap
                    : kActionBgSource_NativeTilemap,
    .default_edge = world ? kActionBgEdge_LiveWorld
                          : kActionBgEdge_RawWrap,
    .world_width = state->world_width,
    .world_height = state->world_height,
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
  if (state->map_group == kBloodpool &&
      state->map_number == kBloodpoolAct1Map) {
    bg2->bands[0] = (ActionBgBand) {
      .y0 = kBloodpoolWaterStartY,
      .y1 = kAuthenticHeight,
      .edge = kActionBgEdge_Repeat,
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
      plan->layer[layer].source = kActionBgSource_NativeTilemap;
      plan->layer[layer].default_edge = kActionBgEdge_RawWrap;
      plan->layer[layer].band_count = 0;
    }
    return;
  }
  if (state->map_number != kDeathHeimHub) return;
  plan->bound_canvas_to_world = false;

  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    plan->layer[layer].source = kActionBgSource_AuthenticViewport;
    plan->layer[layer].default_edge = kActionBgEdge_Clamp;
    plan->layer[layer].band_count = 0;
  }
  if (DeathHeimEndingSky(state)) {
    plan->layer[1].default_edge = kActionBgEdge_Mirror;
  } else {
    plan->layer[1].bands[0] = (ActionBgBand) {
      .y0 = kDeathHeimFogStartY,
      .y1 = kAuthenticHeight,
      .edge = kActionBgEdge_Repeat,
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
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++)
    built.layer[layer] = BaseLayerPlan(&state->layer[layer]);
  ClassifyNarrowBg2(state, &built.layer[1]);
  if (state->map_group == kDeathHeim)
    ClassifyDeathHeim(state, &built);
  *out = built;
  return true;
}

bool ActionBgPlan_CompilePresentation(
    const ActionBgPlan *plan, ActionBgPresentationPolicy *out) {
  if (out) {
    memset(out, 0, sizeof(*out));
    out->repeat_band_layer = -1;
  }
  if (!plan || !out || !plan->valid) return false;

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
  }
}

bool ActionBgPlan_ApplyPresentationPolicy(
    ActionBgPlan *plan, const ActionBgPresentationPolicy *policy) {
  if (!plan || !policy || !plan->valid) return false;
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
    layer_plan->band_count = 0;
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
  *plan = built;
  return true;
}

uint8_t ActionBgPlan_ClampUnboundWorldLayers(
    ActionBgPlan *plan, uint8_t bound_layers, uint8_t visible_layers) {
  if (!plan || !plan->valid) return 0;
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
    layer_plan->band_count = 0;
  }
  return clamp_layers;
}

const char *ActionBgSourceKind_Name(ActionBgSourceKind source) {
  switch (source) {
    case kActionBgSource_NativeTilemap: return "native";
    case kActionBgSource_WorldMap: return "world";
    case kActionBgSource_AuthenticViewport: return "viewport";
    default: return "unknown";
  }
}
