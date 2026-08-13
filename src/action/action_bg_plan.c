#include "action_bg_plan.h"

#include <stddef.h>
#include <string.h>

#include "actraiser_game.h"

enum {
  kWorldPagePixels = 256,
  /* One complete native 64x64 tilemap period. Maps below this width are
   * presentation-owned; an exactly matching map can also be an authored
   * decoded-world cycle when the scene topology proves it. */
  kWorldTilemapPeriodPixels = 512,
  kMapGroupFirst = 1,
  kMapGroupLast = 7,
  kBg1 = 0,
  kBg2 = 1,
  kFillmore = 1,
  kBloodpool = 2,
  kKasandora = 3,
  kAitos = 4,
  kMarahna = 5,
  kNorthwall = 6,
  kDeathHeim = 7,
  kFillmoreAct1 = 1,
  kBloodpoolFirstMap = 1,
  kBloodpoolAct2 = 2,
  kBloodpoolWaterStartY = 136,
  kBloodpoolMap6 = 6,
  kBloodpoolMap7 = 7,
  kBloodpoolBoss = 8,
  kKasandoraFirstHybridRoom = 1,
  kKasandoraLastHybridRoom = 2,
  kKasandoraDunesWorldY = 256,
  kMarahnaMap5 = 5,
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

typedef struct TunedLayerPolicy {
  uint8_t map_group;
  uint8_t map_number;
  uint8_t layer;
  ActionBgSourceKind required_source;
  ActionBgEdgeMode required_edge;
  ActionBgEdgeMode edge;
  ActionBgMotionMode motion;
  uint16_t left;
  uint16_t right;
  uint16_t top;
  uint16_t bottom;
  bool apply_horizontal_extent;
  bool apply_vertical_extent;
  bool requires_existing_band;
  bool bands_inherit_extent;
} TunedLayerPolicy;

/* Canonical transcription target for live BG Extents exports. Classification
 * still owns role/source/bands. Each entry is guarded by that canonical
 * source/edge and, where required, the classified band before applying the
 * exported edge, motion and explicitly selected extents; this prevents a
 * stale tuning from silently reclassifying another scene. */
static const TunedLayerPolicy kTunedLayerPolicies[] = {
  {
    .map_group = kFillmore,
    .map_number = kFillmoreAct1,
    .layer = kBg2,
    .required_source = kActionBgSource_WorldMap,
    .required_edge = kActionBgEdge_LiveWorld,
    .edge = kActionBgEdge_LiveWorld,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 128,
    .right = 128,
  },
  {
    .map_group = kBloodpool,
    .map_number = kBloodpoolFirstMap,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 76,
    .right = 100,
  },
  {
    .map_group = kBloodpool,
    .map_number = kBloodpoolAct2,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 68,
    .right = 68,
    .bands_inherit_extent = true,
  },
  {
    .map_group = kBloodpool,
    .map_number = kBloodpoolMap6,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 68,
    .right = 68,
  },
  {
    .map_group = kBloodpool,
    .map_number = kBloodpoolMap7,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 92,
    .right = 92,
  },
  {
    .map_group = kBloodpool,
    .map_number = kBloodpoolBoss,
    .layer = kBg1,
    .required_source = kActionBgSource_WorldMap,
    .required_edge = kActionBgEdge_LiveWorld,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 16,
    .right = 16,
  },
  {
    .map_group = kBloodpool,
    .map_number = kBloodpoolBoss,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 0,
    .right = 0,
  },
  {
    .map_group = kKasandora,
    .map_number = kKasandoraFirstHybridRoom,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 128,
    .right = 128,
    .requires_existing_band = true,
  },
  {
    .map_group = kKasandora,
    .map_number = kKasandoraLastHybridRoom,
    .layer = kBg2,
    .required_source = kActionBgSource_AuthenticViewport,
    .required_edge = kActionBgEdge_Mirror,
    .edge = kActionBgEdge_Mirror,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 128,
    .right = 128,
    .requires_existing_band = true,
  },
  {
    .map_group = kMarahna,
    .map_number = kMarahnaMap5,
    .layer = kBg2,
    .required_source = kActionBgSource_WorldMap,
    .required_edge = kActionBgEdge_LiveWorld,
    .edge = kActionBgEdge_Repeat,
    .motion = kActionBgMotion_FillRelative,
    .apply_horizontal_extent = true,
    .left = 128,
    .right = 128,
  },
  {
    /* Promoted verbatim from the 20260812-000613 BG Extents draft. This
     * 512px decoder state uses a native 32x32 PPU map, so source/edge remain
     * native/raw; only its independently verified vertical budget is tuned. */
    .map_group = kAitos,
    .map_number = 2,
    .layer = kBg2,
    .required_source = kActionBgSource_NativeTilemap,
    .required_edge = kActionBgEdge_RawWrap,
    .edge = kActionBgEdge_RawWrap,
    .motion = kActionBgMotion_FillRelative,
    .top = 24,
    .bottom = kActionBgAitosWaterfallBottomExtensionPixels,
    .apply_vertical_extent = true,
  },
};

_Static_assert(kActionBgPlanLayerCount == 2,
               "action background plan owns BG1/BG2 only");
_Static_assert(kActionBgMaxBands >= 4,
               "live authoring promises four independent row bands");

static bool ValidMap(uint8_t group, uint8_t map) {
  return ActRaiser_IsActionMap(group, map);
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

static ActionBgVerticalExtent FixedVerticalExtent(uint16_t top,
                                                   uint16_t bottom) {
  return (ActionBgVerticalExtent) {
    .mode = kActionBgExtent_Fixed,
    .top = top,
    .bottom = bottom,
  };
}

static void ClearBands(ActionBgLayerPlan *layer) {
  layer->band_count = 0;
  memset(layer->bands, 0, sizeof(layer->bands));
}

static void SetLayerSource(ActionBgLayerPlan *layer,
                           ActionBgSourceKind source) {
  if (!layer) return;
  layer->source = source;
  if (source != kActionBgSource_WorldMap)
    layer->wrap_world_x = false;
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
    .default_motion = kActionBgMotion_FillRelative,
    .camera_y = state->camera_y,
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
  if (state->layer[1].world_width >= kWorldTilemapPeriodPixels) return;
  SetLayerSource(bg2, kActionBgSource_AuthenticViewport);
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
      state->map_number <= kBloodpoolAct2) {
    bg2->bands[0] = (ActionBgBand) {
      .y0 = kBloodpoolWaterStartY,
      .y1 = kActRaiserAuthenticHeight,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = AvailableHorizontalExtent(),
    };
    bg2->band_count = 1;
  }
}

static void ClassifyMarahnaSharedCameraBg2(
    const ActionBgFrameState *state, ActionBgLayerPlan *bg2) {
  if (state->map_group != kMarahna ||
      state->layer[1].world_width != kWorldTilemapPeriodPixels ||
      state->layer[0].world_width <= state->layer[1].world_width ||
      state->layer[0].camera_x != state->layer[1].camera_x ||
      bg2->source != kActionBgSource_WorldMap)
    return;

  /* Marahna's action subsections declare a 512px BG2 map but drive it with the
   * same full X camera as the independently wider BG1 playfield. Captures from
   * 0501 (X=543) and 0502 (X=503) prove every authentic ring word equals the
   * decoded BG2 word at X mod 64 tiles. The structural relationship, not the
   * subsection number, therefore identifies one authored horizontal cycle. */
  bg2->wrap_world_x = true;
}

static void ClassifyKasandora(const ActionBgFrameState *state,
                              ActionBgPlan *plan) {
  if (!state->decorative_padding_enabled ||
      state->map_group != kKasandora ||
      state->map_number < kKasandoraFirstHybridRoom ||
      state->map_number > kKasandoraLastHybridRoom)
    return;
  ActionBgLayerPlan *bg2 = &plan->layer[1];
  if (bg2->source != kActionBgSource_WorldMap ||
      bg2->default_edge != kActionBgEdge_LiveWorld ||
      bg2->world_height <= kKasandoraDunesWorldY)
    return;

  /* Rooms 0301/0302 author sparse clouds above BG2 world Y=256 and cyclic
   * dunes at and below it. Keep that content seam in world coordinates; the
   * row resolver projects it through the live parallax camera each frame. */
  SetLayerSource(bg2, kActionBgSource_AuthenticViewport);
  bg2->default_edge = kActionBgEdge_Mirror;
  ClearBands(bg2);
  bg2->bands[0] = (ActionBgBand) {
    .y0 = kKasandoraDunesWorldY,
    .y1 = bg2->world_height,
    .edge = kActionBgEdge_Repeat,
    .anchor = kActionBgBandAnchor_World,
    .horizontal_extent = AvailableHorizontalExtent(),
  };
  bg2->band_count = 1;
}

static void ApplyTunedMapOverrides(const ActionBgFrameState *state,
                                   ActionBgPlan *plan) {
  if (!state->decorative_padding_enabled) return;
  for (unsigned i = 0;
       i < sizeof(kTunedLayerPolicies) / sizeof(kTunedLayerPolicies[0]);
       i++) {
    const TunedLayerPolicy *tuning = &kTunedLayerPolicies[i];
    if (tuning->layer >= kActionBgPlanLayerCount) continue;
    ActionBgLayerPlan *layer = &plan->layer[tuning->layer];
    if (state->map_group != tuning->map_group ||
        state->map_number != tuning->map_number ||
        layer->source != tuning->required_source ||
        layer->default_edge != tuning->required_edge ||
        (tuning->requires_existing_band && layer->band_count == 0))
      continue;
    layer->default_edge = tuning->edge;
    layer->default_motion = tuning->motion;
    if (tuning->apply_horizontal_extent) {
      layer->horizontal_extent = FixedHorizontalExtent(
          tuning->left, tuning->right);
    }
    if (tuning->apply_vertical_extent) {
      layer->vertical_extent = FixedVerticalExtent(
          tuning->top, tuning->bottom);
    }
    if (tuning->apply_horizontal_extent && tuning->bands_inherit_extent) {
      for (unsigned band = 0; band < layer->band_count; band++) {
        layer->bands[band].horizontal_extent = (ActionBgHorizontalExtent) {
          .mode = kActionBgExtent_Inherit,
        };
      }
    }
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
      SetLayerSource(&plan->layer[layer], kActionBgSource_NativeTilemap);
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
    SetLayerSource(&plan->layer[layer],
                   kActionBgSource_AuthenticViewport);
    plan->layer[layer].default_edge = kActionBgEdge_Clamp;
    ClearBands(&plan->layer[layer]);
  }
  plan->layer[1].horizontal_extent = FixedHorizontalExtent(0, 0);
  if (DeathHeimEndingSky(state)) {
    plan->layer[1].default_edge = kActionBgEdge_Mirror;
  } else {
    plan->layer[1].bands[0] = (ActionBgBand) {
      .y0 = kDeathHeimFogStartY,
      .y1 = kActRaiserAuthenticHeight,
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
  ClassifyMarahnaSharedCameraBg2(state, &built.layer[1]);
  ClassifyKasandora(state, &built);
  if (state->map_group == kDeathHeim)
    ClassifyDeathHeim(state, &built);
  ApplyTunedMapOverrides(state, &built);
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

static bool ValidMotion(ActionBgMotionMode motion) {
  return motion >= kActionBgMotion_FillRelative &&
      motion <= kActionBgMotion_NormalScroll;
}

static bool ValidBandAnchor(ActionBgBandAnchor anchor) {
  return anchor >= kActionBgBandAnchor_Screen &&
      anchor <= kActionBgBandAnchor_World;
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

static int MaximumCameraY(const ActionBgLayerPlan *layer) {
  int maximum = layer->world_height > kActRaiserActionCameraViewportHeight
      ? (int)layer->world_height - kActRaiserActionCameraViewportHeight : 0;
  if (maximum < layer->camera_y) maximum = layer->camera_y;
  return maximum;
}

static bool BandsRemainOrderedAcrossCameraTravel(
    const ActionBgLayerPlan *layer,
    const ActionBgBand *preceding, const ActionBgBand *following) {
  if (preceding->anchor == following->anchor)
    return preceding->y1 <= following->y0;

  /* World rows move upward as camera_y increases. A World->Screen pair is
   * closest at camera zero; a Screen->World pair is closest at the maximum
   * reachable camera. Checking that limiting state proves the two half-open
   * intervals cannot cross later after a tuner draft has been accepted. */
  if (preceding->anchor == kActionBgBandAnchor_World) {
    const int preceding_y1_at_top = (int)preceding->y1 - 1;
    return preceding_y1_at_top <= following->y0;
  }
  const int following_y0_at_bottom =
      (int)following->y0 - MaximumCameraY(layer) - 1;
  return preceding->y1 <= following_y0_at_bottom;
}

bool ActionBgLayerPlan_Validate(const ActionBgLayerPlan *layer) {
  if (!layer || !layer->valid || !ValidRole(layer->role) ||
      !ValidSource(layer->source) ||
      (layer->wrap_world_x &&
       layer->source != kActionBgSource_WorldMap) ||
      !ValidEdge(layer->default_edge) ||
      !ValidMotion(layer->default_motion) ||
      !ValidHorizontalExtent(&layer->horizontal_extent, false) ||
      !ValidVerticalExtent(&layer->vertical_extent) ||
      layer->band_count > kActionBgMaxBands)
    return false;

  for (unsigned i = 0; i < layer->band_count; i++) {
    const ActionBgBand *band = &layer->bands[i];
    int y0 = 0, y1 = 0;
    if (!ValidEdge(band->edge) || !ValidMotion(band->motion) ||
        !ValidHorizontalExtent(&band->horizontal_extent, true) ||
        !ActionBgLayerPlan_ResolveBand(layer, i, &y0, &y1))
      return false;
    if (i && !BandsRemainOrderedAcrossCameraTravel(
                 layer, &layer->bands[i - 1], band))
      return false;
  }
  return true;
}

bool ActionBgPlan_Validate(const ActionBgPlan *plan) {
  if (!plan || !plan->valid) return false;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++)
    if (!ActionBgLayerPlan_Validate(&plan->layer[layer])) return false;
  return true;
}

bool ActionBgLayerPlan_ResolveBand(const ActionBgLayerPlan *layer,
                                  unsigned band,
                                  int *screen_y0, int *screen_y1) {
  if (screen_y0) *screen_y0 = 0;
  if (screen_y1) *screen_y1 = 0;
  if (!layer || !screen_y0 || !screen_y1 ||
      band >= layer->band_count || band >= kActionBgMaxBands)
    return false;
  const ActionBgBand *entry = &layer->bands[band];
  if (!ValidBandAnchor(entry->anchor) || entry->y0 >= entry->y1)
    return false;
  const uint16_t limit = entry->anchor == kActionBgBandAnchor_World
      ? layer->world_height : kActRaiserAuthenticHeight;
  if (entry->y1 > limit) return false;
  const int offset = entry->anchor == kActionBgBandAnchor_World
      ? (int)layer->camera_y + 1 : 0;
  *screen_y0 = (int)entry->y0 - offset;
  *screen_y1 = (int)entry->y1 - offset;
  return true;
}

static void ResolveValidatedBand(const ActionBgLayerPlan *layer,
                                 unsigned band,
                                 int *screen_y0, int *screen_y1) {
  const ActionBgBand *entry = &layer->bands[band];
  const int offset = entry->anchor == kActionBgBandAnchor_World
      ? (int)layer->camera_y + 1 : 0;
  *screen_y0 = (int)entry->y0 - offset;
  *screen_y1 = (int)entry->y1 - offset;
}

static const ActionBgBand *FindValidatedRowBand(
    const ActionBgLayerPlan *layer, int authentic_y) {
  /* A content family that reaches an authentic viewport edge continues into
   * that edge's synthetic presentation margin. This is deliberately derived
   * from the band's existing bounds: it adds no second flag or policy source,
   * and an internal band can never leak outside the authentic viewport. */
  for (unsigned i = 0; i < layer->band_count; i++) {
    const ActionBgBand *band = &layer->bands[i];
    int y0 = 0, y1 = 0;
    ResolveValidatedBand(layer, i, &y0, &y1);
    const bool owns_top_margin = authentic_y < 0 && y0 <= 0 && y1 > 0;
    const bool owns_bottom_margin =
        authentic_y >= kActRaiserAuthenticHeight &&
        y0 < kActRaiserAuthenticHeight &&
        y1 >= kActRaiserAuthenticHeight;
    if (owns_top_margin || owns_bottom_margin ||
        (authentic_y >= y0 && authentic_y < y1))
      return band;
  }
  return NULL;
}

void ActionBgLayerPlan_ResolveValidatedRow(const ActionBgLayerPlan *layer,
                                           int authentic_y,
                                           ActionBgRowPolicy *out) {
  ActionBgRowPolicy resolved = {
    .edge = layer->default_edge,
    .motion = layer->default_motion,
    .horizontal_extent = layer->horizontal_extent,
  };
  const ActionBgBand *band = FindValidatedRowBand(layer, authentic_y);
  if (band) {
    resolved.edge = band->edge;
    resolved.motion = band->motion;
    if (band->horizontal_extent.mode != kActionBgExtent_Inherit)
      resolved.horizontal_extent = band->horizontal_extent;
  }
  *out = resolved;
}

bool ActionBgLayerPlan_ResolveRow(const ActionBgLayerPlan *layer,
                                  int authentic_y,
                                  ActionBgRowPolicy *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!out || !ActionBgLayerPlan_Validate(layer)) return false;
  ActionBgLayerPlan_ResolveValidatedRow(layer, authentic_y, out);
  return true;
}

bool ActionBgPlan_CompilePresentation(
    const ActionBgPlan *plan, ActionBgPresentationPolicy *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!plan || !out || !ActionBgPlan_Validate(plan)) return false;

  ActionBgPresentationPolicy built = { 0 };
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    const ActionBgLayerPlan *layer_plan = &plan->layer[layer];
    if (!layer_plan->valid || layer_plan->band_count > kActionBgMaxBands)
      return false;
    const uint8_t mask = (uint8_t)(1u << layer);
    switch (layer_plan->default_edge) {
      case kActionBgEdge_Transparent:
      case kActionBgEdge_Clamp:  built.clamp_layers |= mask; break;
      case kActionBgEdge_Mirror: built.mirror_layers |= mask; break;
      case kActionBgEdge_Repeat: built.repeat_layers |= mask; break;
      case kActionBgEdge_LiveWorld:
      case kActionBgEdge_RawWrap:
        break;
      default:
        return false;
    }
    if (layer_plan->default_motion == kActionBgMotion_NormalScroll)
      built.normal_scroll_layers |= mask;
    for (unsigned band = 0; band < layer_plan->band_count; band++) {
      const ActionBgBand *entry = &layer_plan->bands[band];
      int y0 = 0, y1 = 0;
      if (!ActionBgLayerPlan_ResolveBand(layer_plan, band, &y0, &y1))
        return false;
      if (y0 < 0) y0 = 0;
      if (y1 > kActRaiserAuthenticHeight)
        y1 = kActRaiserAuthenticHeight;
      if (y0 >= y1) continue;
      if (built.band_count >= kActionBgPresentationBandMax) return false;
      built.bands[built.band_count++] = (ActionBgPresentationBand) {
        .layer = (uint8_t)layer,
        .y0 = (uint8_t)y0,
        .y1 = (uint8_t)y1,
        .edge = entry->edge,
        .motion = entry->motion,
      };
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
    SetLayerSource(&out->layer[layer], kActionBgSource_NativeTilemap);
    out->layer[layer].default_edge = kActionBgEdge_RawWrap;
    out->layer[layer].default_motion = kActionBgMotion_FillRelative;
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
  const uint8_t normal = policy->normal_scroll_layers & owned_mask;
  if ((clamp & mirror) || (clamp & repeat) || (mirror & repeat) ||
      policy->band_count > kActionBgPresentationBandMax)
    return false;
  uint8_t preceding_layer = 0;
  uint8_t preceding_y1 = 0;
  uint8_t layer_band_count[kActionBgPlanLayerCount] = { 0 };
  for (unsigned i = 0; i < policy->band_count; i++) {
    const ActionBgPresentationBand *band = &policy->bands[i];
    if (band->layer >= kActionBgPlanLayerCount || band->y0 >= band->y1 ||
        band->y1 > kActRaiserAuthenticHeight || !ValidEdge(band->edge) ||
        !ValidMotion(band->motion) ||
        (i && band->layer < preceding_layer) ||
        (i && band->layer == preceding_layer && band->y0 < preceding_y1))
      return false;
    if (++layer_band_count[band->layer] > kActionBgMaxBands) return false;
    preceding_layer = band->layer;
    preceding_y1 = band->y1;
  }

  ActionBgPlan built = *plan;
  built.bound_canvas_to_world = policy->bound_canvas_to_world;
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    ActionBgLayerPlan *layer_plan = &built.layer[layer];
    const uint8_t mask = (uint8_t)(1u << layer);
    layer_plan->valid = true;
    ClearBands(layer_plan);
    layer_plan->default_motion = normal & mask
        ? kActionBgMotion_NormalScroll
        : kActionBgMotion_FillRelative;
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
  for (unsigned i = 0; i < policy->band_count; i++) {
    const ActionBgPresentationBand *src = &policy->bands[i];
    ActionBgLayerPlan *layer_plan = &built.layer[src->layer];
    ActionBgBand *dst = &layer_plan->bands[layer_plan->band_count++];
    *dst = (ActionBgBand) {
      .y0 = src->y0,
      .y1 = src->y1,
      .edge = src->edge,
      .motion = src->motion,
      .anchor = kActionBgBandAnchor_Screen,
    };
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
    SetLayerSource(layer_plan, kActionBgSource_AuthenticViewport);
    layer_plan->default_edge = kActionBgEdge_Clamp;
    layer_plan->default_motion = kActionBgMotion_FillRelative;
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

const char *ActionBgMotionMode_Name(ActionBgMotionMode motion) {
  static const char *const names[] = { "fill", "normal" };
  return motion >= kActionBgMotion_FillRelative &&
      motion <= kActionBgMotion_NormalScroll ? names[motion] : "unknown";
}

const char *ActionBgBandAnchor_Name(ActionBgBandAnchor anchor) {
  static const char *const names[] = { "screen", "world" };
  return anchor >= kActionBgBandAnchor_Screen &&
      anchor <= kActionBgBandAnchor_World ? names[anchor] : "unknown";
}

const char *ActionBgExtentMode_Name(ActionBgExtentMode mode) {
  switch (mode) {
    case kActionBgExtent_Inherit: return "inherit";
    case kActionBgExtent_Available: return "available";
    case kActionBgExtent_Fixed: return "fixed";
    default: return "unknown";
  }
}
