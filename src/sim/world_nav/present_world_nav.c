#include "sim/world_nav/present_world_nav.h"
/* Standalone world-navigation composition over the shared authored town
 * models and portable globe terrain/art. Camera-dependent memoization lives
 * here; model compilation/cache policy stays in the portable SIM modules.
 * All drawing crosses the renderer/depth-pass interfaces, never a backend.
 *
 * This file owns the shared resources (declared in present_world_nav_internal.h),
 * projection and scene orchestration. Terrain, ground, models and weather live
 * in present_world_nav_{terrain,ground,models,weather}.c, and the continuous
 * SIM globe in present_sim_globe.c.
 *
 * Captured state is the input contract for the present family: no g_ppu, no
 * g_settings, no Settings_Visible*(). State arrives via the `const FrameSlot *`. */
#include "sim/world_nav/present_world_nav_internal.h"

/* Presentation choices, not native coordinates or renderer settings. Keep
 * the local tile/relief scale while giving navigation a broader landscape
 * and the Palace backdrop its separately art-directed horizon. */
float WorldNavigationChartRadius(const FrameSlot *slot) {
  return kSimWorldNavigationGlobeRadiusTiles *
      (slot->sim.view == kSimView_SkyPalace ? 3.0f : 2.0f);
}

WorldNavigationArtState g_world_nav_art;

WorldNavigationMountainState g_world_nav_mountains;

WorldNavigationModelState g_world_nav_models = {.detail = -1, .style = -1};

WorldNavigationTerrainState g_world_nav_terrain;

/* Ground and native mountain cutouts are separate materials in one bounded
 * mesh. Their projected source is immutable on a held view; animated atlas
 * pixels and weather samples are deliberately not part of this cache. */
WorldNavigationSurfaceState g_world_nav_surfaces;
WorldNavigationGpuGridState g_world_nav_gpu_grid;

/* One presentation-owned fork/join group for immutable geometry math. It is
 * independent of weather enablement; jobs never overlap or own resources. */
static HostParallelWork *s_world_workers;
static bool s_world_workers_attempted;

HostParallelWork *WorldNavigationWorkers(void) {
  if (!s_world_workers_attempted) {
    s_world_workers_attempted = true;
    s_world_workers = HostParallelWork_Create(3);
  }
  return s_world_workers;
}

WorldNavigationShellState g_world_nav_shells;

WorldNavigationWeatherState g_world_nav_weather;

#if AR_WORLD_NAV_CACHE_TESTING
size_t g_world_nav_model_test_bytes = 8 * 1024 * 1024,
       g_world_nav_receiver_test_bytes = 4 * 1024 * 1024;
void PresentWorldNav_TestCacheBudgets(size_t model_bytes, size_t receiver_bytes) {
  g_world_nav_model_test_bytes = model_bytes;
  g_world_nav_receiver_test_bytes = receiver_bytes;
}
WorldNavigationCacheTestState PresentWorldNav_TestCacheState(void) {
  return (WorldNavigationCacheTestState){ g_world_nav_models.projection_unavailable,
                                          g_world_nav_weather.receivers_unavailable,
                                          g_world_nav_models.projected_count,
                                          g_world_nav_weather.receiver_count };
}
#endif

static void DestroyWorldNavigationMountainProjection(void) {
  free(g_world_nav_mountains.projection);
  g_world_nav_mountains.projection = NULL;
  g_world_nav_mountains.projection_capacity = 0;
  g_world_nav_mountains.samples_ready = false;
  g_world_nav_mountains.projection_ready = false;
  g_world_nav_mountains.projection_unavailable = false;
}

/* The publication key certifies both the retained CPU pixels and the GPU
 * atlas. Invalidating it does not discard usable old GPU resources, but
 * prevents a cache hit or an incremental patch until a full upload succeeds. */
static void InvalidateWorldNavigationArtPublication(void) {
  ++g_world_nav_art.image_revision;
  g_world_nav_art.serial = 0;
  Sim3DDepthPass_DestroyAtlasCache(g_world_nav_art.atlas_cache);
  g_world_nav_art.atlas_cache = NULL;
}

static void RefreshWorldNavigationMountainGeometry(void) {
  ++g_world_nav_mountains.geometry_revision;
  const bool joined = g_world_nav_mountains.transition_ready &&
      g_world_nav_mountains.relief_pct > 0;
  SimWorldNavigationTerrain_SetMountainJoin(
      joined ? g_world_nav_mountains.transition.join_rise : NULL,
      joined ? g_world_nav_mountains.transition.join_weight : NULL,
      joined ? 100.0f / g_world_nav_mountains.relief_pct : 0);
  const bool limited = joined && g_world_nav_mountains.transition.continuation_anchor_count > 0;
  SimWorldNavigationTerrain_SetMountainContinuationLimit(
      limited ? g_world_nav_mountains.transition.continuation_rise : NULL,
      limited ? g_world_nav_mountains.transition.continuation_weight : NULL,
      limited ? 100.0f / g_world_nav_mountains.relief_pct : 0);
  g_world_nav_terrain.ready = false;
  g_world_nav_terrain.cliffs_ready = false;
  g_world_nav_terrain.projection_ready = false;
  g_world_nav_mountains.samples_ready = false;
  g_world_nav_mountains.projection_ready = false;
  g_world_nav_mountains.projection_unavailable = false;
}

static void InvalidateWorldNavigationMountainSurface(void) {
  SimWorldNavigationTerrain_SetMountainReplacement(
      g_world_nav_mountains.active
          ? g_world_nav_mountains.scene.replacement : NULL);
  SimWorldNavigationTerrain_SetMountainTransition(
      g_world_nav_mountains.transition_ready
          ? g_world_nav_mountains.transition.ridge_scale : NULL);
  RefreshWorldNavigationMountainGeometry();
  InvalidateWorldNavigationArtPublication();
}

static void RebuildWorldNavigationMountainTransition(
    const SimWorldNavigationTownGround *ground, float radius_tiles) {
  SimWorldNavigationMountainTransition_Destroy(&g_world_nav_mountains.transition);
  g_world_nav_mountains.transition_serial = SimWorldMap_GeographySerial();
  g_world_nav_mountains.chart_radius_tiles = radius_tiles;
  g_world_nav_mountains.transition_ready = g_world_nav_mountains.active &&
      SimWorldNavigationMountainTransition_BuildAtRadius(&g_world_nav_mountains.scene,
          ground, radius_tiles, &g_world_nav_mountains.transition);
  InvalidateWorldNavigationMountainSurface();
}

static bool WorldNavigationMountainsEnabled(const FrameSlot *slot) {
  return slot->sim.world_navigation_mountains &&
      slot->sim.world_navigation_relief && slot->sim.landscape_height_pct &&
      !g_world_nav_art.unavailable && SimTownGroundArt_Available();
}

static void UpdateWorldNavigationLava(const FrameSlot *slot) {
  if (!g_world_nav_mountains.active) return;
  SimWorldNavigationMountainAtlasUpdate update;
  if (SimWorldNavigationMountains_UpdateLava(&g_world_nav_mountains.scene,
          slot->sim.game_frame, &update))
    g_world_nav_mountains.lava_upload = update;
  if (!g_world_nav_mountains.lava_upload.width) return;
  const SimWorldNavigationMountainAtlasUpdate *pending = &g_world_nav_mountains.lava_upload;
  const ArRenderRectI region = {pending->x, pending->y, pending->width, pending->height};
  const int width = kSimWorldNavigationMountainAtlasPixels;
  const Sim3DPerformanceScope transfer = Sim3DPerformance_Begin(kSim3DPerformance_WorldTransfer);
  const bool uploaded = Sim3DDepthPass_UploadAtlasRegions(&g_render_device,
      kSim3DDepthPass_WorldMountain, g_world_nav_mountains.scene.atlas,
      width, width, width * (int)sizeof(uint32_t), &region, 1);
  Sim3DPerformance_End(transfer);
  /* A failed upload leaves the last GPU palette usable. Retain the dirty
   * rectangle for retry, even if the clock freezes or reverses next frame. */
  if (uploaded) {
    ++g_world_nav_mountains.atlas_revision;
    Sim3DPerformance_AddUpload((uint64_t)region.w * region.h * sizeof(uint32_t));
    g_world_nav_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  }
}

static void EnsureWorldNavigationMountains(const FrameSlot *slot, float radius_tiles) {
  const bool relief_changed = g_world_nav_mountains.relief_pct != slot->sim.landscape_height_pct;
  g_world_nav_mountains.relief_pct = slot->sim.landscape_height_pct;
  if (!WorldNavigationMountainsEnabled(slot)) {
    g_world_nav_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
    if (g_world_nav_mountains.ready || g_world_nav_mountains.active) {
      DestroyWorldNavigationMountainProjection();
      SimWorldNavigationMountains_Destroy(&g_world_nav_mountains.scene);
      g_world_nav_mountains.ready = false;
      g_world_nav_mountains.active = false;
      RebuildWorldNavigationMountainTransition(NULL, radius_tiles);
    }
    return;
  }
  const SimWorldNavigationTownGround *ground = &slot->sim.world_navigation_towns.ground;
  /* Curvature changes only when switching view families, not when selecting
   * another town. Rebuild from native art here so transition colors never
   * inherit an animated lava palette; keep the shared compiled model cache. */
  if (g_world_nav_mountains.ready &&
      g_world_nav_mountains.chart_radius_tiles == radius_tiles &&
      g_world_nav_mountains.developed == SimWorldMap_DevelopedAvailable() &&
      g_world_nav_mountains.transition_serial == SimWorldMap_GeographySerial() && !memcmp(
          ground, &g_world_nav_mountains.sources, sizeof(*ground))) {
    if (relief_changed)
      RefreshWorldNavigationMountainGeometry();
    UpdateWorldNavigationLava(slot);
    return;
  }
  g_world_nav_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  g_world_nav_mountains.sources = *ground;
  g_world_nav_mountains.developed = SimWorldMap_DevelopedAvailable();
  g_world_nav_mountains.ready = true;
  g_world_nav_mountains.active = SimWorldNavigationMountains_Build(
      ground, &g_world_nav_mountains.scene) && g_world_nav_mountains.scene.face_count;
  if (g_world_nav_mountains.active) {
    /* Optional completion of clipped stamps depends on exterior rock, so
     * geography (but not animated water) participates in the source key. */
    (void)SimWorldNavigationMountains_ContinueEdges(ground, &g_world_nav_mountains.scene);
    /* Ground blending derives its rock tint from the immutable source art,
     * never from whichever lava pulse happened to trigger a cold rebuild. */
    RebuildWorldNavigationMountainTransition(ground, radius_tiles);
    SimWorldNavigationMountainAtlasUpdate initial_palette;
    (void)SimWorldNavigationMountains_UpdateLava(&g_world_nav_mountains.scene,
        slot->sim.game_frame, &initial_palette);
    const int width = kSimWorldNavigationMountainAtlasPixels;
    const ArRenderRectI region = {0, 0, width, width};
    if (!Sim3DDepthPass_UploadAtlasRegions(
            &g_render_device, kSim3DDepthPass_WorldMountain,
            g_world_nav_mountains.scene.atlas, width, width,
            width * (int)sizeof(uint32_t), &region, 1)) {
      g_world_nav_mountains.active = false;
      SimWorldNavigationMountains_Destroy(&g_world_nav_mountains.scene);
      RebuildWorldNavigationMountainTransition(NULL, radius_tiles);
    } else {
      Sim3DPerformance_AddUpload((uint64_t)width * width * sizeof(uint32_t));
    }
  } else {
    RebuildWorldNavigationMountainTransition(ground, radius_tiles);
  }
}

static void ResetWorldNavigationMountains(void) {
  DestroyWorldNavigationMountainProjection();
  SimWorldNavigationMountains_Destroy(&g_world_nav_mountains.scene);
  SimWorldNavigationMountainTransition_Destroy(&g_world_nav_mountains.transition);
  g_world_nav_mountains.transition_ready = false;
  g_world_nav_mountains.transition_serial = 0;
  g_world_nav_mountains.relief_pct = 0;
  g_world_nav_mountains.ready = false;
  g_world_nav_mountains.active = false;
  g_world_nav_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  g_world_nav_mountains.developed = false;
}


static bool WorldNavigationAnimationBlockChanged(
    const SimWorldNavigationArtChanges *changes, int x, int y, int step) {
  for (int dy = 0; dy < step; dy++)
    for (int dx = 0; dx < step; dx++)
      if (changes->cells[(y + dy) * kSimWorldMapTiles + x + dx]) return true;
  return false;
}

static bool UploadWorldNavigationAnimation(const SimWorldNavigationArtChanges *changes) {
  enum { kMaxRegions = 256, kCell = kSimTownCellPixels,
         kWidth = kSimWorldNavigationArtPixels };
  ArRenderRectI regions[kMaxRegions];
  int count, step = 1;
collect:
  count = 0;
  for (int y = 0; y < kSimWorldMapTiles; y += step) {
    for (int x = 0; x < kSimWorldMapTiles;) {
      if (!WorldNavigationAnimationBlockChanged(changes, x, y, step)) { x += step; continue; }
      const int start = x;
      x += step;
      while (x < kSimWorldMapTiles && WorldNavigationAnimationBlockChanged(changes, x, y, step))
        x += step;
      const ArRenderRectI span = {start * kCell, y * kCell,
                                 (x - start) * kCell, step * kCell};
      int i = 0;
      for (; i < count; i++)
        if (regions[i].x == span.x && regions[i].w == span.w &&
            regions[i].y + regions[i].h == span.y) {
          regions[i].h += step * kCell;
          break;
        }
      if (i != count) continue;
      /* Numerous small shores use coarser upload regions, never coarser art.
       * The 16x16 block grid has at most 128 horizontal runs; copying retained
       * unchanged texels between them is exact and bounds submission work. */
      if (count == kMaxRegions) {
        step = 8;
        goto collect;
      }
      regions[count++] = span;
    }
  }
  if (!count) return true;
  const Sim3DPerformanceScope transfer = Sim3DPerformance_Begin(kSim3DPerformance_WorldTransfer);
  const bool uploaded = Sim3DDepthPass_UploadAtlasRegions(&g_render_device, kSim3DDepthPass_Ground,
          g_world_nav_art.pixels, kWidth, kWidth, kWidth * (int)sizeof(uint32_t),
          regions, count);
  Sim3DPerformance_End(transfer);
  if (!uploaded) return false;
  uint64_t bytes = 0;
  for (int i = 0; i < count; i++) bytes += (uint64_t)regions[i].w * regions[i].h * sizeof(uint32_t);
  Sim3DPerformance_AddUpload(bytes);
  return true;
}

static void RebuildWorldNavigationAnimationRange(void *context, size_t first, size_t end) {
  SimWorldNavigationArt_RenderAnimationRows(context, first, end);
}

static bool UpdateWorldNavigationAnimation(const uint32_t *developed,
    const uint8_t *world_cells, const SimWorldNavigationTownGround *ground,
    bool detailed, bool models, uint8_t phase, SimWorldNavigationArtChanges *changes) {
  if (!g_world_nav_art.animation && !g_world_nav_art.animation_unavailable) {
    g_world_nav_art.animation = malloc(sizeof(*g_world_nav_art.animation));
    g_world_nav_art.animation_unavailable = !g_world_nav_art.animation;
  }
  if (!g_world_nav_art.animation)
    return SimWorldNavigationArt_UpdateAnimation(
        g_world_nav_art.pixels, kSimWorldNavigationArtPixels, developed, kSimWorldMapPixels,
        world_cells, ground, detailed, models,
        g_world_nav_terrain.cliffs.town_mask != 0, g_world_nav_art.phase, phase, changes);
  SimWorldNavigationArtAnimation *work = g_world_nav_art.animation;
  if (!SimWorldNavigationArt_PrepareAnimation(work,
          g_world_nav_art.pixels, kSimWorldNavigationArtPixels,
          developed, kSimWorldMapPixels,
          world_cells, ground, detailed, models, g_world_nav_terrain.cliffs.town_mask != 0,
          g_world_nav_art.phase, phase)) return false;
  HostParallelWork_Run(WorldNavigationWorkers(), kSimWorldMapTiles, 16,
      RebuildWorldNavigationAnimationRange, work);
  *changes = work->changes;
  /* The reusable storage must not advertise borrowed inputs between frames. */
  work->ready = false;
  return true;
}

static bool WorldNavigationArtVersion(const FrameSlot *slot, uint8_t phase,
    unsigned *version) {
  _Static_assert(kWorldWaterFrameCount * kSimTownGroundAnimationFrames <=
      kSim3DDepthAtlasVersionLimit, "Ground animation fits bounded GPU snapshots");
  uint8_t water;
  if (g_world_nav_art.atlas_cache_unavailable || g_world_nav_art.unavailable ||
      !WorldNavigationGpuGridEnabled() ||
      slot->sim.underlay_serial != SimWorldMap_Serial() ||
      !SimWorldMap_WaterAnimationFrame(&water)) return false;
  *version = water * kSimTownGroundAnimationFrames + phase;
  return true;
}

static void CaptureWorldNavigationArtVersion(const FrameSlot *slot, uint8_t phase) {
  unsigned version;
  if (!WorldNavigationArtVersion(slot, phase, &version)) return;
  if (!g_world_nav_art.atlas_cache)
    g_world_nav_art.atlas_cache = Sim3DDepthPass_CreateAtlasCache();
  if (g_world_nav_art.atlas_cache &&
      Sim3DDepthPass_CaptureAtlasVersion(g_world_nav_art.atlas_cache, version)) return;
  Sim3DDepthPass_DestroyAtlasCache(g_world_nav_art.atlas_cache);
  g_world_nav_art.atlas_cache = NULL;
  g_world_nav_art.atlas_cache_unavailable = true;
  fprintf(stderr, "[world-navigation] GPU ground snapshots unavailable; using mutable atlas\n");
}

static bool EnsureWorldNavigationArt(const FrameSlot *slot, float radius_tiles) {
  g_world_nav_art.displayed_version = -1;
  if (!slot || !slot->sim.underlay_serial) return false;
  const bool detailed = slot->sim.world_navigation_ground_detail != 0;
  const bool models = slot->sim.world_navigation_models &&
      slot->sim.background_voxel_enabled;
  const SimWorldNavigationTownGround *ground =
      &slot->sim.world_navigation_towns.ground;
  const uint8_t phase =
      (detailed || models) && ground->enabled_town_mask && !g_world_nav_art.unavailable
          ? SimTownGroundArt_AnimationPhase(slot->sim.game_frame) : 0;
  const bool same_style =
      g_world_nav_art.cliffs == (g_world_nav_terrain.cliffs.town_mask != 0) &&
      g_world_nav_art.detailed == detailed &&
      g_world_nav_art.models == models &&
      (!(detailed || models || g_world_nav_mountains.active) ||
       !memcmp(&g_world_nav_art.sources,
                            ground, sizeof(*ground)));
  const bool same_image = g_world_nav_art.serial == slot->sim.underlay_serial;
  unsigned version;
  if (same_style && g_world_nav_art.serial &&
      g_world_nav_art.geography == SimWorldMap_GeographySerial() &&
      WorldNavigationArtVersion(slot, phase, &version) && g_world_nav_art.atlas_cache &&
      Sim3DDepthPass_HasAtlasVersion(g_world_nav_art.atlas_cache, version)) {
    /* These keys certify CPU pixels AND the mutable atlas, not the image
     * selected for display. Never advance them when only a snapshot is used. */
    Sim3DPerformance_AddAtlasReuse();
    g_world_nav_art.displayed_version = (int)version;
    return true;
  }
  if (same_style && same_image && g_world_nav_art.phase == phase) {
    CaptureWorldNavigationArtVersion(slot, phase);
    return true;
  }
  const uint32_t *developed = SimWorldMap_BakedPixels();
  if (!developed) return false;
  if (same_style && g_world_nav_art.serial &&
      g_world_nav_art.geography == SimWorldMap_GeographySerial() &&
      slot->sim.underlay_serial == SimWorldMap_Serial() && !g_world_nav_art.unavailable &&
      g_world_nav_art.pixels) {
    SimWorldNavigationArtChanges changes;
    uint8_t world_cells[kSimWorldMapBytes];
    const Sim3DPerformanceScope animation =
        Sim3DPerformance_Begin(kSim3DPerformance_WorldAnimation);
    const bool world_ready = same_image || SimWorldMap_WaterAnimationCells(world_cells);
    const bool updated = world_ready && UpdateWorldNavigationAnimation(developed,
            same_image ? NULL : world_cells, detailed || models ? ground : NULL,
            detailed, models, phase, &changes) &&
        (!g_world_nav_mountains.active || SimWorldNavigationMountains_ClearGround(
            g_world_nav_art.pixels, kSimWorldNavigationArtPixels, ground,
            g_world_nav_mountains.scene.town_mask, changes.cells)) &&
        (!g_world_nav_mountains.transition_ready || SimWorldNavigationMountainTransition_Apply(
            &g_world_nav_mountains.transition, g_world_nav_art.pixels,
            kSimWorldNavigationArtPixels, changes.cells));
    Sim3DPerformance_End(animation);
    if (updated) {
      if (!UploadWorldNavigationAnimation(&changes)) {
        /* CPU pixels advanced but the GPU transaction did not. Reconstruct
         * and fully upload on retry, even if the simulation clock rewinds. */
        InvalidateWorldNavigationArtPublication();
        return false;
      }
      g_world_nav_art.phase = phase;
      g_world_nav_art.serial = slot->sim.underlay_serial;
      ++g_world_nav_art.image_revision;
      CaptureWorldNavigationArtVersion(slot, phase);
      return true;
    }
  }
  /* A full rebuild overwrites the CPU image before upload. Even if a failed
   * upload is followed by a return to the last GPU style/phase, that old key
   * must not certify the newly mutated CPU pixels for animation patches. */
  InvalidateWorldNavigationArtPublication();
  const uint32_t *pixels = developed;
  int width = kSimWorldMapPixels;
  if (!g_world_nav_art.unavailable) {
    if (!g_world_nav_art.pixels)
      g_world_nav_art.pixels = malloc(
          (size_t)kSimWorldNavigationArtPixels * kSimWorldNavigationArtPixels *
          sizeof(uint32_t));
    if (g_world_nav_art.pixels &&
        SimWorldNavigationArt_Build(
            g_world_nav_art.pixels, kSimWorldNavigationArtPixels,
            developed, kSimWorldMapPixels)) {
      pixels = g_world_nav_art.pixels;
      width = kSimWorldNavigationArtPixels;
      if (detailed || models)
        SimWorldNavigationArt_OverlayTownGround(
            g_world_nav_art.pixels, width, ground, detailed, models,
            g_world_nav_terrain.cliffs.town_mask != 0, phase);
      if (g_world_nav_mountains.active &&
          !SimWorldNavigationMountains_ClearGround(
              g_world_nav_art.pixels, width, ground,
              g_world_nav_mountains.scene.town_mask, NULL)) return false;
      if (g_world_nav_mountains.transition_ready &&
          !SimWorldNavigationMountainTransition_Apply(
              &g_world_nav_mountains.transition, g_world_nav_art.pixels, width, NULL))
        return false;
    } else {
      g_world_nav_art.unavailable = true;
      fprintf(stderr, "[world-navigation] using native-resolution world art\n");
      /* Old ground must keep its inferred slopes if native cleanup failed. */
      EnsureWorldNavigationMountains(slot,radius_tiles);
    }
  }
  const ArRenderRectI region = {0, 0, width, width};
  const Sim3DPerformanceScope transfer = Sim3DPerformance_Begin(kSim3DPerformance_WorldTransfer);
  const bool uploaded = Sim3DDepthPass_UploadAtlasRegions(
          &g_render_device, kSim3DDepthPass_Ground, pixels,
          width, width, width * (int)sizeof(uint32_t), &region, 1);
  Sim3DPerformance_End(transfer);
  if (!uploaded) return false;
  Sim3DPerformance_AddUpload((uint64_t)width * width * sizeof(uint32_t));
  g_world_nav_art.serial = slot->sim.underlay_serial;
  g_world_nav_art.geography = SimWorldMap_GeographySerial();
  g_world_nav_art.detailed = detailed;
  g_world_nav_art.models = models;
  g_world_nav_art.phase = phase;
  g_world_nav_art.cliffs = g_world_nav_terrain.cliffs.town_mask != 0;
  if (detailed || models || g_world_nav_mountains.active)
    g_world_nav_art.sources = *ground;
  CaptureWorldNavigationArtVersion(slot, phase);
  return true;
}

bool EnsureWorldNavigationBlur(const FrameSlot *slot) {
  if (g_world_nav_art.blur_serial == slot->sim.underlay_serial) return true;
  enum { kBlurDivisor = 4, kBlurPixels = kSimWorldMapPixels / kBlurDivisor };
  static uint32_t pixels[kBlurPixels * kBlurPixels];
  const ArRenderRectI region = {0, 0, kBlurPixels, kBlurPixels};
  if (!SimWorldMap_Downsample(pixels, kBlurPixels, kBlurDivisor) ||
      !Sim3DDepthPass_UploadAtlasRegions(
          &g_render_device, kSim3DDepthPass_GroundBlur, pixels,
          kBlurPixels, kBlurPixels, kBlurPixels * (int)sizeof(uint32_t), &region, 1))
    return false;
  g_world_nav_art.blur_serial = slot->sim.underlay_serial;
  return true;
}

/* $09 full-world presentation.
 *
 * Navigation uses the same developed texture cache as the town underlay, but
 * none of the underlay geometry. Its captured Mode-7 matrix supplies focus,
 * zoom and in-plane rotation before the shared town perspective is applied. */
static void ResetWorldNavigationArt(void) {
  free(g_world_nav_art.animation);
  g_world_nav_art.animation = NULL;
  g_world_nav_art.animation_unavailable = false;
  InvalidateWorldNavigationArtPublication();
  g_world_nav_art.blur_serial = 0;
  g_world_nav_art.geography = 0;
  g_world_nav_art.unavailable = false;
  g_world_nav_art.atlas_cache_unavailable = false;
  free(g_world_nav_art.pixels);
  g_world_nav_art.pixels = NULL;
}

static bool WorldNavigationFlatOutputPoint(
    const FrameSlot *slot, ArRenderRectI viewport,
    float source_x, float source_y, ArRenderPointF *out) {
  float authentic_x = 0.0f, authentic_y = 0.0f;
  if (!out || !SimWorldNavigationScene_ProjectSource(
          &slot->sim.world_navigation_scene, source_x, source_y,
          &authentic_x, &authentic_y))
    return false;
  *out = WorldNavigationComposition_ProjectPoint(
      slot, viewport, authentic_x, authentic_y);
  return true;
}

static bool WorldNavigationInspecting(const FrameSlot *slot) {
  return slot->sim_camera.orbit_yaw != 0 || slot->sim_camera.orbit_pitch != 0;
}

static float WorldNavigationModelHeightBound(const FrameSlot *slot) {
  if (!slot->sim.world_navigation_models || !slot->sim.background_voxel_enabled)
    return 0.0f;
  const SimWorldNavigationTowns *towns = &slot->sim.world_navigation_towns;
  if (towns->overflow || towns->object_count > kSimWorldNavigationTownObjectCapacity)
    return 0.0f; /* The draw path rejects invalid captures. */
  const bool same_bounds_basis =
      g_world_nav_models.detail == slot->sim.background_voxel_detail &&
      g_world_nav_models.style == slot->sim.background_voxel_style &&
      g_world_nav_models.chart_radius_tiles == WorldNavigationChartRadius(slot);
  if (same_bounds_basis && g_world_nav_models.object_count == towns->object_count &&
      !memcmp(g_world_nav_models.objects, towns->objects,
          towns->object_count * sizeof(towns->objects[0])))
    return g_world_nav_models.maximum_rise;
  float maximum = 0.0f;
  for (uint16_t i = 0; i < towns->object_count; i++) {
    const SimWorldNavigationTownObject *object = &towns->objects[i];
    if (!same_bounds_basis || i >= g_world_nav_models.object_count ||
        memcmp(&g_world_nav_models.objects[i], object, sizeof(*object))) {
      const SimBackgroundVoxelProportions *proportions =
          SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object->kind);
      SimBackgroundVoxelModelBounds measured;
      WorldNavigationModelBounds *bound = &g_world_nav_models.bounds[i];
      *bound = (WorldNavigationModelBounds){0};
      if (SimBackgroundVoxelModel_MeasureBounds(
          object, (SimBackgroundVoxelDetail)slot->sim.background_voxel_detail,
          (SimBackgroundVoxelStyle)slot->sim.background_voxel_style, &measured)) {
        const SimBackgroundBridgeBounds footprint = WorldNavigationObjectBounds(object);
        const float extent_x = fmaxf(fabsf(measured.min_x - footprint.width * .5f),
            fabsf(measured.max_x - footprint.width * .5f));
        const float extent_y = fmaxf(fabsf(measured.min_y - footprint.depth * .5f),
            fabsf(measured.max_y - footprint.depth * .5f));
        bound->minimum_rise = measured.min_z * proportions->height_scale / kSimTownCellPixels;
        bound->maximum_rise = measured.max_z * proportions->height_scale / kSimTownCellPixels;
        bound->angular_radius = hypotf(extent_x, extent_y) * proportions->footprint_scale /
            (kSimTownCellPixels * WorldNavigationChartRadius(slot));
      }
      memcpy(&g_world_nav_models.objects[i], object, sizeof(*object));
    }
    maximum = fmaxf(maximum, g_world_nav_models.bounds[i].maximum_rise);
  }
  g_world_nav_models.object_count = towns->object_count;
  g_world_nav_models.detail = slot->sim.background_voxel_detail;
  g_world_nav_models.style = slot->sim.background_voxel_style;
  g_world_nav_models.chart_radius_tiles = WorldNavigationChartRadius(slot);
  g_world_nav_models.maximum_rise = maximum;
  g_world_nav_models.revision++;
  /* The stereographic chart metric is at most one. This radial bound covers
   * every column, independent of orbit, LOD selection or windmill frame. */
  return maximum;
}

static bool PrepareSkyPalaceProjection(
    const FrameSlot *slot, ArRenderRectI viewport,
    WorldNavigationProjection *out) {
  out->clip_frustum = true;
  out->tile_world = 4.0f / kSimWorldNavigationGlobeRadiusTiles;
  out->globe_radius_world = out->tile_world * out->chart_radius_tiles;
  const float landscape = slot->sim.world_navigation_relief
      ? slot->sim.landscape_height_pct / (float)kPercentScale : 0;
  out->height_world_per_unit = out->tile_world * landscape;
  const float terrain = g_world_nav_terrain.maximum_height * landscape;
  const SimWorldNavigationAtmosphereHeights atmosphere =
      SimWorldNavigationScene_AtmosphereHeights(terrain +
          (g_world_nav_mountains.active ? g_world_nav_mountains.scene.maximum_rise : 0),
          slot->sim.cloud_altitude_px);
  out->cloud_height_world = atmosphere.cloud_tiles * out->tile_world;
  out->atmosphere_height_world = atmosphere.outer_tiles * out->tile_world;
  const float models = WorldNavigationModelHeightBound(slot) *
      slot->sim.height_scale_x100 / (float)kPercentScale;
  /* Keep the larger Palace globe's altitude and local town scale as the
   * radius expands; scaling all three together would retain the old curve.
   * The global safety bound still keeps the eye outside all terrain/air. */
  const float eye_height = fmaxf(3.0f,
      (fmaxf(atmosphere.outer_tiles, terrain + models) + .35f) * out->tile_world);
  out->camera_world[2] = eye_height;
  const float horizon_dip = acosf(out->globe_radius_world /
      (out->globe_radius_world + eye_height));
  const Scene3DCamera camera = {
    .tilt_x = -(kPi * .5f - horizon_dip + .03f),
    .distance = 0, .fov_y = 1.05f,
  };
  Scene3D_BuildViewProjection(&camera, viewport.w, viewport.h, out->matrix);
  for (int row = 0; row < 4; row++)
    out->matrix[12 + row] -= out->matrix[8 + row] * eye_height;
  const SimWorldNavigationScene *scene = &slot->sim.world_navigation_scene;
  const float focus_x = scene->active_region_valid
      ? scene->active_region_x + scene->active_region_width * .5f
      : slot->sim.world_navigation.focus_x;
  const float focus_y = scene->active_region_valid
      ? scene->active_region_y + scene->active_region_height * .5f
      : slot->sim.world_navigation.focus_y;
  if (!SimWorldNavigationGlobe_BuildFrameAtRadius(
          out->chart_radius_tiles, focus_x / kSimWorldMapTilePixels,
          focus_y / kSimWorldMapTilePixels, 0, &out->globe_frame))
    return false;
  /* Rotate the selected region, not native travel coordinates, toward the
   * visible horizon. Intersect a ray just below the sea tangent with its
   * raised surface so high- and low-datum towns get the same framing.
   * The near intersection keeps the centre on the visible hemisphere and
   * leaves room for the town footprint before the limb. */
  const float focus_height = out->height_world_per_unit > 0
      ? WorldNavigationTerrainHeightAt(focus_x, focus_y, NULL) * out->height_world_per_unit : 0;
  const float focus_radius = out->globe_radius_world + fmaxf(0, focus_height);
  const float eye_radius = out->globe_radius_world + eye_height;
  const float sightline = fmaxf(0, kPi * .5f - horizon_dip - .045f);
  const float sine = sinf(sightline), cosine = cosf(sightline);
  const float along = eye_radius * cosine - sqrtf(fmaxf(0,
      focus_radius * focus_radius - eye_radius * eye_radius * sine * sine));
  const float rotation = atan2f(along * sine, eye_radius - along * cosine);
  return SimWorldNavigationGlobe_OrbitFrame(&out->globe_frame, 0, -rotation);
}

static bool PrepareWorldNavigationProjection(
    const FrameSlot *slot, ArRenderRectI viewport,
    WorldNavigationProjection *out) {
  if (!slot || !out || viewport.w <= 0 || viewport.h <= 0) return false;
  memset(out, 0, sizeof(*out));
  out->chart_radius_tiles = WorldNavigationChartRadius(slot);
  if (slot->sim.view == kSimView_SkyPalace)
    return PrepareSkyPalaceProjection(slot, viewport, out);
  /* Navigation's native Palace sprite is top-down. Look radially through
   * its travel location and the planet centre, independently of the town
   * camera's oblique pose. Manual inspection rotates the globe, not this eye. */
  Scene3DCamera camera = {
    .tilt_x = 0,
    .tilt_y = 0,
    .distance = (float)slot->sim.projection_distance_x100 /
        (float)kPercentScale,
    .fov_y = 0.4f,
  };
  if (camera.distance <= 0.0f)
    camera.distance = Scene3D_AutoFitDistance(camera.fov_y);
  else if (camera.distance < 2.0f)
    camera.distance = 2.0f;
  Scene3D_BuildViewProjection(
      &camera, viewport.w, viewport.h, out->matrix);
  for (int i = 0; i < 3; i++)
    out->camera_world[i] = -camera.distance * out->matrix[i * 4 + 3];

  const float focus_x = slot->sim.world_navigation.focus_x;
  const float focus_y = slot->sim.world_navigation.focus_y;
  const float *affine = slot->sim.world_navigation_scene.source_to_screen;
  const float heading = atan2f(-affine[3], affine[0]);
  if (!SimWorldNavigationGlobe_BuildFrameAtRadius(out->chart_radius_tiles,
          focus_x / kSimWorldMapTilePixels,
          focus_y / kSimWorldMapTilePixels, heading, &out->globe_frame))
    return false;
  if (!SimWorldNavigationGlobe_OrbitFrame(&out->globe_frame,
          slot->sim_camera.orbit_yaw, slot->sim_camera.orbit_pitch)) return false;
  ArRenderPointF centre, east, south;
  if (!WorldNavigationFlatOutputPoint(
          slot, viewport, focus_x, focus_y, &centre) ||
      !WorldNavigationFlatOutputPoint(
          slot, viewport, focus_x + kSimWorldMapTilePixels,
          focus_y, &east) ||
      !WorldNavigationFlatOutputPoint(
          slot, viewport, focus_x,
          focus_y + kSimWorldMapTilePixels, &south))
    return false;
  const float ex = east.x - centre.x, ey = east.y - centre.y;
  const float sx = south.x - centre.x, sy = south.y - centre.y;
  const float tile_area = fabsf(ex * sy - ey * sx);
  if (!isfinite(tile_area) || tile_area <= 0.0f) return false;
  float focus_normal[3], focus_metric;
  if (!SimWorldNavigationGlobe_SampleAtRadius(out->chart_radius_tiles,
          focus_x / kSimWorldMapTilePixels, focus_y / kSimWorldMapTilePixels,
          focus_normal, &focus_metric) || focus_metric <= 0) return false;
  /* Match the native tile scale at the travel focus. The spherical chart's
   * local metric would otherwise shrink towns away from the chart centre,
   * even with a scale-matched camera and the same native zoom register. */
  out->tile_world = sqrtf(tile_area) / ((float)viewport.h * focus_metric);
  const float landscape_scale = slot->sim.world_navigation_relief
      ? (float)slot->sim.landscape_height_pct / kPercentScale : 0.0f;
  out->height_world_per_unit = out->tile_world *
      landscape_scale;
  out->reference_height_units = landscape_scale > 0.0f
      ? WorldNavigationTerrainHeightAt(focus_x, focus_y, NULL) : 0.0f;
  out->globe_radius_world = fmaxf(
      0.25f, out->tile_world * out->chart_radius_tiles);
  const SimWorldNavigationAtmosphereHeights atmosphere =
      SimWorldNavigationScene_AtmosphereHeights(
          g_world_nav_terrain.maximum_height * landscape_scale +
              (g_world_nav_mountains.active
                  ? g_world_nav_mountains.scene.maximum_rise : 0.0f),
          slot->sim.cloud_altitude_px);
  /* Retain bounds during ordinary travel (including the initial black load),
   * not at the first visible Advent frame. Model height is independently
   * adjustable even with relief and native mountain geometry disabled. */
  const float model_rise = WorldNavigationModelHeightBound(slot) *
      slot->sim.height_scale_x100 / (float)kPercentScale;
  /* Native Advent hides OAM and drives its flat Mode-7 scale almost to
   * zero before black. The raised scene has a nonzero landing envelope.
   * Continue the approach through the original fade without entering its
   * geometry or arriving early and hovering while black catches up. */
  if (slot->sim.world_navigation_scene.composition.empty_animation) {
    const float facing_z = -out->matrix[11];
    const float envelope = fmaxf(atmosphere.outer_tiles,
        g_world_nav_terrain.maximum_height * landscape_scale + model_rise);
    const float support = out->chart_radius_tiles * (1 - facing_z) +
        envelope - out->reference_height_units * landscape_scale * facing_z;
    const float maximum_scale = support > 0 ? (camera.distance - .25f) / support : out->tile_world;
    out->tile_world = SimWorldNavigationScene_AdventScale(out->tile_world, maximum_scale);
    if (out->tile_world <= 0) return false;
    out->height_world_per_unit = out->tile_world * landscape_scale;
    out->globe_radius_world = fmaxf(.25f, out->tile_world * out->chart_radius_tiles);
  }
  const float reference_height_world =
      out->reference_height_units * out->height_world_per_unit;
  out->cloud_height_world =
      atmosphere.cloud_tiles * out->tile_world - reference_height_world;
  out->atmosphere_height_world =
      atmosphere.outer_tiles * out->tile_world - reference_height_world;
  return true;
}


static bool DrawWorldNavigationPalace(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  ArRenderPointF offset = {0};
  if (WorldNavigationInspecting(slot)) {
    /* The native Palace remains the travel marker, not a screen-fixed marker
     * for the inspection direction. Move its authored billboard with the
     * real focus and omit it on the far hemisphere; destination UI stays put. */
    float normal[3], world[3];
    if (!WorldNavigationSurfaceNormal(projection, slot->sim.world_navigation.focus_x,
                                      slot->sim.world_navigation.focus_y, normal))
      return false;
    WorldNavigationRadialPoint(projection, normal, 0, world);
    float facing = 0;
    for (int i = 0; i < 3; i++) facing += normal[i] * (projection->camera_world[i] - world[i]);
    if (facing <= 0) return true;
    Scene3DPoint focus;
    if (!Scene3D_ProjectWorldPoint(projection->matrix, world[0], world[1], world[2],
            viewport.w, viewport.h, &focus)) return false;
    /* Preserve the authored orientation; only its location follows
     * inspection. Normal radial travel needs no screen offset. */
    offset = (ArRenderPointF){focus.x - viewport.w * .5f, focus.y - viewport.h * .5f};
  }
  /* Retain native animation/brightness, but integrate its screen-space
   * marker with camera zoom: 75% at the default distance of 3, never larger
   * than native, and at least 35% so the travel focus stays readable in orbit.
   * Use the resolved eye distance so auto-fit and clamped cameras agree. */
  const float distance = hypotf(hypotf(projection->camera_world[0],
      projection->camera_world[1]), projection->camera_world[2]);
  const float scale = fminf(1.0f, fmaxf(.35f, .75f * 3.0f / distance));
  return WorldNavigationComposition_DrawLayer(slot, viewport,
      &slot->sim.world_navigation_scene.composition.palace,
      WorldNavigationComposition_Get()->palace, offset, scale);
}

static bool DrawWorldNavigationMasterFade(
    const FrameSlot *slot, ArRenderRectI viewport) {
  const uint8_t alpha = SimWorldNavigationScene_MasterFadeAlpha(
      slot->sim.world_navigation_brightness);
  if (!alpha) return true;
  const ArRenderRectF area = {
    (float)viewport.x, (float)viewport.y,
    (float)viewport.w, (float)viewport.h,
  };
  return ArRenderDevice_DrawSolidRect(
      &g_render_device, &area,
      (ArRenderColorF){0.0f, 0.0f, 0.0f, alpha / 255.0f},
      kArRenderBlendMode_Alpha);
}

bool EnsureWorldNavigationResourcesAtRadius(const FrameSlot *slot, float radius_tiles) {
  Sim3DPerformanceScope art_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Upload);
  const bool setup_timing = Sim3DPerformance_Enabled() &&
      ((!g_world_nav_mountains.ready && WorldNavigationMountainsEnabled(slot)) ||
       !g_world_nav_terrain.cliffs_ready ||
       !g_world_nav_art.serial);
  const uint64_t setup_started = setup_timing ? HostClock_Nanoseconds() : 0;
  EnsureWorldNavigationMountains(slot,radius_tiles);
  const uint64_t mountains_done = setup_timing ? HostClock_Nanoseconds() : 0;
  EnsureWorldNavigationCliffs(slot,radius_tiles);
  const uint64_t cliffs_done = setup_timing ? HostClock_Nanoseconds() : 0;
  const bool art_ready = EnsureWorldNavigationArt(slot,radius_tiles);
  const uint64_t art_done = setup_timing ? HostClock_Nanoseconds() : 0;
  if (setup_timing && art_done - setup_started > 5000000)
    fprintf(stderr, "[world-navigation-setup] mountains=%.3fms cliffs=%.3fms art=%.3fms\n",
        (double)(mountains_done - setup_started) / 1000000.0,
        (double)(cliffs_done - mountains_done) / 1000000.0,
        (double)(art_done - cliffs_done) / 1000000.0);
  /* Allocation fallback in the art bake must restore the matching old mesh. */
  if (g_world_nav_art.unavailable) EnsureWorldNavigationCliffs(slot,radius_tiles);
  Sim3DPerformance_End(art_performance);
  if (!art_ready) return false;
  /* Ensure the reference height used to keep the Palace's focus stationary
   * and the vertex field below it come from the same developed-map serial. */
  const Sim3DPerformanceScope terrain_prepare =
      Sim3DPerformance_Begin(kSim3DPerformance_WorldPrepare);
  if (slot->sim.world_navigation_relief && slot->sim.landscape_height_pct)
    PrepareWorldNavigationTerrain();
  Sim3DPerformance_End(terrain_prepare);
  return true;
}

static bool EnsureWorldNavigationResources(const FrameSlot *slot) {
  return EnsureWorldNavigationResourcesAtRadius(slot,WorldNavigationChartRadius(slot));
}

/* Draw into the caller's established viewport. This scene pass does not own
 * output setup/restoration or native foreground/UI composition. Navigation
 * and the Sky Palace backdrop share one mutually exclusive globe instance. */
static PresentationOutcome DrawWorldNavigationScene(
    const FrameSlot *slot, ArRenderRectI viewport,
    WorldNavigationProjection *projection) {
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  const Sim3DPerformanceScope projection_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_WorldPrepare);
  const bool projection_ok = PrepareWorldNavigationProjection(slot, viewport, projection);
  const uint64_t elapsed_ms = HostClock_Milliseconds();
  Sim3DPerformance_End(projection_performance);
  if (!projection_ok) {
    return kPresentationOutcome_CoreFailure;
  }
  if (slot->sim.view == kSimView_SkyPalace || slot->sim.world_navigation_backdrop) {
    const Sim3DPerformanceScope backdrop_performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Backdrop);
    const bool backdrop_ok = slot->sim.view == kSimView_SkyPalace
        ? PresentWorldNavSky_DrawBackdrop(&g_render_device, viewport,
                                          slot->sim.world_navigation_backdrop,
                                          PresentWorldNavSky_Horizon(viewport, projection))
        : DrawWorldNavigationSpaceBackdrop(viewport);
    Sim3DPerformance_End(backdrop_performance);
    if (!backdrop_ok) {
      return kPresentationOutcome_CoreFailure;
    }
  }
  if (slot->sim.world_navigation_atmosphere) {
    const Sim3DPerformanceScope atmosphere_performance =
        Sim3DPerformance_Begin(kSim3DPerformance_WorldAtmosphere);
    const bool atmosphere_ok = DrawWorldNavigationSphereShell(
        viewport, projection, kWorldNavigationShell_Atmosphere, 1);
    Sim3DPerformance_End(atmosphere_performance);
    if (!atmosphere_ok) {
      return kPresentationOutcome_CoreFailure;
    }
  }
  if (!DrawWorldNavigationSurfaceLayers(slot, viewport, projection, elapsed_ms)) {
    return kPresentationOutcome_CoreFailure;
  }
  if (!DrawWorldNavigationActiveRegionHaze(
          slot, viewport, projection)) {
    return kPresentationOutcome_CoreFailure;
  }
  if (!DrawWorldNavigationTowns(slot, viewport, projection)) {
    return kPresentationOutcome_CoreFailure;
  }
  /* Whole-world weather follows the same curved perspective surface as the
   * ground. It has no town sprite-window hole or cull boundary: every part of
   * this world is intentional content. Palace and labels stay screen-space. */
  Sim3DPerformanceScope weather_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Cloud);
  outcome = PresentationOutcome_Combine(
      outcome, DrawWorldNavigationWeather(slot, viewport, projection, elapsed_ms));
  Sim3DPerformance_End(weather_performance);
  Sim3DPerformanceScope submit =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthSubmit);
  ArRenderTexture composite = Sim3DDepthPass_Submit(
      &g_render_device, ArRenderTexture_Invalid());
  Sim3DPerformance_End(submit);
  const ArRenderRectF destination = {0, 0, (float)viewport.w, (float)viewport.h};
  /* The depth target accumulated straight-alpha inputs over transparent
   * black, so its stored RGB is premultiplied. Preserve soft sky-cloud edges
   * when composing the Palace; keep the established navigation look intact. */
  const ArRenderDrawState palace_composite = {
    .flags = kArRenderDrawState_Blend, .blend = kArRenderBlendMode_AlphaPremultiplied,
  };
  if (!ArRenderTexture_IsValid(composite) ||
      !ArRenderDevice_DrawTextureWithState(&g_render_device, composite, NULL, &destination,
          slot->sim.view == kSimView_SkyPalace ? &palace_composite : NULL)) {
    return kPresentationOutcome_CoreFailure;
  }
  if (slot->sim.view == kSimView_SkyPalace && slot->sim.world_navigation_atmosphere &&
      !PresentWorldNavSky_DrawMist(&g_render_device, viewport,
                                   PresentWorldNavSky_Horizon(viewport, projection)))
    return kPresentationOutcome_CoreFailure;
  if (!DrawWorldNavigationLightTreatment(slot, viewport)) {
    return kPresentationOutcome_CoreFailure;
  }

  /* INIDISP is a master brightness applied after the PPU has composed every
   * layer. Do the same for the host-owned world and all its effects. The
   * Palace/plaque/label captures are drawn afterward because OBJ range
   * rasterization already applied this frame's brightness to their pixels. */
  if (!DrawWorldNavigationMasterFade(slot, viewport)) {
    return kPresentationOutcome_CoreFailure;
  }
  return outcome;
}

PresentationOutcome PresentWorldNavigationBackdrop(
    const FrameSlot *slot, ArRenderRectI viewport) {
  if (!slot || slot->sim.view != kSimView_SkyPalace ||
      !slot->sim.world_navigation_scene.valid ||
      viewport.x != 0 || viewport.y != 0 || viewport.w <= 0 || viewport.h <= 0 ||
      !EnsureWorldNavigationResources(slot))
    return kPresentationOutcome_CoreFailure;
  WorldNavigationProjection projection;
  const PresentationOutcome outcome = DrawWorldNavigationScene(slot, viewport, &projection);
  Sim3DPerformance_EndPresentation();
  return outcome;
}

PresentationOutcome PresentWorldNavigation3D(const FrameSlot *slot) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  const SimWorldNavigationComposition *composition = &scene->composition;
  if (!scene->valid || !composition->valid ||
      !WorldNavigationComposition_Get()->uploaded)
    return kPresentationOutcome_CoreFailure;
  if (!EnsureWorldNavigationResources(slot)) return kPresentationOutcome_CoreFailure;
  if (!composition->empty_animation &&
      (!ArRenderTexture_IsValid(WorldNavigationComposition_Get()->palace) ||
       !ArRenderTexture_IsValid(WorldNavigationComposition_Get()->plaque) ||
       (composition->label.visible &&
        !ArRenderTexture_IsValid(WorldNavigationComposition_Get()->label))))
    return kPresentationOutcome_CoreFailure;

  const int aspect_width = slot->visible_width *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 7 : 1);
  const int aspect_height = slot->snes_height *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 6 : 1);
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_BeginAspectFit(
          &g_render_device, slot->ignore_aspect_ratio,
          aspect_width, aspect_height, black, black, &output_frame))
    return kPresentationOutcome_CoreFailure;
  const ArRenderRectI viewport = {
    0, 0, output_frame.viewport.w, output_frame.viewport.h,
  };
  ArLocalizedPreparedFrame localized_label;
  bool localized_label_ready = false;
  if (!composition->empty_animation && composition->label.visible) {
    const ArLocalizationScreenTextRecord *record =
        ArLocalizationFrame_FindScreenText(
            &slot->localization,
            kActRaiserLocalizationWorldNavigationSurface);
    if (record &&
        (unsigned)record->x + record->width <=
            kSimWorldNavigationCompositionWidth &&
        (unsigned)record->y + record->height <=
            kSimWorldNavigationCompositionHeight) {
      const ArRenderPointF top_left = WorldNavigationComposition_ProjectPoint(
          slot, viewport, record->x, record->y);
      const ArRenderPointF bottom_right = WorldNavigationComposition_ProjectPoint(
          slot, viewport, record->x + record->width,
          record->y + record->height);
      const int left = (int)lroundf(top_left.x);
      const int top = (int)lroundf(top_left.y);
      const ArRenderRectI bounds = {
          left, top,
          (int)lroundf(bottom_right.x) - left,
          (int)lroundf(bottom_right.y) - top,
      };
      localized_label_ready = ArLocalizedTextPresenter_PrepareScreenText(
          &g_render_device, &slot->localization,
          kActRaiserLocalizationWorldNavigationSurface,
          bounds, &localized_label);
    }
  }
  WorldNavigationProjection projection;
  const PresentationOutcome outcome = DrawWorldNavigationScene(slot, viewport, &projection);
  if (!PresentationOutcome_IsUsable(outcome)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return outcome;
  }
  if (!composition->empty_animation &&
      (!DrawWorldNavigationPalace(slot, viewport, &projection) ||
       !WorldNavigationComposition_DrawLayer(
           slot, viewport, &composition->plaque,
           WorldNavigationComposition_Get()->plaque, (ArRenderPointF){0}, 1.0f) ||
       (!localized_label_ready && composition->label.visible &&
        !WorldNavigationComposition_DrawLayer(
            slot, viewport, &composition->label,
            WorldNavigationComposition_Get()->label, (ArRenderPointF){0}, 1.0f)) ||
       (localized_label_ready &&
        !ArLocalizedTextPresenter_DrawWithBrightness(
            &g_render_device, &localized_label,
            slot->sim.world_navigation_brightness / 15.0f)))) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }
  if (!ArRenderOutputFrame_Finish(&output_frame))
    return kPresentationOutcome_CoreFailure;
  Sim3DPerformance_EndPresentation();
  return outcome;
}

/* Each reset owns the same fields as its preparation path. Readiness gates
 * invalidate retained CPU keys without erasing monotonic revision counters.
 * Presentation has already drained; stop the shared workers before releasing
 * their borrowed arrays, and detach terrain before destroying mountain data. */
static void ResetWorldNavigationWorkers(void) {
  HostParallelWork_Destroy(s_world_workers);
  s_world_workers = NULL;
  s_world_workers_attempted = false;
}

/* World navigation resource reset. All resource releases
 * stay private to this view; no cache survives a renderer replacement. */
void PresentWorldNav_ResetResources(void) {
  ResetWorldNavigationWorkers();
  ResetWorldNavigationGlobeSurfaces();
  ResetWorldNavigationModels();
  ResetWorldNavigationWeather();
  ResetWorldNavigationTerrain();
  ResetWorldNavigationMountains();
  ResetWorldNavigationArt();
  WorldNavigationComposition_Reset();
}
