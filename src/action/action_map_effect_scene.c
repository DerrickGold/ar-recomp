#include "action_environment_capture_internal.h"
#include "action_map_effect_sources.h"
#include "action_bg_plan.h"

typedef struct WallTorchMapRule {
  uint8_t top_metatile;
  uint8_t bottom_metatile;
  int8_t anchor_y;
  int8_t bottom_extent;
  bool requires_bottom;
  bool camera_bounded;
} WallTorchMapRule;

static bool WallTorchRuleFor(uint8_t group, uint8_t room, WallTorchMapRule *rule) {
  if (!rule) return false;
  if (group == kActRaiserMapGroup_Bloodpool) {
    *rule = (WallTorchMapRule) {
      .top_metatile = kBloodpoolTorchTopMetatile,
      .bottom_metatile = kBloodpoolTorchBottomMetatile,
      .anchor_y = 15,
      .bottom_extent = 2,
      .requires_bottom = true,
    };
    return true;
  }
  if ((group == kActRaiserMapGroup_Marahna && room >= kMarahnaFirstEffectMap && room <= kMarahnaLastEffectMap) ||
      (group ==
           kActRaiserMapGroup_Marahna &&
       room ==
           kMarahnaBossMap) ||
      (group ==
           kActRaiserMapGroup_DeathHeim &&
       room ==
           kDeathHeimViperMap)) {
    *rule = (WallTorchMapRule) {
      .top_metatile = kMarahnaTorchMetatile,
      .anchor_y = 11,
      .bottom_extent = 5,
      .camera_bounded = true,
    };
    return true;
  }
  return false;
}

typedef struct SceneBgScanBounds {
  unsigned x0, y0;
  unsigned x1, y1;  /* exclusive */
} SceneBgScanBounds;

static bool SceneBgScanBounds_InitWindow(
    SceneBgScanBounds *bounds, const ActionBgMapView *map,
    int camera_x, int camera_y, int margin_x, int margin_y,
    bool include_partial_cells) {
  if (!bounds || !map || !map->world_width || !map->world_height ||
      margin_x < 0 || margin_y < 0)
    return false;
  int min_x = camera_x - margin_x;
  int min_y = camera_y - margin_y;
  int max_x = camera_x + kActRaiserAuthenticWidth + margin_x;
  int max_y = camera_y + kActRaiserAuthenticHeight + margin_y;
  if (max_x < 0 || max_y < 0 || min_x >= (int)map->world_width ||
      min_y >= (int)map->world_height)
    return false;
  if (min_x < 0) min_x = 0;
  if (min_y < 0) min_y = 0;
  if (max_x >= (int)map->world_width) max_x = (int)map->world_width - 1;
  if (max_y >= (int)map->world_height) max_y = (int)map->world_height - 1;
  const unsigned cell = kActionBgMetatilePixels;
  const unsigned align_bias = include_partial_cells ? 0u : cell - 1u;
  bounds->x0 = ((unsigned)min_x + align_bias) / cell * cell;
  bounds->y0 = ((unsigned)min_y + align_bias) / cell * cell;
  bounds->x1 = ((unsigned)max_x / cell + 1u) * cell;
  bounds->y1 = ((unsigned)max_y / cell + 1u) * cell;
  if (bounds->x1 > map->world_width) bounds->x1 = map->world_width;
  if (bounds->y1 > map->world_height) bounds->y1 = map->world_height;
  return bounds->x0 < bounds->x1 && bounds->y0 < bounds->y1;
}

static bool SceneBgScanBounds_Init(SceneBgScanBounds *bounds,
                                   const ActionBgMapView *map,
                                   bool camera_bounded,
                                   int camera_x, int camera_y) {
  if (!bounds || !map || !map->world_width || !map->world_height)
    return false;
  if (!camera_bounded) {
    *bounds = (SceneBgScanBounds){
      .x1 = map->world_width,
      .y1 = map->world_height,
    };
    return true;
  }
  return SceneBgScanBounds_InitWindow(
      bounds, map, camera_x, camera_y,
      kActRaiserAuthenticWidth, kActRaiserAuthenticWidth, false);
}

static void CaptureWallTorches(ActionSceneEffectFrame *dst,
                               const ActionEnvironmentScene *scene) {
  WallTorchMapRule rule;
  if (!dst || !WallTorchRuleFor(scene->group,scene->room,&rule)) return;

  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_Init(&bounds, &map, rule.camera_bounded,
                              camera_x, camera_y))
    return;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      uint8_t top = 0, bottom = 0;
      if (!ActionBgMapView_LookupMetatile(&map, (int)x, (int)y, &top) ||
          top != rule.top_metatile)
        continue;
      if (rule.requires_bottom &&
          (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)(y + kActionBgMetatilePixels), &bottom) ||
           bottom != rule.bottom_metatile))
        continue;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      /* Every instance uses the same animated BG tiles, so its source flame
       * changes on one shared gameplay clock. The renderer owns the
       * deliberately faster visual response used by the added light and
       * embers; the observer clock keeps that response frozen with the source
       * BG throughout ActRaiser's native pause. */
      ActionEffectInstance effect = {
        .generation = 0x54000000u ^ identity,
        .pulse_generation = 0x74000000u ^ identity,
        .world_x = (int16_t)(x + 8),
        .world_y = (int16_t)(y + rule.anchor_y),
        .left_extent = 5,
        .top_extent = 9,
        .right_extent = 5,
        .bottom_extent = (uint16_t)rule.bottom_extent,
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_WallTorch,
        .phase = kActionEffectPhase_WallTorch,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_Bg1Plane,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-5.0f, -9.0f, 5.0f,
                        (float)rule.bottom_extent},
        },
      };
      SceneDecorationAppend(dst, &effect);
    }
  }
}

static void CaptureAitosLavaPits(ActionSceneEffectFrame *dst,
                                 const ActionEnvironmentScene *scene) {
  if (!dst || !(scene->group == kActRaiserMapGroup_Aitos && scene->room == kAitosLavaMap)) return;
  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_Init(&bounds, &map, true, camera_x, camera_y))
    return;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      uint8_t metatile = 0;
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)y, &metatile) ||
          metatile != kAitosLavaLeftMetatile)
        continue;

      unsigned middle_cells = 0;
      unsigned total_cells = 0;
      for (unsigned step = 1; step <= kAitosLavaMaxMiddleCells + 1;
           step++) {
        const unsigned cell_x = x + step * kActionBgMetatilePixels;
        if (cell_x < x ||
            !ActionBgMapView_LookupMetatile(
                &map, (int)cell_x, (int)y, &metatile))
          break;
        if (step <= kAitosLavaMaxMiddleCells &&
            metatile == kAitosLavaMiddleMetatile) {
          middle_cells++;
          continue;
        }
        if (middle_cells && metatile == kAitosLavaRightMetatile)
          total_cells = step + 1;
        break;
      }
      if (!total_cells) continue;

      bool bubbles_valid = true;
      unsigned bubble_rows = 1;
      static const uint8_t kBubbleRows[] = {
        kAitosLavaFillMetatile, kAitosLavaBubbleMetatile,
      };
      const bool has_second_bubble_row =
          y <= map.world_height - 3u * kActionBgMetatilePixels;
      if (has_second_bubble_row) bubble_rows++;
      for (unsigned row = 0; row < bubble_rows; row++) {
        for (unsigned cell = 0; cell < total_cells; cell++) {
          const unsigned cell_x = x + cell * kActionBgMetatilePixels;
          const unsigned cell_y = y + (row + 1u) * kActionBgMetatilePixels;
          if (cell_x < x || cell_y < y ||
              !ActionBgMapView_LookupMetatile(
                  &map, (int)cell_x, (int)cell_y, &metatile) ||
              metatile != kBubbleRows[row]) {
            bubbles_valid = false;
            break;
          }
        }
        if (!bubbles_valid) break;
      }
      if (!bubbles_valid) continue;

      const unsigned width = total_cells * kActionBgMetatilePixels;
      const unsigned height = bubble_rows * kActionBgMetatilePixels;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      const float half_width = (float)width * 0.5f;
      const float half_height = (float)height * 0.5f;
      ActionEffectInstance effect = {
        .generation = 0x4C000000u ^ identity,
        .pulse_generation = 0x6C000000u ^ identity,
        .world_x = (int16_t)(x + width / 2u),
        .world_y = (int16_t)(
            y + kActionBgMetatilePixels + height / 2u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = (uint16_t)(height / 2u),
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = (uint16_t)(height / 2u),
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_AitosLavaPit,
        .phase = kActionEffectPhase_AitosLavaPit,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -half_height,
                        half_width, half_height},
        },
      };
      SceneDecorationAppend(dst, &effect);
    }
  }
}

static bool IsAitosSideLavaAnimatedCell(uint8_t metatile) {
  return (metatile >= kAitosSideLavaFirstAnimatedMetatile &&
          metatile <= kAitosSideLavaLastAnimatedMetatile) ||
      metatile == kAitosSideLavaMap6AnimatedMetatile;
}

static bool IsAitosSideLavaBankPair(uint8_t left, uint8_t right) {
  return (left == 0x33 && right == 0x34) ||
      (left == 0x2C && right == 0x32) ||
      (left == 0x33 && right == 0x32);
}

/* Act 2 turns the isometric pit mouths into broad side-on lakes. Their exact
 * semantic is a maximal $01 lip run, one of the measured bank pairs, an
 * animated/transparent surface row immediately above, and lava body below.
 * Lighting is anchored to the lip rather than the red volume: using the full
 * lake depth as an emitter would put sparks hundreds of pixels underwater. */
static void CaptureAitosSideLavaReservoirs(
    ActionSceneEffectFrame *dst, const ActionEnvironmentScene *scene) {
  if (!dst || !(ActRaiserRoom_ProfileFor(scene->group,scene->room) == kActRaiserRoomProfile_AitosAct2Lava)) return;
  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_Init(&bounds, &map, true, camera_x, camera_y))
    return;

  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    if (y < kActionBgMetatilePixels ||
        y + kActionBgMetatilePixels >= map.world_height)
      continue;
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      if (x < kActionBgMetatilePixels) continue;
      uint8_t metatile = 0, left_bank = 0;
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)y, &metatile) ||
          metatile != kAitosSideLavaLipMetatile)
        continue;

      /* If the camera window begins inside a very wide lake, walk left to its
       * authentic bank once. Later in-window cells see a lip immediately to
       * their left and skip as duplicates. Without this, map $06's 640px lake
       * lost all heat whenever its left bank sat beyond the scan margin. */
      unsigned run_x = x;
      unsigned walked = 0;
      if (x == bounds.x0) {
        while (run_x >= 2u * kActionBgMetatilePixels &&
               walked < kAitosSideLavaMaxCells) {
          uint8_t previous = 0;
          if (!ActionBgMapView_LookupMetatile(
                  &map, (int)(run_x - kActionBgMetatilePixels),
                  (int)y, &previous) ||
              previous != kAitosSideLavaLipMetatile)
            break;
          run_x -= kActionBgMetatilePixels;
          walked++;
        }
        if (walked == kAitosSideLavaMaxCells) continue;
      }
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)(run_x - kActionBgMetatilePixels),
               (int)y, &left_bank) ||
          left_bank == kAitosSideLavaLipMetatile)
        continue;

      unsigned cells = 0;
      bool surface_valid = true;
      bool has_animated_surface = false;
      bool has_lava_body = false;
      for (; cells < kAitosSideLavaMaxCells; cells++) {
        const unsigned cell_x = run_x + cells * kActionBgMetatilePixels;
        if (cell_x < run_x || cell_x >= map.world_width ||
            !ActionBgMapView_LookupMetatile(
                &map, (int)cell_x, (int)y, &metatile) ||
            metatile != kAitosSideLavaLipMetatile)
          break;
        uint8_t surface = 0, body = 0;
        if (!ActionBgMapView_LookupMetatile(
                 &map, (int)cell_x,
                 (int)(y - kActionBgMetatilePixels), &surface) ||
            (surface != 0 && !IsAitosSideLavaAnimatedCell(surface)) ||
            !ActionBgMapView_LookupMetatile(
                 &map, (int)cell_x,
                 (int)(y + kActionBgMetatilePixels), &body)) {
          surface_valid = false;
          break;
        }
        has_animated_surface |= IsAitosSideLavaAnimatedCell(surface);
        has_lava_body |= body == kAitosSideLavaBodyMetatile;
      }
      if (!surface_valid || cells < 3 || !has_animated_surface ||
          !has_lava_body || cells == kAitosSideLavaMaxCells)
        continue;
      const unsigned right_x = run_x + cells * kActionBgMetatilePixels;
      uint8_t right_bank = 0;
      if (right_x < run_x || right_x >= map.world_width ||
          !ActionBgMapView_LookupMetatile(
              &map, (int)right_x, (int)y, &right_bank) ||
          !IsAitosSideLavaBankPair(left_bank, right_bank))
        continue;

      const unsigned width = cells * kActionBgMetatilePixels;
      const float half_width = (float)width * 0.5f;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(run_x / kActionBgMetatilePixels);
      ActionEffectInstance effect = {
        .generation = 0x4A000000u ^ identity,
        .pulse_generation = 0x6A000000u ^ identity,
        .world_x = (int16_t)(run_x + width / 2u),
        /* The visible orange lip occupies the top half of the $01 row. */
        .world_y = (int16_t)(y + 4u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = 4,
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = 4,
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_AitosLavaReservoir,
        .phase = kActionEffectPhase_AitosLavaReservoir,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_Bg1HighPlane,
        .projection_plane = kActionEffectProjectionPlane_Bg1High,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -4.0f, half_width, 4.0f},
        },
      };
      SceneDecorationAppend(dst, &effect);
    }
  }
}

static bool AitosSplashStructureWidth(
    const ActionBgMapView *map, unsigned x, unsigned y,
    unsigned *total_cells) {
  if (total_cells) *total_cells = 0;
  if (!map || !total_cells) return false;
  uint8_t tile = 0;
  if (!ActionBgMapView_LookupMetatile(map, (int)x, (int)y, &tile) ||
      tile != kAitosSplashTopLeft)
    return false;
  for (unsigned cells = 2; cells <= kAitosSplashMaxCells; cells++) {
    const unsigned right_x = x + (cells - 1u) * kActionBgMetatilePixels;
    if (right_x < x ||
        !ActionBgMapView_LookupMetatile(
            map, (int)right_x, (int)y, &tile))
      return false;
    if (tile == kAitosSplashTopMiddle) continue;
    if (tile != kAitosSplashTopRight) return false;
    static const uint8_t kLeft[] = {
      kAitosSplashBodyLeft, kAitosSplashDripLeft,
    };
    static const uint8_t kMiddle[] = {
      kAitosSplashBodyMiddle, kAitosSplashDripMiddle,
    };
    static const uint8_t kRight[] = {
      kAitosSplashBodyRight, kAitosSplashDripRight,
    };
    for (unsigned row = 0; row < 2; row++) {
      const unsigned row_y = y + (row + 1u) * kActionBgMetatilePixels;
      for (unsigned cell = 0; cell < cells; cell++) {
        const uint8_t expected = cell == 0 ? kLeft[row]
            : cell + 1u == cells ? kRight[row] : kMiddle[row];
        if (!ActionBgMapView_LookupMetatile(
                map, (int)(x + cell * kActionBgMetatilePixels),
                (int)row_y, &tile) || tile != expected)
          return false;
      }
    }
    *total_cells = cells;
    return true;
  }
  return false;
}

static void CaptureAitosWater(ActionSceneEffectFrame *dst,
                              const ActionEnvironmentScene *scene, ActionEnvironmentWaterfallTrace *trace) {
  if (!dst || !(ActRaiserRoom_ProfileFor(scene->group,scene->room) == kActRaiserRoomProfile_AitosWaterfall)) return;
  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  /* Cover the maximum wide side margin and Diorama vertical extension while
   * keeping the immutable scene payload bounded to structures that can
   * actually enter this presentation. */
  if (!SceneBgScanBounds_InitWindow(
          &bounds, &map, camera_x, camera_y, 128, 64, true))
    return;

  unsigned splash_count = 0;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      unsigned cells = 0;
      if (!AitosSplashStructureWidth(&map, x, y, &cells)) continue;
      const unsigned width = cells * kActionBgMetatilePixels;
      const float half_width = (float)width * 0.5f;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      ActionEffectInstance effect = {
        .generation = 0x57000000u ^ identity,
        .pulse_generation = 0x77000000u ^ identity,
        .world_x = (int16_t)(x + width / 2u),
        .world_y = (int16_t)(y + 16u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = 16,
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = 16,
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_AitosWaterSplash,
        .phase = kActionEffectPhase_AitosWaterSplash,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -16.0f, half_width, 16.0f},
        },
      };
      /* This stage's geometry budget remains fourteen splash structures even
       * when another environment needs more host decoration records. */
      if (splash_count >= 14) {
        dst->decoration_overflow = 1;
        dst->decoration_count = dst->decoration_visible_count = 0;
        return;
      }
      if (SceneDecorationAppend(dst, &effect)) splash_count++;
    }
  }
  if (trace) *trace = (ActionEnvironmentWaterfallTrace){
    .valid=true, .published=splash_count && !dst->decoration_overflow,
    .camera_x=camera_x, .camera_y=camera_y, .splash_count=splash_count,
    .x0=bounds.x0,.y0=bounds.y0,.x1=bounds.x1,.y1=bounds.y1};
  if (!splash_count || dst->decoration_overflow) {

    return;
  }


  /* BG2 uses the same decoded 512x512 map in the preceding dark cave, so the
   * camera-local presence of an exact splash structure is the live art
   * discriminator for the waterfall section. One broad BG2 record supplies
   * a restrained flow veil without replacing the source pixels. */
  const int bg2_camera_x =
      scene->camera_x[1];
  const int bg2_camera_y =
      scene->camera_y[1];
  const uint32_t map_identity = scene->room;
  ActionEffectInstance waterfall = {
    .generation = 0x57540000u ^ map_identity,
    .pulse_generation = 0x77540000u ^ map_identity,
    .world_x = (int16_t)(bg2_camera_x + 128),
    .world_y = (int16_t)(bg2_camera_y + 112),
    .left_extent = 256,
    .top_extent = 176,
    .right_extent = 256,
    .bottom_extent = 312,
    .age_ticks = scene->clock,
    .phase_ticks = scene->clock,
    .pulse_ticks = scene->clock,
    .kind = kActionEffect_AitosWaterfall,
    .phase = kActionEffectPhase_AitosWaterfallFlow,
    .role = kActionEffectRole_Body,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      /* Continue the veil through one 224-row overflow repeat. The lower
       * bound is chosen so the particle field ends at screen row 448 after
       * its 24px travel margin: authentic bottom 224 + one repeat 224. */
      .data.rect = {-256.0f, -176.0f, 256.0f, 312.0f},
    },
  };
  if (!SceneDecorationAppend(dst, &waterfall)) return;

  /* `$04/$02` intentionally keeps BG2's vertical extension short: allowing
   * more raw-wrap rows repeats water into non-water areas. Anchor the
   * after-BG2 foam/mist at the END of those safe rows, not at authentic row
   * 224. The latter was technically submitted but faded out before reaching
   * the black gap. It uses BG2's camera/shape but not its winner pixels; later
   * BG1 and OBJ planes remain in front. */
  ActionEffectInstance mist = waterfall;
  mist.generation = 0x575D0000u ^ map_identity;
  mist.pulse_generation = 0x775D0000u ^ map_identity;
  mist.world_y = (int16_t)(
      bg2_camera_y + kActRaiserAuthenticHeight +
      kActionBgAitosWaterfallBottomExtensionPixels);
  mist.top_extent = 64;
  mist.bottom_extent = 152;
  mist.kind = kActionEffect_AitosWaterfallMist;
  mist.phase = kActionEffectPhase_AitosWaterfallMist;
  mist.render_layer = kActionEffectRenderLayer_Atmosphere;
  mist.geometry.data.rect =
      (ActionEffectLocalRect){-256.0f, -64.0f, 256.0f, 152.0f};
  SceneDecorationAppend(dst, &mist);
}


bool ActionMapEnvironmentScene_UsesTorches(uint8_t group, uint8_t room) {
  WallTorchMapRule rule;
  return WallTorchRuleFor(group,room,&rule);
}
void ActionMapEnvironmentScene_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *dst, ActionEnvironmentWaterfallTrace *trace) {
  if (!scene || !dst) return;
  CaptureWallTorches(dst,scene);
  CaptureAitosLavaPits(dst,scene);
  CaptureAitosSideLavaReservoirs(dst,scene);
  CaptureAitosWater(dst,scene,trace);
}
