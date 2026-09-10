/* Standalone world-navigation composition over the shared authored town
 * models and portable globe terrain/art. Camera-dependent memoization lives
 * here; model compilation/cache policy stays in the portable SIM modules.
 * All drawing crosses the renderer/depth-pass interfaces, never a backend.
 *
 * The D6 no-live-globals invariant holds here as everywhere in the present
 * family: no g_ppu, no g_settings, no Settings_Visible*(). State arrives via
 * the `const FrameSlot *`. */
#include "present_world_nav_geometry.h"
#include "present_world_nav_sky.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present.h"
#include "action/action_effect_render.h"
#include "constants.h"
#include "deterministic_hash.h"
#include "snesrecomp/game/types.h"
#include "diorama/diorama.h"
#include "host/host_clock.h"
#include "presentation_outcome.h"
#include "render/render_device.h"
#include "render/render_output.h"
#include "scene3d_math.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_background_bridge.h"
#include "sim/sim_background_voxel_biome.h"
#include "sim/sim_background_voxel_model_cache.h"
#include "sim/sim_background_voxel_palette.h"
#include "sim/sim_background_voxel_proportions.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_navigation_art.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim_world_navigation_globe.h"
#include "sim/sim_world_navigation_terrain.h"
#include "sim/sim_world_navigation_mountains.h"
#include "sim/sim_world_navigation_mountain_transition.h"
#include "sim/sim_world_navigation_clouds.h"
#include "sim/sim_world_navigation_cliffs.h"
#include "sim/sim_town_ground_art.h"
#include "sim/sim3d.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
extern ArRenderDevice g_render_device;
#include "present_sim3d_internal.h"


typedef struct WorldNavigationModelBounds {
  float minimum_rise, maximum_rise, angular_radius;
} WorldNavigationModelBounds;
typedef struct WorldNavigationCliffProjection {
  Sim3DDepthVertex depth[4];
  Scene3DClipPoint clip[4];
  ArRenderColorF colour[4];
  float normal[4][3];
  ArRenderPointF cloud_uv[kSimCloudLayerCount][4];
  uint8_t outside[4];
} WorldNavigationCliffProjection;
static void PrepareWorldNavigationTerrain(void);

enum {
  kWorldNavigationTerrainCells = kSimWorldMapTiles,
  kWorldNavigationTerrainAxis = kWorldNavigationTerrainCells + 1,
  kWorldNavigationTerrainVertexCount =
      kWorldNavigationTerrainAxis * kWorldNavigationTerrainAxis,
  kWorldNavigationOceanRings = 48,
  kWorldNavigationOceanSectors = 96,
  kWorldNavigationOceanVertexCount =
      1 + kWorldNavigationOceanRings * kWorldNavigationOceanSectors,
  kWorldNavigationOceanIndexCount =
      kWorldNavigationOceanSectors * 3 +
      (kWorldNavigationOceanRings - 1) *
          kWorldNavigationOceanSectors * 6,
};

_Static_assert(kWorldNavigationTerrainVertexCount <= UINT16_MAX &&
    kWorldNavigationOceanVertexCount <= UINT16_MAX,
    "shadow receiver corner indices must cover both surface grids");

typedef struct WorldNavigationShellGeometry {
  Sim3DDepthVertex points[kWorldNavigationOceanVertexCount];
  Scene3DClipPoint clip[kWorldNavigationOceanVertexCount];
  float normal[kWorldNavigationOceanVertexCount][3];
  float alpha[kWorldNavigationOceanVertexCount];
  bool front[kWorldNavigationOceanVertexCount];
  uint8_t outside[kWorldNavigationOceanVertexCount];
} WorldNavigationShellGeometry;

typedef enum WorldNavigationShell {
  kWorldNavigationShell_Ocean,
  kWorldNavigationShell_Atmosphere,
  kWorldNavigationShell_Cloud,
} WorldNavigationShell;

static const float kWorldNavigationTerrainAmbient = 0.68f;

/* Presentation choice, not a native coordinate or renderer setting. Keep
 * town/relief scale fixed while broadening only the Palace's horizon. */
static float WorldNavigationChartRadius(const FrameSlot *slot) {
  return kSimWorldNavigationGlobeRadiusTiles *
      (slot->sim.view == kSimView_SkyPalace ? 3.0f : 1.0f);
}

typedef struct WorldNavigationGroundKey {
  WorldNavigationProjection projection;
  float source_to_screen[6];
  ArRenderRectI viewport;
  uint32_t geography_serial;
  int snes_width, snes_height, visible_width, visible_x0;
  int light_azimuth, light_elevation, lighting;
} WorldNavigationGroundKey;
typedef struct WorldNavigationMountainProjection {
  float normal[4][3], floor[4], rise[4];
  Sim3DDepthVertex points[4];
  Scene3DClipPoint clip[4];
  bool visible;
} WorldNavigationMountainProjection;
typedef struct WorldNavigationMountainProjectionKey {
  WorldNavigationProjection projection;
  int width, height, lighting;
} WorldNavigationMountainProjectionKey;
typedef struct WorldNavigationVisibleTownObject {
  const SimWorldNavigationTownObject *object;
  SimBackgroundVoxelDetail detail;
} WorldNavigationVisibleTownObject;

typedef struct WorldNavigationModelProjectionKey {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  uint32_t model_revision, surface_revision;
  int height_scale, light_azimuth, light_elevation, lighting, windmill_phase;
} WorldNavigationModelProjectionKey;

/* Private owners: artwork publication, registered surfaces, model bounds,
 * shell scratch, weather mapping, and native composition have separate reset
 * and invalidation lifetimes. These are one mutually exclusive globe view,
 * not an implicit multi-scene renderer or storage borrowed by SIM. */
typedef enum WorldNavigationCloudSurface {
  kWorldNavigationCloudSurface_Ground,
  kWorldNavigationCloudSurface_Ocean,
  kWorldNavigationCloudSurface_Body,
} WorldNavigationCloudSurface;

typedef struct WorldNavigationCloudShellKey {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  SimWorldNavigationCloudRotation rotation;
} WorldNavigationCloudShellKey;

typedef struct WorldNavigationCloudUVCache {
  ArRenderPointF ground[kWorldNavigationTerrainVertexCount];
  ArRenderPointF ocean[kWorldNavigationOceanVertexCount];
  ArRenderPointF body[kWorldNavigationOceanVertexCount];
  float body_direction[kWorldNavigationOceanVertexCount][3];
  bool ground_ready, ocean_ready, body_ready;
  SimWorldNavigationCloudRotation ground_rotation;
  WorldNavigationCloudShellKey ocean_key, body_key;
  uint32_t cliff_serial;
  SimWorldNavigationCloudRotation cliff_rotation;
} WorldNavigationCloudUVCache;

typedef enum WorldNavigationReceiverSurface {
  kWorldNavigationReceiver_Ground,
  kWorldNavigationReceiver_Cliff,
  kWorldNavigationReceiver_Ocean,
} WorldNavigationReceiverSurface;
typedef struct WorldNavigationShadowReceiver {
  WorldNavigationClipPlan geometry;
  uint16_t corners[4];
  uint32_t cliff;
  WorldNavigationReceiverSurface surface;
} WorldNavigationShadowReceiver;
typedef struct WorldNavigationReceiverKey {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  uint32_t terrain_serial, cliff_serial;
} WorldNavigationReceiverKey;

static struct {
  ArRenderTexture palace;
  ArRenderTexture ui;
  bool uploaded;
} s_world_composition;

static struct {
  uint32_t serial;
  uint32_t geography;
  SimWorldNavigationTownGround sources;
  bool detailed;
  bool models;
  uint8_t phase;
  uint32_t blur_serial;
  bool unavailable;
  uint32_t *pixels;
  uint32_t *baseline;
  bool cliffs;
} s_world_art;

static struct {
  SimWorldNavigationMountainScene scene;
  SimWorldNavigationTownGround sources;
  bool ready;
  SimWorldNavigationMountainAtlasUpdate lava_upload;
  bool active;
  bool developed;
  SimWorldNavigationMountainTransition transition;
  bool transition_ready;
  uint32_t transition_serial;
  float chart_radius_tiles;
  int relief_pct;
  WorldNavigationMountainProjection *projection;
  size_t projection_capacity;
  bool samples_ready;
  bool projection_ready;
  bool projection_unavailable;
  WorldNavigationMountainProjectionKey projection_key;
} s_world_mountains;

static struct {
  SimWorldNavigationTownObject
      objects[kSimWorldNavigationTownObjectCapacity];
  WorldNavigationModelBounds
      bounds[kSimWorldNavigationTownObjectCapacity];
  uint16_t object_count;
  int detail;
  int style;
  float chart_radius_tiles;
  float maximum_rise;
  uint32_t revision;
  bool windmills;
  WorldNavigationModelProjectionKey projection_key;
  Sim3DDepthVertex *projected;
  size_t projected_count, projected_capacity;
  bool projected_valid, projection_key_ready, capturing, projection_unavailable;
} s_world_models = {.detail = -1, .style = -1};

static struct {
  SimWorldNavigationCliffScene cliffs;
  WorldNavigationCliffProjection *cliff_projection;
  uint32_t cliff_serial;
  uint32_t cliff_geography;
  float cliff_chart_radius_tiles;
  uint8_t cliff_mask;
  bool cliffs_ready;
  bool ready;
  uint32_t serial;
  float maximum_height;
  float height[
      kWorldNavigationTerrainVertexCount];
  float floor[
      kWorldNavigationTerrainVertexCount];
  float authored[
      kWorldNavigationTerrainVertexCount];
  ArRenderVertex2D vertices[
      kWorldNavigationTerrainVertexCount];
  Sim3DDepthVertex depth[
      kWorldNavigationTerrainVertexCount];
  Scene3DClipPoint clip[
      kWorldNavigationTerrainVertexCount];
  uint8_t outside[kWorldNavigationTerrainVertexCount];
  WorldNavigationGroundKey projection_key;
  bool projection_ready;
} s_world_terrain;

static struct {
  WorldNavigationShellGeometry cloud;
  WorldNavigationShellGeometry ocean;
  bool indices_ready;
  int32_t indices[
      kWorldNavigationOceanIndexCount];
  ArRenderVertex2D vertices[
      kWorldNavigationOceanVertexCount];
} s_world_shells;

static struct {
  bool unavailable;
  bool failure_reported;
  bool ready;
  float ground_normals[kWorldNavigationTerrainVertexCount][3];
  bool normals_ready;
  float chart_radius_tiles;
  WorldNavigationCloudUVCache uv[kSimCloudLayerCount];
  WorldNavigationShadowReceiver *receivers;
  size_t receiver_count, receiver_capacity;
  WorldNavigationReceiverKey receiver_key;
  bool receivers_ready, receivers_unavailable;
} s_world_weather;

static void DestroyWorldNavigationMountainProjection(void) {
  free(s_world_mountains.projection);
  s_world_mountains.projection = NULL;
  s_world_mountains.projection_capacity = 0;
  s_world_mountains.samples_ready = false;
  s_world_mountains.projection_ready = false;
  s_world_mountains.projection_unavailable = false;
}

static ArRenderTexture EnsureWorldNavigationCompositionTexture(
    ArRenderTexture *texture) {
  if (ArRenderTexture_IsValid(*texture)) return *texture;
  const ArRenderTextureDesc desc = {
    .width = kSimWorldNavigationCompositionWidth,
    .height = kSimWorldNavigationCompositionHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(&g_render_device, &desc, texture)) {
    fprintf(stderr,
            "[world-navigation] composition texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    return ArRenderTexture_Invalid();
  }
  return *texture;
}

void UploadWorldNavigationComposition(const FrameSlot *slot) {
  s_world_composition.uploaded = false;
  if (!slot || slot->sim.view != kSimView_WorldNavigation) return;
  const SimWorldNavigationComposition *composition =
      &slot->sim.world_navigation_scene.composition;
  if (!composition->valid) return;
  if (composition->empty_animation) {
    s_world_composition.uploaded = true;
    return;
  }

  ArRenderTexture palace = EnsureWorldNavigationCompositionTexture(
      &s_world_composition.palace);
  ArRenderTexture ui = EnsureWorldNavigationCompositionTexture(
      &s_world_composition.ui);
  if (!ArRenderTexture_IsValid(palace) ||
      !ArRenderTexture_IsValid(ui))
    return;
  ArRenderRectI palace_rect = {
    0, 0, composition->palace.width, composition->palace.height,
  };
  ArRenderRectI ui_rect = {
    0, 0, composition->ui.width, composition->ui.height,
  };
  if (!ArRenderDevice_UpdateTexture(
          &g_render_device, palace, &palace_rect,
          g_sim_world_navigation_palace_pixels,
          kSimWorldNavigationCompositionPitch) ||
      !ArRenderDevice_UpdateTexture(
          &g_render_device, ui, &ui_rect,
          g_sim_world_navigation_ui_pixels,
          kSimWorldNavigationCompositionPitch))
    return;
  s_world_composition.uploaded = true;
}

static bool EnsureWorldNavigationCloudTexture(void) {
  enum { kPaddedPixels = kSimWorldNavigationCloudWidth * 2 };
  const int height = kSimWorldNavigationCloudHeight * kSimCloudLayerCount;
  if (s_world_weather.ready) return true;
  if (s_world_weather.unavailable) return false;
  uint32_t *pixels = malloc(
      (size_t)kPaddedPixels * height * sizeof(*pixels));
  if (!pixels) {
    s_world_weather.unavailable = true;
    return false;
  }
  for (int layer = 0; layer < kSimCloudLayerCount; layer++) {
    uint32_t *band = pixels + (size_t)layer * kSimWorldNavigationCloudHeight * kPaddedPixels;
    if (!SimWorldNavigationClouds_Bake(
            band, kPaddedPixels, kSimWorldNavigationCloudWidth,
            kSimWorldNavigationCloudHeight, kSimCloudLayers[layer].scale)) {
      free(pixels);
      s_world_weather.unavailable = true;
      return false;
    }
    /* The first/last texels coincide, so longitude repeats after width-1
     * intervals. Copying whole rows would shift the unwrapped half one texel. */
    for (int y = 0; y < kSimWorldNavigationCloudHeight; y++) {
      uint32_t *row = band + (size_t)y * kPaddedPixels;
      for (int x = kSimWorldNavigationCloudWidth; x < kPaddedPixels; x++)
        row[x] = row[x - (kSimWorldNavigationCloudWidth - 1)];
    }
  }
  const ArRenderRectI region = {0, 0, kPaddedPixels, height};
  s_world_weather.ready = Sim3DDepthPass_UploadAtlasRegions(
      &g_render_device, kSim3DDepthPass_Cloud, pixels,
      kPaddedPixels, height, kPaddedPixels * (int)sizeof(*pixels),
      &region, 1);
  free(pixels);
  s_world_weather.unavailable = !s_world_weather.ready;
  return s_world_weather.ready;
}

/* The publication key certifies both the retained CPU pixels and the GPU
 * atlas. Invalidating it does not discard usable old GPU resources, but
 * prevents a cache hit or an incremental patch until a full upload succeeds. */
static void InvalidateWorldNavigationArtPublication(void) {
  s_world_art.serial = 0;
}

static void RefreshWorldNavigationMountainGeometry(void) {
  const bool joined = s_world_mountains.transition_ready &&
      s_world_mountains.relief_pct > 0;
  SimWorldNavigationTerrain_SetMountainJoin(
      joined ? s_world_mountains.transition.join_rise : NULL,
      joined ? s_world_mountains.transition.join_weight : NULL,
      joined ? 100.0f / s_world_mountains.relief_pct : 0);
  const bool limited = joined && s_world_mountains.transition.continuation_anchor_count > 0;
  SimWorldNavigationTerrain_SetMountainContinuationLimit(
      limited ? s_world_mountains.transition.continuation_rise : NULL,
      limited ? s_world_mountains.transition.continuation_weight : NULL,
      limited ? 100.0f / s_world_mountains.relief_pct : 0);
  s_world_terrain.ready = false;
  s_world_terrain.cliffs_ready = false;
  s_world_terrain.projection_ready = false;
  s_world_mountains.samples_ready = false;
  s_world_mountains.projection_ready = false;
  s_world_mountains.projection_unavailable = false;
}

static void InvalidateWorldNavigationMountainSurface(void) {
  SimWorldNavigationTerrain_SetMountainReplacement(
      s_world_mountains.active
          ? s_world_mountains.scene.replacement : NULL);
  SimWorldNavigationTerrain_SetMountainTransition(
      s_world_mountains.transition_ready
          ? s_world_mountains.transition.ridge_scale : NULL);
  RefreshWorldNavigationMountainGeometry();
  InvalidateWorldNavigationArtPublication();
}

static void RebuildWorldNavigationMountainTransition(
    const SimWorldNavigationTownGround *ground, float radius_tiles) {
  SimWorldNavigationMountainTransition_Destroy(&s_world_mountains.transition);
  s_world_mountains.transition_serial = SimWorldMap_GeographySerial();
  s_world_mountains.chart_radius_tiles = radius_tiles;
  s_world_mountains.transition_ready = s_world_mountains.active &&
      SimWorldNavigationMountainTransition_BuildAtRadius(&s_world_mountains.scene,
          ground, radius_tiles, &s_world_mountains.transition);
  InvalidateWorldNavigationMountainSurface();
}

static bool WorldNavigationMountainsEnabled(const FrameSlot *slot) {
  return slot->sim.world_navigation_mountains &&
      slot->sim.world_navigation_relief && slot->sim.landscape_height_pct &&
      !s_world_art.unavailable && SimTownGroundArt_Available();
}

static void UpdateWorldNavigationLava(const FrameSlot *slot) {
  if (!s_world_mountains.active) return;
  SimWorldNavigationMountainAtlasUpdate update;
  if (SimWorldNavigationMountains_UpdateLava(&s_world_mountains.scene,
          slot->sim.game_frame, &update))
    s_world_mountains.lava_upload = update;
  if (!s_world_mountains.lava_upload.width) return;
  const SimWorldNavigationMountainAtlasUpdate *pending = &s_world_mountains.lava_upload;
  const ArRenderRectI region = {pending->x, pending->y, pending->width, pending->height};
  const int width = kSimWorldNavigationMountainAtlasPixels;
  const Sim3DPerformanceScope transfer = Sim3DPerformance_Begin(kSim3DPerformance_WorldTransfer);
  const bool uploaded = Sim3DDepthPass_UploadAtlasRegions(&g_render_device,
      kSim3DDepthPass_WorldMountain, s_world_mountains.scene.atlas,
      width, width, width * (int)sizeof(uint32_t), &region, 1);
  Sim3DPerformance_End(transfer);
  /* A failed upload leaves the last GPU palette usable. Retain the dirty
   * rectangle for retry, even if the clock freezes or reverses next frame. */
  if (uploaded) {
    Sim3DPerformance_AddUpload((uint64_t)region.w * region.h * sizeof(uint32_t));
    s_world_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  }
}

static void EnsureWorldNavigationMountains(const FrameSlot *slot) {
  const float radius_tiles = WorldNavigationChartRadius(slot);
  const bool relief_changed = s_world_mountains.relief_pct != slot->sim.landscape_height_pct;
  s_world_mountains.relief_pct = slot->sim.landscape_height_pct;
  if (!WorldNavigationMountainsEnabled(slot)) {
    s_world_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
    if (s_world_mountains.ready || s_world_mountains.active) {
      DestroyWorldNavigationMountainProjection();
      SimWorldNavigationMountains_Destroy(&s_world_mountains.scene);
      s_world_mountains.ready = false;
      s_world_mountains.active = false;
      RebuildWorldNavigationMountainTransition(NULL, radius_tiles);
    }
    return;
  }
  const SimWorldNavigationTownGround *ground = &slot->sim.world_navigation_towns.ground;
  /* Curvature changes only when switching view families, not when selecting
   * another town. Rebuild from native art here so transition colors never
   * inherit an animated lava palette; keep the shared compiled model cache. */
  if (s_world_mountains.ready &&
      s_world_mountains.chart_radius_tiles == radius_tiles &&
      s_world_mountains.developed == SimWorldMap_DevelopedAvailable() &&
      s_world_mountains.transition_serial == SimWorldMap_GeographySerial() && !memcmp(
          ground, &s_world_mountains.sources, sizeof(*ground))) {
    if (relief_changed)
      RefreshWorldNavigationMountainGeometry();
    UpdateWorldNavigationLava(slot);
    return;
  }
  s_world_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  s_world_mountains.sources = *ground;
  s_world_mountains.developed = SimWorldMap_DevelopedAvailable();
  s_world_mountains.ready = true;
  s_world_mountains.active = SimWorldNavigationMountains_Build(
      ground, &s_world_mountains.scene) && s_world_mountains.scene.face_count;
  if (s_world_mountains.active) {
    /* Optional completion of clipped stamps depends on exterior rock, so
     * geography (but not animated water) participates in the source key. */
    (void)SimWorldNavigationMountains_ContinueEdges(ground, &s_world_mountains.scene);
    /* Ground blending derives its rock tint from the immutable source art,
     * never from whichever lava pulse happened to trigger a cold rebuild. */
    RebuildWorldNavigationMountainTransition(ground, radius_tiles);
    SimWorldNavigationMountainAtlasUpdate initial_palette;
    (void)SimWorldNavigationMountains_UpdateLava(&s_world_mountains.scene,
        slot->sim.game_frame, &initial_palette);
    const int width = kSimWorldNavigationMountainAtlasPixels;
    const ArRenderRectI region = {0, 0, width, width};
    if (!Sim3DDepthPass_UploadAtlasRegions(
            &g_render_device, kSim3DDepthPass_WorldMountain,
            s_world_mountains.scene.atlas, width, width,
            width * (int)sizeof(uint32_t), &region, 1)) {
      s_world_mountains.active = false;
      SimWorldNavigationMountains_Destroy(&s_world_mountains.scene);
      RebuildWorldNavigationMountainTransition(NULL, radius_tiles);
    } else {
      Sim3DPerformance_AddUpload((uint64_t)width * width * sizeof(uint32_t));
    }
  } else {
    RebuildWorldNavigationMountainTransition(ground, radius_tiles);
  }
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
      while (x < kSimWorldMapTiles && WorldNavigationAnimationBlockChanged(changes, x, y, step)) x += step;
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
          s_world_art.pixels, kWidth, kWidth, kWidth * (int)sizeof(uint32_t),
          regions, count);
  Sim3DPerformance_End(transfer);
  if (!uploaded) return false;
  uint64_t bytes = 0;
  for (int i = 0; i < count; i++) bytes += (uint64_t)regions[i].w * regions[i].h * sizeof(uint32_t);
  Sim3DPerformance_AddUpload(bytes);
  return true;
}

static bool EnsureWorldNavigationArt(const FrameSlot *slot) {
  if (!slot || !slot->sim.underlay_serial) return false;
  const bool detailed = slot->sim.world_navigation_ground_detail != 0;
  const bool models = slot->sim.world_navigation_models &&
      slot->sim.background_voxel_enabled;
  const SimWorldNavigationTownGround *ground =
      &slot->sim.world_navigation_towns.ground;
  const uint8_t phase = detailed && ground->enabled_town_mask && !s_world_art.unavailable
      ? SimTownGroundArt_AnimationPhase(slot->sim.game_frame) : 0;
  const bool same_style =
      s_world_art.cliffs == (s_world_terrain.cliffs.town_mask != 0) &&
      s_world_art.detailed == detailed &&
      s_world_art.models == models &&
      (!(detailed || s_world_mountains.active) ||
       !memcmp(&s_world_art.sources,
                            ground, sizeof(*ground)));
  const bool same_image = s_world_art.serial == slot->sim.underlay_serial;
  if (same_style && same_image && s_world_art.phase == phase) return true;
  const uint32_t *developed = SimWorldMap_BakedPixels();
  if (!developed) return false;
  if (same_style && s_world_art.serial &&
      s_world_art.geography == SimWorldMap_GeographySerial() &&
      slot->sim.underlay_serial == SimWorldMap_Serial() && !s_world_art.unavailable &&
      s_world_art.pixels && s_world_art.baseline) {
    SimWorldNavigationArtChanges changes;
    uint8_t world_cells[kSimWorldMapBytes];
    const Sim3DPerformanceScope animation = Sim3DPerformance_Begin(kSim3DPerformance_WorldAnimation);
    const bool world_ready = same_image ||
        (SimWorldMap_WaterAnimationCells(world_cells) &&
         SimWorldMap_BakeBaseline(s_world_art.baseline, kSimWorldMapPixels));
    const bool updated = world_ready && SimWorldNavigationArt_UpdateAnimation(
            s_world_art.pixels, kSimWorldNavigationArtPixels,
            developed, kSimWorldMapPixels, s_world_art.baseline, kSimWorldMapPixels,
            same_image ? NULL : world_cells, detailed ? ground : NULL,
            models, s_world_terrain.cliffs.town_mask != 0,
            s_world_art.phase, phase, &changes) &&
        (!s_world_mountains.active || SimWorldNavigationMountains_ClearGround(
            s_world_art.pixels, kSimWorldNavigationArtPixels, ground,
            s_world_mountains.scene.town_mask, changes.cells)) &&
        (!s_world_mountains.transition_ready || SimWorldNavigationMountainTransition_Apply(
            &s_world_mountains.transition, s_world_art.pixels,
            kSimWorldNavigationArtPixels, changes.cells));
    Sim3DPerformance_End(animation);
    if (updated) {
      if (!UploadWorldNavigationAnimation(&changes)) {
        /* CPU pixels advanced but the GPU transaction did not. Reconstruct
         * and fully upload on retry, even if the simulation clock rewinds. */
        InvalidateWorldNavigationArtPublication();
        return false;
      }
      s_world_art.phase = phase;
      s_world_art.serial = slot->sim.underlay_serial;
      return true;
    }
  }
  /* A full rebuild overwrites the CPU image before upload. Even if a failed
   * upload is followed by a return to the last GPU style/phase, that old key
   * must not certify the newly mutated CPU pixels for animation patches. */
  InvalidateWorldNavigationArtPublication();
  const uint32_t *pixels = developed;
  int width = kSimWorldMapPixels;
  if (!s_world_art.unavailable) {
    if (!s_world_art.pixels)
      s_world_art.pixels = malloc(
          (size_t)kSimWorldNavigationArtPixels * kSimWorldNavigationArtPixels *
          sizeof(uint32_t));
    if (!s_world_art.baseline)
      s_world_art.baseline = malloc(
          (size_t)kSimWorldMapPixels * kSimWorldMapPixels * sizeof(uint32_t));
    if (s_world_art.pixels && s_world_art.baseline &&
        SimWorldMap_BakeBaseline(s_world_art.baseline, kSimWorldMapPixels) &&
        SimWorldNavigationArt_Build(
            s_world_art.pixels, kSimWorldNavigationArtPixels,
            developed, kSimWorldMapPixels,
            s_world_art.baseline, kSimWorldMapPixels)) {
      pixels = s_world_art.pixels;
      width = kSimWorldNavigationArtPixels;
      if (detailed)
        SimWorldNavigationArt_OverlayTownGround(
            s_world_art.pixels, width, ground, models,
            s_world_terrain.cliffs.town_mask != 0, phase);
      if (s_world_mountains.active &&
          !SimWorldNavigationMountains_ClearGround(
              s_world_art.pixels, width, ground,
              s_world_mountains.scene.town_mask, NULL)) return false;
      if (s_world_mountains.transition_ready &&
          !SimWorldNavigationMountainTransition_Apply(
              &s_world_mountains.transition, s_world_art.pixels, width, NULL))
        return false;
    } else {
      s_world_art.unavailable = true;
      fprintf(stderr, "[world-navigation] using native-resolution world art\n");
      /* Old ground must keep its inferred slopes if native cleanup failed. */
      EnsureWorldNavigationMountains(slot);
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
  s_world_art.serial = slot->sim.underlay_serial;
  s_world_art.geography = SimWorldMap_GeographySerial();
  s_world_art.detailed = detailed;
  s_world_art.models = models;
  s_world_art.phase = phase;
  s_world_art.cliffs = s_world_terrain.cliffs.town_mask != 0;
  if (detailed || s_world_mountains.active)
    s_world_art.sources = *ground;
  return true;
}

static bool EnsureWorldNavigationBlur(const FrameSlot *slot) {
  if (s_world_art.blur_serial == slot->sim.underlay_serial) return true;
  enum { kBlurDivisor = 4, kBlurPixels = kSimWorldMapPixels / kBlurDivisor };
  static uint32_t pixels[kBlurPixels * kBlurPixels];
  const ArRenderRectI region = {0, 0, kBlurPixels, kBlurPixels};
  if (!SimWorldMap_Downsample(pixels, kBlurPixels, kBlurDivisor) ||
      !Sim3DDepthPass_UploadAtlasRegions(
          &g_render_device, kSim3DDepthPass_GroundBlur, pixels,
          kBlurPixels, kBlurPixels, kBlurPixels * (int)sizeof(uint32_t), &region, 1))
    return false;
  s_world_art.blur_serial = slot->sim.underlay_serial;
  return true;
}

/* $09 full-world presentation.
 *
 * Navigation uses the same developed texture cache as the town underlay, but
 * none of the underlay geometry. Its captured Mode-7 matrix supplies focus,
 * zoom and in-plane rotation before the shared town perspective is applied. */
static ArRenderPointF WorldNavigationAuthenticToOutput(
    const FrameSlot *slot, ArRenderRectI viewport,
    float authentic_x, float authentic_y) {
  const float authentic_x0 =
      ((float)slot->snes_width -
       (float)kSimWorldNavigationCompositionWidth) * 0.5f;
  const float captured_x = authentic_x0 + authentic_x;
  return (ArRenderPointF){
    (float)viewport.x +
        (captured_x - (float)slot->visible_x0) *
            (float)viewport.w / (float)slot->visible_width,
    (float)viewport.y + authentic_y *
        (float)viewport.h / (float)slot->snes_height,
  };
}

static int WorldNavigationTerrainVertexIndex(int tile_x, int tile_y) {
  return tile_y * kWorldNavigationTerrainAxis + tile_x;
}

static float WorldNavigationSmoothstep(float value) {
  if (value <= 0.0f) return 0.0f;
  if (value >= 1.0f) return 1.0f;
  return value * value * (3.0f - 2.0f * value);
}

static void EnsureWorldNavigationCliffs(const FrameSlot *slot) {
  const float radius_tiles = WorldNavigationChartRadius(slot);
  uint8_t mask = 0;
#if AR_SIM3D_TERRAIN_ELEVATION
  if (slot->sim.world_navigation_ground_detail && slot->sim.world_navigation_relief &&
      slot->sim.landscape_height_pct && !s_world_art.unavailable &&
      SimTownGroundArt_Available())
    mask = slot->sim.world_navigation_towns.ground.enabled_town_mask;
#endif
  const uint32_t geography = SimWorldMap_GeographySerial();
  if (s_world_terrain.cliffs_ready && s_world_terrain.cliff_mask == mask &&
      s_world_terrain.cliff_chart_radius_tiles == radius_tiles &&
      s_world_terrain.cliff_geography == geography && (!mask || s_world_terrain.ready))
    return;
  if (mask) PrepareWorldNavigationTerrain();
  /* The atmosphere bound must restore exactly when detailed cliffs turn off,
   * including a high owned corner absent from the shared overview vertices. */
  s_world_terrain.maximum_height = 0;
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++)
    s_world_terrain.maximum_height = fmaxf(
        s_world_terrain.maximum_height, s_world_terrain.height[i]);
  SimWorldNavigationCliffs_Destroy(&s_world_terrain.cliffs);
  free(s_world_terrain.cliff_projection);
  s_world_terrain.cliff_projection = NULL;
  /* Validate the same immutable variants required by the native overlay.
   * Geometry must not activate over a failed native-art allocation. */
  for (uint8_t town = 1; town <= kSimTownCount; town++)
    if ((mask & (1u << (town - 1))) && !SimTownGroundArt_Metatile(town,
            slot->sim.world_navigation_towns.ground.development_tier[town - 1], 8)) {
      mask = 0;
      break;
    }
  if (mask && SimWorldNavigationCliffs_Build(mask, &s_world_terrain.cliffs) &&
      s_world_terrain.cliffs.face_count) {
    s_world_terrain.cliff_projection = calloc(s_world_terrain.cliffs.face_count,
        sizeof(*s_world_terrain.cliff_projection));
    if (!s_world_terrain.cliff_projection)
      SimWorldNavigationCliffs_Destroy(&s_world_terrain.cliffs);
  }
  for (size_t i = 0; i < s_world_terrain.cliffs.face_count; i++)
    for (int p = 0; p < 4; p++) {
      const SimWorldNavigationCliffFace *face = &s_world_terrain.cliffs.faces[i];
      SimWorldNavigationGlobe_SampleAtRadius(radius_tiles, face->x[p], face->y[p],
          s_world_terrain.cliff_projection[i].normal[p], NULL);
      s_world_terrain.maximum_height = fmaxf(
          s_world_terrain.maximum_height, face->height[p]);
    }
  s_world_terrain.cliff_mask = mask;
  s_world_terrain.cliff_geography = geography;
  s_world_terrain.cliff_chart_radius_tiles = radius_tiles;
  s_world_terrain.cliffs_ready = true;
  s_world_terrain.cliff_serial++;
  s_world_terrain.projection_ready = false;
  s_world_mountains.samples_ready = false;
  s_world_mountains.projection_ready = false;
}

static void PrepareWorldNavigationTerrain(void) {
  const uint32_t world_serial = SimWorldMap_GeographySerial();
  if (s_world_terrain.ready &&
      s_world_terrain.serial == world_serial)
    return;
  const uint32_t *world_pixels = SimWorldMap_BakedPixels();
  if (world_pixels)
    (void)SimWorldNavigationTerrain_RebuildWorldPrior(
        world_pixels, kSimWorldMapPixels, world_serial);
  s_world_terrain.maximum_height = 0.0f;
  for (int y = 0; y <= kWorldNavigationTerrainCells; y++) {
    for (int x = 0; x <= kWorldNavigationTerrainCells; x++) {
      SimWorldNavigationTerrainHeights sample;
      (void)SimWorldNavigationTerrain_SampleHeights((float)x, (float)y, &sample);
      const int at = WorldNavigationTerrainVertexIndex(x, y);
#if AR_SIM3D_TERRAIN_ELEVATION
      s_world_terrain.height[at] = sample.height_units;
      s_world_terrain.floor[at] = sample.floor_height_units;
      s_world_terrain.authored[at] = sample.authored_weight;
#else
      s_world_terrain.height[at] = 0.0f;
      s_world_terrain.floor[at] = 0.0f;
      s_world_terrain.authored[at] = 0.0f;
#endif
      s_world_terrain.maximum_height = fmaxf(
          s_world_terrain.maximum_height,
          s_world_terrain.height[at]);
    }
  }
  s_world_terrain.ready = true;
  s_world_terrain.serial = world_serial;
}

static float WorldNavigationTerrainHeightAtImpl(float source_x, float source_y,
                                                float *authored_weight, bool floor_only) {
  PrepareWorldNavigationTerrain();
  float tile_x = source_x / (float)kSimWorldMapTilePixels;
  float tile_y = source_y / (float)kSimWorldMapTilePixels;
  if (tile_x < 0.0f) tile_x = 0.0f;
  if (tile_y < 0.0f) tile_y = 0.0f;
  if (tile_x > kWorldNavigationTerrainCells)
    tile_x = kWorldNavigationTerrainCells;
  if (tile_y > kWorldNavigationTerrainCells)
    tile_y = kWorldNavigationTerrainCells;
  int x0 = (int)tile_x, y0 = (int)tile_y;
  int x1 = x0 < kWorldNavigationTerrainCells ? x0 + 1 : x0;
  int y1 = y0 < kWorldNavigationTerrainCells ? y0 + 1 : y0;
  const float u = tile_x - x0, v = tile_y - y0;
  if (!authored_weight && x0 < kWorldNavigationTerrainCells && y0 < kWorldNavigationTerrainCells) {
    const unsigned cap = s_world_terrain.cliffs.replacement[
        y0 * kWorldNavigationTerrainCells + x0];
    if (cap) {
      float h[4];
      memcpy(h, s_world_terrain.cliffs.faces[cap - 1].height, sizeof(h));
      if (floor_only) {
        static const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
        for (int p = 0; p < 4; p++) {
          const int at = WorldNavigationTerrainVertexIndex(x0 + dx[p], y0 + dy[p]);
          h[p] -= s_world_terrain.height[at] - s_world_terrain.floor[at];
        }
      }
      const float north = h[0] + (h[1] - h[0]) * u;
      const float south = h[3] + (h[2] - h[3]) * u;
      return north + (south - north) * v;
    }
  }
  const int nw = WorldNavigationTerrainVertexIndex(x0, y0);
  const int ne = WorldNavigationTerrainVertexIndex(x1, y0);
  const int sw = WorldNavigationTerrainVertexIndex(x0, y1);
  const int se = WorldNavigationTerrainVertexIndex(x1, y1);
  const float *heights = floor_only ? s_world_terrain.floor : s_world_terrain.height;
  const float north = heights[nw] + (heights[ne] - heights[nw]) * u;
  const float south = heights[sw] + (heights[se] - heights[sw]) * u;
  if (authored_weight) {
    const float authored_north = s_world_terrain.authored[nw] +
        (s_world_terrain.authored[ne] -
         s_world_terrain.authored[nw]) * u;
    const float authored_south = s_world_terrain.authored[sw] +
        (s_world_terrain.authored[se] -
         s_world_terrain.authored[sw]) * u;
    *authored_weight = authored_north +
        (authored_south - authored_north) * v;
  }
  return north + (south - north) * v;
}

static float WorldNavigationTerrainHeightAt(float source_x, float source_y,
                                            float *authored_weight) {
  return WorldNavigationTerrainHeightAtImpl(source_x, source_y, authored_weight, false);
}

static bool WorldNavigationFlatOutputPoint(
    const FrameSlot *slot, ArRenderRectI viewport,
    float source_x, float source_y, ArRenderPointF *out) {
  float authentic_x = 0.0f, authentic_y = 0.0f;
  if (!out || !SimWorldNavigationScene_ProjectSource(
          &slot->sim.world_navigation_scene, source_x, source_y,
          &authentic_x, &authentic_y))
    return false;
  *out = WorldNavigationAuthenticToOutput(
      slot, viewport, authentic_x, authentic_y);
  return true;
}

static bool WorldNavigationInspecting(const FrameSlot *slot) {
  return slot->sim_manual_orbit_yaw != 0 || slot->sim_manual_orbit_pitch != 0 ||
      slot->sim_world_inspection_blend > 0;
}

static SimBackgroundBridgeBounds WorldNavigationObjectBounds(
    const SimWorldNavigationTownObject *object);

static float WorldNavigationModelHeightBound(const FrameSlot *slot) {
  if (!slot->sim.world_navigation_models || !slot->sim.background_voxel_enabled)
    return 0.0f;
  const SimWorldNavigationTowns *towns = &slot->sim.world_navigation_towns;
  if (towns->overflow || towns->object_count > kSimWorldNavigationTownObjectCapacity)
    return 0.0f; /* The draw path rejects invalid captures. */
  const bool same_bounds_basis =
      s_world_models.detail == slot->sim.background_voxel_detail &&
      s_world_models.style == slot->sim.background_voxel_style &&
      s_world_models.chart_radius_tiles == WorldNavigationChartRadius(slot);
  if (same_bounds_basis && s_world_models.object_count == towns->object_count &&
      !memcmp(s_world_models.objects, towns->objects,
          towns->object_count * sizeof(towns->objects[0])))
    return s_world_models.maximum_rise;
  float maximum = 0.0f;
  s_world_models.windmills = false;
  for (uint16_t i = 0; i < towns->object_count; i++) {
    const SimWorldNavigationTownObject *object = &towns->objects[i];
    s_world_models.windmills |= object->kind == kSimBackgroundVoxel_Windmill;
    if (!same_bounds_basis || i >= s_world_models.object_count ||
        memcmp(&s_world_models.objects[i], object, sizeof(*object))) {
      const SimBackgroundVoxelProportions *proportions =
          SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object->kind);
      SimBackgroundVoxelModelBounds measured;
      WorldNavigationModelBounds *bound = &s_world_models.bounds[i];
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
      memcpy(&s_world_models.objects[i], object, sizeof(*object));
    }
    maximum = fmaxf(maximum, s_world_models.bounds[i].maximum_rise);
  }
  s_world_models.object_count = towns->object_count;
  s_world_models.detail = slot->sim.background_voxel_detail;
  s_world_models.style = slot->sim.background_voxel_style;
  s_world_models.chart_radius_tiles = WorldNavigationChartRadius(slot);
  s_world_models.maximum_rise = maximum;
  s_world_models.revision++;
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
  const float terrain = s_world_terrain.maximum_height * landscape;
  const SimWorldNavigationAtmosphereHeights atmosphere =
      SimWorldNavigationScene_AtmosphereHeights(terrain +
          (s_world_mountains.active ? s_world_mountains.scene.maximum_rise : 0),
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
  if (!SimWorldNavigationGlobe_BuildFrameAtRadius(out->chart_radius_tiles, focus_x / kSimWorldMapTilePixels,
          focus_y / kSimWorldMapTilePixels, 0, &out->globe_frame)) return false;
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
  Scene3DCamera camera = {
    .tilt_x = (float)slot->sim.projection_pitch_mrad /
        (float)kPermilleScale,
    .tilt_y = (float)slot->sim.projection_yaw_mrad /
        (float)kPermilleScale,
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
  if (!SimWorldNavigationGlobe_BuildFrame(
          focus_x / kSimWorldMapTilePixels,
          focus_y / kSimWorldMapTilePixels, heading, &out->globe_frame))
    return false;
  if (!SimWorldNavigationGlobe_OrbitFrame(&out->globe_frame,
          slot->sim_manual_orbit_yaw, slot->sim_manual_orbit_pitch)) return false;
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
  out->tile_world = sqrtf(tile_area) / (float)viewport.h;
  const float landscape_scale = slot->sim.world_navigation_relief
      ? (float)slot->sim.landscape_height_pct / kPercentScale : 0.0f;
  out->height_world_per_unit = out->tile_world *
      landscape_scale;
  out->reference_height_units = landscape_scale > 0.0f
      ? WorldNavigationTerrainHeightAt(focus_x, focus_y, NULL) : 0.0f;
  out->globe_radius_world = fmaxf(
      0.25f, out->tile_world * kSimWorldNavigationGlobeRadiusTiles);
  const SimWorldNavigationAtmosphereHeights atmosphere =
      SimWorldNavigationScene_AtmosphereHeights(
          s_world_terrain.maximum_height * landscape_scale +
              (s_world_mountains.active
                  ? s_world_mountains.scene.maximum_rise : 0.0f),
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
        s_world_terrain.maximum_height * landscape_scale + model_rise);
    const float support = kSimWorldNavigationGlobeRadiusTiles * (1 - facing_z) +
        envelope - out->reference_height_units * landscape_scale * facing_z;
    const float maximum_scale = support > 0 ? (camera.distance - .25f) / support : out->tile_world;
    out->tile_world = SimWorldNavigationScene_AdventScale(out->tile_world, maximum_scale);
    if (out->tile_world <= 0) return false;
    out->height_world_per_unit = out->tile_world * landscape_scale;
    out->globe_radius_world = fmaxf(.25f, out->tile_world * kSimWorldNavigationGlobeRadiusTiles);
  }
  const float reference_height_world =
      out->reference_height_units * out->height_world_per_unit;
  out->cloud_height_world =
      atmosphere.cloud_tiles * out->tile_world - reference_height_world;
  out->atmosphere_height_world =
      atmosphere.outer_tiles * out->tile_world - reference_height_world;
  if (!isfinite(slot->sim_world_inspection_blend)) return false;
  const float inspection = fminf(1, fmaxf(0, slot->sim_world_inspection_blend));
  if (inspection > 0) {
    const float centre_z = -out->globe_radius_world - reference_height_world;
    const float target_z = centre_z * inspection;
    /* Aim at the planet centre during inspection, not at the town tangent
     * above it. Preserve the planet centre's axial camera distance, so this
     * reframes without a surprise dolly/scale change or overriding zoom.
     * The eye and homogeneous matrix move together; depth, horizon culling,
     * atmosphere and all weather receivers continue to share one camera. */
    camera.distance += target_z * out->matrix[11];
    Scene3D_BuildViewProjection(&camera, viewport.w, viewport.h, out->matrix);
    for (int row = 0; row < 4; row++)
      out->matrix[12 + row] -= out->matrix[8 + row] * target_z;
    for (int i = 0; i < 3; i++)
      out->camera_world[i] = -camera.distance * out->matrix[i * 4 + 3];
    out->camera_world[2] += target_z;
  }
  return true;
}

static bool WorldNavigationSurfaceNormal(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, float normal[3]) {
  (void)slot;
  (void)viewport;
  if (!projection || !normal || !SimWorldNavigationGlobe_SampleAtRadius(
          projection->chart_radius_tiles,
          source_x / kSimWorldMapTilePixels,
          source_y / kSimWorldMapTilePixels, normal, NULL)) return false;
  SimWorldNavigationGlobe_TransformNormal(
      &projection->globe_frame, normal, normal);
  return true;
}

static void WorldNavigationRadialPoint(
    const WorldNavigationProjection *projection, const float normal[3],
    float radial_height, float out[3]) {
  /* Change camera clearance over the focused height, not the planet radius.
   * The sea, mountains and atmosphere remain concentric at every location. */
  const float radius = projection->globe_radius_world +
      projection->reference_height_units * projection->height_world_per_unit;
  out[0] = radius * normal[0];
  out[1] = radius * normal[1];
  out[2] = radius * (normal[2] - 1.0f);
  out[0] += normal[0] * radial_height;
  out[1] += normal[1] * radial_height;
  out[2] += normal[2] * radial_height;
}

static bool WorldNavigationSurfaceWorldPoint(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, bool terrain,
    float height_offset_world, float out[3]) {
  float normal[3];
  if (!out || !WorldNavigationSurfaceNormal(
          slot, viewport, projection, source_x, source_y, normal)) return false;
  float radial_height = height_offset_world;
  if (terrain && projection->height_world_per_unit > 0.0f) {
    const float height = WorldNavigationTerrainHeightAt(
        source_x, source_y, NULL);
    radial_height += (height - projection->reference_height_units) *
        projection->height_world_per_unit;
  }
  WorldNavigationRadialPoint(projection, normal, radial_height, out);
  return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static bool WorldNavigationProjectSurface(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, bool terrain,
    float height_offset_world, ArRenderPointF *out) {
  float world[3];
  Scene3DPoint projected;
  if (!out || !WorldNavigationSurfaceWorldPoint(
          slot, viewport, projection, source_x, source_y, terrain,
          height_offset_world, world) ||
      !Scene3D_ProjectWorldPoint(
          projection->matrix, world[0], world[1], world[2],
          viewport.w, viewport.h, &projected))
    return false;
  *out = (ArRenderPointF){viewport.x + projected.x,
                         viewport.y + projected.y};
  return true;
}

static float WorldNavigationSurfaceShade(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const float light[3], float source_x, float source_y) {
  if (!slot->sim.world_navigation_lighting) return 1.0f;
  const float step = (float)kSimWorldMapTilePixels * 0.5f;
  float centre[3], east[3], south[3];
  if (!WorldNavigationSurfaceWorldPoint(
          slot, viewport, projection, source_x, source_y, true, 0.0f,
          centre) ||
      !WorldNavigationSurfaceWorldPoint(
          slot, viewport, projection, source_x + step, source_y, true,
          0.0f, east) ||
      !WorldNavigationSurfaceWorldPoint(
          slot, viewport, projection, source_x, source_y + step, true,
          0.0f, south))
    return 1.0f;
  const float tx[3] = {east[0] - centre[0], east[1] - centre[1],
                       east[2] - centre[2]};
  const float ty[3] = {south[0] - centre[0], south[1] - centre[1],
                       south[2] - centre[2]};
  float normal[3] = {
    ty[1] * tx[2] - ty[2] * tx[1],
    ty[2] * tx[0] - ty[0] * tx[2],
    ty[0] * tx[1] - ty[1] * tx[0],
  };
  const float normal_length = sqrtf(
      normal[0] * normal[0] + normal[1] * normal[1] +
      normal[2] * normal[2]);
  if (normal_length <= 0.0f) return 1.0f;
  for (int i = 0; i < 3; i++) normal[i] /= normal_length;
  float diffuse = normal[0] * light[0] + normal[1] * light[1] +
      normal[2] * light[2];
  if (diffuse < 0.0f) diffuse = 0.0f;
  return kWorldNavigationTerrainAmbient +
      (1.0f - kWorldNavigationTerrainAmbient) * diffuse;
}

static void PrepareWorldNavigationOceanIndices(void) {
  if (s_world_shells.indices_ready) return;
  int index_count = 0;
  for (int sector = 0; sector < kWorldNavigationOceanSectors; sector++) {
    const int next = (sector + 1) % kWorldNavigationOceanSectors;
    s_world_shells.indices[index_count++] = 0;
    s_world_shells.indices[index_count++] = 1 + sector;
    s_world_shells.indices[index_count++] = 1 + next;
  }
  for (int ring = 1; ring < kWorldNavigationOceanRings; ring++) {
    const int inner = 1 + (ring - 1) * kWorldNavigationOceanSectors;
    const int outer = inner + kWorldNavigationOceanSectors;
    for (int sector = 0; sector < kWorldNavigationOceanSectors; sector++) {
      const int next = (sector + 1) % kWorldNavigationOceanSectors;
      s_world_shells.indices[index_count++] = inner + sector;
      s_world_shells.indices[index_count++] = outer + sector;
      s_world_shells.indices[index_count++] = outer + next;
      s_world_shells.indices[index_count++] = inner + sector;
      s_world_shells.indices[index_count++] = outer + next;
      s_world_shells.indices[index_count++] = inner + next;
    }
  }
  s_world_shells.indices_ready = true;
}


/* Ocean closes the entire sphere, including the uncharted hemisphere.
 * The atmospheric silhouette is an exact camera-tangent cap on a larger
 * concentric sphere. It is background color, not an opaque occluder. */
static bool DrawWorldNavigationSphereShell(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection, WorldNavigationShell kind) {
  const bool atmosphere = kind == kWorldNavigationShell_Atmosphere;
  const bool cloud = kind == kWorldNavigationShell_Cloud;
  PrepareWorldNavigationOceanIndices();
  const float reference = projection->reference_height_units *
      projection->height_world_per_unit;
  const float shell_height = atmosphere
      ? projection->atmosphere_height_world
      : cloud ? projection->cloud_height_world
      : -reference - projection->globe_radius_world * 0.0025f;
  const float radius = projection->globe_radius_world + reference + shell_height;
  const float centre_z = -projection->globe_radius_world - reference;
  float outward[3] = {
    projection->camera_world[0], projection->camera_world[1],
    projection->camera_world[2] - centre_z,
  };
  const float eye_distance = hypotf(hypotf(outward[0], outward[1]), outward[2]);
  if (!isfinite(eye_distance) || eye_distance <= radius) return false;
  for (int i = 0; i < 3; i++) outward[i] /= eye_distance;
  float right[3] = {outward[2], 0, -outward[0]};
  float length = hypotf(right[0], right[2]);
  if (length < 0.0001f) {
    right[0] = 1; right[2] = 0; length = 1;
  }
  for (int i = 0; i < 3; i++) right[i] /= length;
  const float up[3] = {
    outward[1] * right[2] - outward[2] * right[1],
    outward[2] * right[0] - outward[0] * right[2],
    outward[0] * right[1] - outward[1] * right[0],
  };
  const float maximum_angle = atmosphere || cloud ? acosf(radius / eye_distance) : kPi;
  static Sim3DDepthVertex depth_vertices[kWorldNavigationOceanVertexCount];
  static Scene3DClipPoint clip_vertices[kWorldNavigationOceanVertexCount];
  int vertex_count = 0;
  for (int ring = 0; ring <= kWorldNavigationOceanRings; ring++) {
    const float radial = ring / (float)kWorldNavigationOceanRings;
    const float angle = radial * maximum_angle;
    const float sine = sinf(angle), cosine = cosf(angle);
    float atmosphere_alpha = 0, cloud_alpha = 1;
    if (atmosphere || cloud) {
      /* All sectors of a camera-tangent ring share this view ray. Its
       * closest approach measures apparent altitude, unlike an arbitrary
       * fraction of cap angle (which bunches up at the projected rim).
       * Compute the profiles once per ring, not per vertex or fragment. */
      const float ray_length = hypotf(radius * sine, eye_distance - radius * cosine);
      if (atmosphere) {
        const float impact = eye_distance * radius * sine / ray_length;
        atmosphere_alpha = SimWorldNavigationScene_AtmosphereOpacity(
            (impact - projection->globe_radius_world) /
            (radius - projection->globe_radius_world));
      } else {
        cloud_alpha = SimWorldNavigationScene_CloudLimbOpacity(
            (eye_distance * cosine - radius) / ray_length);
      }
    }
    const int sectors = ring ? kWorldNavigationOceanSectors : 1;
    for (int sector = 0; sector < sectors; sector++) {
      const float longitude = 2.0f * kPi * sector / kWorldNavigationOceanSectors;
      const float cx = cosf(longitude), sy = sinf(longitude);
      float world[3];
      for (int i = 0; i < 3; i++)
        world[i] = radius * (cosine * outward[i] +
            sine * (cx * right[i] + sy * up[i]));
      world[2] += centre_z;
      Scene3DPoint output;
      if (!WorldNavigationProjectPoint(projection, viewport, world, &output,
              &depth_vertices[vertex_count].depth, &clip_vertices[vertex_count])) return false;
      const float light = 0.72f + fmaxf(0, cosine) * 0.22f;
      const ArRenderColorF colour = atmosphere
          ? (ArRenderColorF){0.28f, 0.56f, 1.0f, atmosphere_alpha}
          : (ArRenderColorF){0.05f * light, 0.15f * light, 0.84f * light, 1.0f};
      depth_vertices[vertex_count].x = output.x;
      depth_vertices[vertex_count].y = output.y;
      depth_vertices[vertex_count].color = colour;
      depth_vertices[vertex_count].uv = (ArRenderPointF){-1, -1};
      if (!atmosphere) {
        WorldNavigationShellGeometry *geometry = cloud
            ? &s_world_shells.cloud : &s_world_shells.ocean;
        geometry->points[vertex_count] = depth_vertices[vertex_count];
        if (projection->clip_frustum) geometry->clip[vertex_count] = clip_vertices[vertex_count];
        geometry->outside[vertex_count] = projection->clip_frustum
            ? WorldNavigationClipOutside(clip_vertices[vertex_count])
            : WorldNavigationViewportOutside(output.x, output.y, viewport.w, viewport.h);
        const float normal[3] = {world[0] / radius, world[1] / radius,
                                (world[2] - centre_z) / radius};
        for (int i = 0; i < 3; i++)
          geometry->normal[vertex_count][i] =
              projection->globe_frame.right[i] * normal[0] +
              projection->globe_frame.up[i] * normal[1] +
              projection->globe_frame.outward[i] * normal[2];
        geometry->alpha[vertex_count] = cloud ? cloud_alpha : 1;
        geometry->front[vertex_count] =
            normal[0] * (projection->camera_world[0] - world[0]) +
            normal[1] * (projection->camera_world[1] - world[1]) +
            normal[2] * (projection->camera_world[2] - world[2]) > 0;
      }
      s_world_shells.vertices[vertex_count++] =
          (ArRenderVertex2D){{output.x, output.y}, colour, {0, 0}};
    }
  }
  if (cloud) return true;
  if (atmosphere) {
    const ArRenderDrawState state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Alpha,
    };
    if (projection->clip_frustum) {
      enum { kBatch = 64 };
      ArRenderVertex2D vertices[kBatch * 4];
      int32_t indices[kBatch * 6];
      for (int i = 0; i < kBatch; i++) {
        const int corners[6] = {0, 1, 2, 0, 2, 3};
        for (int p = 0; p < 6; p++) indices[i * 6 + p] = i * 4 + corners[p];
      }
      size_t used = 0;
      for (int i = 0; i < kWorldNavigationOceanIndexCount; i += 3) {
        Sim3DDepthVertex input[4], clipped[kWorldNavigationClippedQuads * 4];
        Scene3DClipPoint clip[4];
        for (int p = 0; p < 4; p++) {
          const int at = s_world_shells.indices[i + (p < 3 ? p : 2)];
          input[p] = depth_vertices[at]; clip[p] = clip_vertices[at];
        }
        size_t count;
        if (!WorldNavigationClipQuad(input, clip, viewport, clipped, &count)) return false;
        for (size_t q = 0; q < count; q++) {
          for (int p = 0; p < 4; p++) {
            const Sim3DDepthVertex *v = &clipped[q * 4 + p];
            vertices[used * 4 + p] = (ArRenderVertex2D){{v->x, v->y}, v->color, {0, 0}};
          }
          if (++used == kBatch) {
            if (!ArRenderDevice_DrawGeometryWithState(&g_render_device,
                    ArRenderTexture_Invalid(), vertices, (int)used * 4,
                    indices, (int)used * 6, &state)) return false;
            used = 0;
          }
        }
      }
      return !used || ArRenderDevice_DrawGeometryWithState(&g_render_device,
          ArRenderTexture_Invalid(), vertices, (int)used * 4, indices, (int)used * 6, &state);
    }
    return ArRenderDevice_DrawGeometryWithState(
        &g_render_device, ArRenderTexture_Invalid(),
        s_world_shells.vertices, vertex_count,
        s_world_shells.indices,
        kWorldNavigationOceanIndexCount, &state);
  }
  /* Bound stack use while amortizing backend reservation/conversion calls.
   * Keep each original triangle, including its duplicate fourth corner,
   * and the original submission order. No backend storage is borrowed. */
  enum { kOceanBatchQuads = 64 };
  Sim3DDepthVertex batch[kOceanBatchQuads * 4];
  Scene3DClipPoint clip_batch[kOceanBatchQuads * 4];
  size_t batch_count = 0;
  if (projection->clip_frustum) {
    /* Match the shadow receiver's quad topology before clipping. Two ways
     * of triangulating a clipped planar ocean patch can round intersection
     * depths differently; opaque and weather must reuse the same vertices. */
    for (int y = 0; y < kWorldNavigationOceanRings; y++) {
      for (int x = 0; x < kWorldNavigationOceanSectors; x++) {
        const int next = (x + 1) % kWorldNavigationOceanSectors;
        const int at[4] = {
          y ? 1 + (y - 1) * kWorldNavigationOceanSectors + x : 0,
          y ? 1 + (y - 1) * kWorldNavigationOceanSectors + next : 0,
          1 + y * kWorldNavigationOceanSectors + next,
          1 + y * kWorldNavigationOceanSectors + x,
        };
        for (int p = 0; p < 4; p++) {
          batch[batch_count * 4 + p] = depth_vertices[at[p]];
          clip_batch[batch_count * 4 + p] = clip_vertices[at[p]];
        }
        if (++batch_count == kOceanBatchQuads) {
          if (!WorldNavigationAppendProjectedQuads(kSim3DDepthPass_Ground,
                  batch, clip_batch, batch_count, viewport)) return false;
          batch_count = 0;
        }
      }
    }
    return !batch_count || WorldNavigationAppendProjectedQuads(kSim3DDepthPass_Ground,
        batch, clip_batch, batch_count, viewport);
  }
  for (int i = 0; i < kWorldNavigationOceanIndexCount; i += 3) {
    Sim3DDepthVertex *triangle = batch + batch_count * 4;
    for (int corner = 0; corner < 3; corner++) {
      triangle[corner] = depth_vertices[s_world_shells.indices[i + corner]];
    }
    triangle[3] = triangle[2];
    if (++batch_count == kOceanBatchQuads) {
      if (!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Ground, batch, batch_count))
        return false;
      batch_count = 0;
    }
  }
  return !batch_count ||
      Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Ground, batch, batch_count);
}
static bool DrawWorldNavigationSpaceBackdrop(ArRenderRectI viewport) {
  enum { kStarCount = 256, kVertexCount = 4 + kStarCount * 4,
         kIndexCount = 6 + kStarCount * 6 };
  ArRenderVertex2D vertices[kVertexCount];
  int32_t indices[kIndexCount];
  const float left = viewport.x, top = viewport.y;
  const float right = left + viewport.w, bottom = top + viewport.h;
  const ArRenderColorF zenith = {0.003f, 0.006f, 0.020f, 1.0f};
  const ArRenderColorF nadir = {0.013f, 0.021f, 0.052f, 1.0f};
  vertices[0] = (ArRenderVertex2D){{left, top}, zenith, {0, 0}};
  vertices[1] = (ArRenderVertex2D){{right, top}, zenith, {0, 0}};
  vertices[2] = (ArRenderVertex2D){{right, bottom}, nadir, {0, 0}};
  vertices[3] = (ArRenderVertex2D){{left, bottom}, nadir, {0, 0}};
  for (int star = 0; star < kStarCount; star++) {
    /* Stable, sparse pixel-art diamonds. No per-frame randomness or sparkle;
     * the planet and its atmosphere occlude these through normal draw order. */
    const uint32_t position = DeterministicHash_Mix32((uint32_t)star + 0x519A3u);
    const uint32_t style = DeterministicHash_Mix32(position ^ 0xB391u);
    const float x = left + ((position & 0xFFFFu) + 0.5f) / 65536.0f * viewport.w;
    const float y = top + ((position >> 16) + 0.5f) / 65536.0f * viewport.h;
    const float value = 0.20f + (style & 255u) / 255.0f * 0.54f;
    const float size = fmaxf(0.75f, viewport.h / 900.0f) *
        (star % 19 == 0 ? 1.5f : 0.70f);
    const bool warm = (style & 0x100u) != 0;
    const ArRenderColorF colour = {
      value * (warm ? 1.0f : 0.76f), value * 0.88f,
      value * (warm ? 0.76f : 1.0f), 1.0f,
    };
    const int at = 4 + star * 4;
    vertices[at] = (ArRenderVertex2D){{x, y - size}, colour, {0, 0}};
    vertices[at + 1] = (ArRenderVertex2D){{x + size, y}, colour, {0, 0}};
    vertices[at + 2] = (ArRenderVertex2D){{x, y + size}, colour, {0, 0}};
    vertices[at + 3] = (ArRenderVertex2D){{x - size, y}, colour, {0, 0}};
  }
  for (int quad = 0; quad <= kStarCount; quad++) {
    const int at = quad * 4, index = quad * 6;
    indices[index] = at;
    indices[index + 1] = at + 1;
    indices[index + 2] = at + 2;
    indices[index + 3] = at;
    indices[index + 4] = at + 2;
    indices[index + 5] = at + 3;
  }
  return ArRenderDevice_DrawGeometry(
      &g_render_device, ArRenderTexture_Invalid(), vertices, kVertexCount,
      indices, kIndexCount);
}


static bool WorldNavigationAppendGroundLayer(
    Sim3DDepthPassLayer layer, const ArRenderVertex2D *colours,
    const WorldNavigationProjection *projection, ArRenderRectI viewport) {
  for (int y = 0; y < kWorldNavigationTerrainCells; y++)
    for (int x = 0; x < kWorldNavigationTerrainCells; x++) {
      if (s_world_terrain.cliffs.replacement[y * kWorldNavigationTerrainCells + x])
        continue;
      const int at = WorldNavigationTerrainVertexIndex(x, y);
      const int corners[4] = {at, at + 1, at + kWorldNavigationTerrainAxis + 1,
                             at + kWorldNavigationTerrainAxis};
      /* The same conservative edge test already guards weather receivers.
       * Reject before attribute staging; retain every straddling face. */
      if (s_world_terrain.outside[corners[0]] & s_world_terrain.outside[corners[1]] &
          s_world_terrain.outside[corners[2]] & s_world_terrain.outside[corners[3]]) continue;
      Sim3DDepthVertex face[4];
      Scene3DClipPoint clip[4];
      for (int i = 0; i < 4; i++) {
        const int vertex = corners[i];
        face[i] = s_world_terrain.depth[vertex];
        face[i].color = colours[vertex].color;
        face[i].uv = layer == kSim3DDepthPass_GroundHaze
            ? (ArRenderPointF){-1.0f, -1.0f} : colours[vertex].tex_coord;
        if (projection->clip_frustum) clip[i] = s_world_terrain.clip[vertex];
      }
      if (!WorldNavigationAppendProjectedQuad(layer, face,
              projection->clip_frustum ? clip : NULL, viewport)) return false;
    }
  return true;
}

static bool WorldNavigationAppendCliffLayer(Sim3DDepthPassLayer layer, const FrameSlot *slot,
    const WorldNavigationProjection *projection, ArRenderRectI viewport) {
  Sim3DDepthVertex batch[64 * 4];
  Scene3DClipPoint clip[64 * 4];
  size_t count = 0;
  for (size_t i = 0; i < s_world_terrain.cliffs.face_count; i++) {
    const SimWorldNavigationCliffFace *face = &s_world_terrain.cliffs.faces[i];
    const WorldNavigationCliffProjection *projected = &s_world_terrain.cliff_projection[i];
    for (int p = 0; p < 4; p++) {
      Sim3DDepthVertex *v = &batch[count * 4 + p];
      *v = projected->depth[p];
      if (projection->clip_frustum) clip[count * 4 + p] = projected->clip[p];
      v->color = projected->colour[p];
      v->uv = (ArRenderPointF){face->u[p], face->v[p]};
      if (layer != kSim3DDepthPass_Ground) {
        const float haze = SimWorldNavigationScene_LocationHaze(
            &slot->sim.world_navigation_scene, face->x[p] * kSimWorldMapTilePixels,
            face->y[p] * kSimWorldMapTilePixels, fmaxf(1, slot->sim.cull_haze_lead_px * .5f));
        if (layer == kSim3DDepthPass_GroundBlur)
          v->color.a *= haze * slot->sim.underlay_defocus_pct / (float)kPercentScale;
        else {
          v->color = (ArRenderColorF){.24f, .37f, .56f,
              v->color.a * haze * slot->sim.underlay_haze_pct / (float)kPercentScale * .35f};
          v->uv = (ArRenderPointF){-1, -1};
        }
      }
    }
    if (++count == 64) {
      if (!WorldNavigationAppendProjectedQuads(layer, batch,
              projection->clip_frustum ? clip : NULL, count, viewport)) return false;
      count = 0;
    }
  }
  return !count || WorldNavigationAppendProjectedQuads(layer, batch,
      projection->clip_frustum ? clip : NULL, count, viewport);
}

static bool DrawWorldNavigationGround(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  if (!scene->valid ||
      slot->visible_width <= 0 ||
      slot->snes_height <= 0)
    return false;

  if (projection->height_world_per_unit > 0.0f) PrepareWorldNavigationTerrain();
  WorldNavigationGroundKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection;
  memcpy(key.source_to_screen, scene->source_to_screen,
         sizeof(key.source_to_screen));
  key.viewport = viewport;
  key.geography_serial = s_world_terrain.serial;
  key.snes_width = slot->snes_width;
  key.snes_height = slot->snes_height;
  key.visible_width = slot->visible_width;
  key.visible_x0 = slot->visible_x0;
  key.light_azimuth = slot->sim.light_azimuth_deg;
  key.light_elevation = slot->sim.light_elevation_deg;
  key.lighting = slot->sim.world_navigation_lighting;
  if (!s_world_terrain.projection_ready ||
      memcmp(&key, &s_world_terrain.projection_key, sizeof(key)) != 0) {
    s_world_terrain.projection_ready = false;
    /* Ground and cliff samples share one captured sun direction. Keep the
     * original arithmetic, but evaluate it once per projection/light key. */
    float light[3] = {0};
    if (slot->sim.world_navigation_lighting) {
      const float azimuth =
          (float)slot->sim.light_azimuth_deg * kPi / 180.0f;
      const float elevation =
          (float)slot->sim.light_elevation_deg * kPi / 180.0f;
      const float horizontal = cosf(elevation);
      light[0] = -cosf(azimuth) * horizontal;
      light[1] = -sinf(azimuth) * horizontal;
      light[2] = sinf(elevation);
    }
    for (int y = 0; y <= kWorldNavigationTerrainCells; y++) {
      for (int x = 0; x <= kWorldNavigationTerrainCells; x++) {
        const float source_x = x * (float)kSimWorldMapTilePixels;
        const float source_y = y * (float)kSimWorldMapTilePixels;
        const int at = WorldNavigationTerrainVertexIndex(x, y);
        float world[3];
        Scene3DPoint output;
        if (!WorldNavigationSurfaceWorldPoint(
                slot, viewport, projection, source_x, source_y, true, 0.0f, world) ||
            !WorldNavigationProjectPoint(projection, viewport, world, &output,
                &s_world_terrain.depth[at].depth,
                &s_world_terrain.clip[at]))
          return false;
        s_world_terrain.depth[at].x = output.x;
        s_world_terrain.depth[at].y = output.y;
        s_world_terrain.outside[at] = projection->clip_frustum
            ? WorldNavigationClipOutside(s_world_terrain.clip[at])
            : WorldNavigationViewportOutside(output.x, output.y, viewport.w, viewport.h);
        s_world_terrain.depth[at].uv = (ArRenderPointF){-1.0f, -1.0f};
        const float shade = WorldNavigationSurfaceShade(
            slot, viewport, projection, light, source_x, source_y);
        const float edge_tiles = fminf(
            fminf((float)x, (float)y),
            fminf((float)(kWorldNavigationTerrainCells - x),
                  (float)(kWorldNavigationTerrainCells - y)));
        const float edge_alpha = WorldNavigationSmoothstep(edge_tiles / 10.0f);
        s_world_terrain.vertices[
            WorldNavigationTerrainVertexIndex(x, y)] = (ArRenderVertex2D){
          {output.x, output.y},
          {shade, shade, shade, edge_alpha},
          {x / (float)kWorldNavigationTerrainCells,
           y / (float)kWorldNavigationTerrainCells},
        };
      }
    }
    for (size_t i = 0; i < s_world_terrain.cliffs.face_count; i++) {
      const SimWorldNavigationCliffFace *face = &s_world_terrain.cliffs.faces[i];
      WorldNavigationCliffProjection *projected = &s_world_terrain.cliff_projection[i];
      for (int p = 0; p < 4; p++) {
        float normal[3], world[3];
        Scene3DPoint screen;
        SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, projected->normal[p], normal);
        WorldNavigationRadialPoint(projection, normal,
            (face->height[p] - projection->reference_height_units) * projection->height_world_per_unit, world);
        if (!WorldNavigationProjectPoint(projection, viewport, world,
                &screen, &projected->depth[p].depth, &projected->clip[p])) return false;
        projected->depth[p].x = screen.x; projected->depth[p].y = screen.y;
        projected->outside[p] = projection->clip_frustum ? WorldNavigationClipOutside(projected->clip[p])
            : WorldNavigationViewportOutside(screen.x, screen.y, viewport.w, viewport.h);
        const float shade = face->shade * WorldNavigationSurfaceShade(slot, viewport, projection,
            light, face->x[p] * kSimWorldMapTilePixels, face->y[p] * kSimWorldMapTilePixels);
        projected->colour[p] = (ArRenderColorF){shade, shade, shade, 1};
      }
    }
    s_world_terrain.projection_key = key;
    s_world_terrain.projection_ready = true;
  }
  return WorldNavigationAppendGroundLayer(
      kSim3DDepthPass_Ground, s_world_terrain.vertices, projection, viewport) &&
      WorldNavigationAppendCliffLayer(kSim3DDepthPass_Ground, slot, projection, viewport);
}

/* Towns use the same authored model compiler, proportions and material
 * palettes as the full-town renderer. Only their projection changes. The
 * common depth pass also hides rear facades and objects behind the terrain. */
static SimBackgroundBridgeBounds WorldNavigationObjectBounds(
    const SimWorldNavigationTownObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge)
    return SimBackgroundBridge_ResolveBounds(object);
  return (SimBackgroundBridgeBounds){
    .origin_x = object->cell_x * kSimTownCellPixels,
    .origin_y = (object->cell_y + object->source_cells_h -
                 object->footprint_cells_d) * kSimTownCellPixels,
    .width = object->footprint_cells_w * kSimTownCellPixels,
    .depth = object->footprint_cells_d * kSimTownCellPixels,
  };
}

/* Retain only successfully projected portable values. Allocation/budget
 * failure disables capture, not drawing, and never publishes a partial model
 * set. This cache owns no model-cache pointer or backend allocation. */
static bool WorldNavigationAppendModelQuad(const Sim3DDepthVertex input[4],
    const Scene3DClipPoint *clip, ArRenderRectI viewport) {
  if (!s_world_models.capturing)
    return WorldNavigationAppendProjectedQuad(kSim3DDepthPass_Solid, input, clip, viewport);
  Sim3DDepthVertex clipped[kWorldNavigationClippedQuads * 4];
  const Sim3DDepthVertex *vertices = input;
  size_t quads = 1;
  if (clip) {
    if (!WorldNavigationClipQuad(input, clip, viewport, clipped, &quads)) return false;
    vertices = clipped;
  }
  if (!quads) return true;
  if (!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Solid, vertices, quads)) return false;
  const size_t needed = s_world_models.projected_count + quads * 4;
  enum { kMaximumVertices = (8 * 1024 * 1024 / sizeof(Sim3DDepthVertex)) & ~3u };
  if (needed > kMaximumVertices) {
    s_world_models.capturing = false;
    s_world_models.projection_unavailable = true; /* Do not recopy an oversized view every frame. */
    return true;
  }
  if (needed > s_world_models.projected_capacity) {
    size_t capacity = s_world_models.projected_capacity ? s_world_models.projected_capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > kMaximumVertices) capacity = kMaximumVertices;
    void *points = realloc(s_world_models.projected, capacity * sizeof(*s_world_models.projected));
    if (!points) {
      s_world_models.capturing = false;
      s_world_models.projection_unavailable = true; /* Retry only on reset. */
      return true;
    }
    s_world_models.projected = points;
    s_world_models.projected_capacity = capacity;
  }
  memcpy(s_world_models.projected + s_world_models.projected_count, vertices,
      quads * 4 * sizeof(*vertices));
  s_world_models.projected_count = needed;
  return true;
}

static bool WorldNavigationAppendAuthoredModel(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const WorldNavigationVisibleTownObject *visible) {
  SimBackgroundVoxelObject object = *visible->object;
  /* Windmills retain the native three-position model family. Navigation has
   * no live town tilemap, so its captured game clock supplies the phase. */
  if (object.kind == kSimBackgroundVoxel_Windmill)
    object.animation_phase = (uint8_t)((slot->sim.game_frame / 12) % 3);
  if (object.kind >= kSimBackgroundVoxelKindCount) return true;
  const SimBackgroundVoxelBiome biome =
      SimBackgroundVoxelBiome_ForTown(object.town);
  const SimBackgroundVoxelModelShadingKey light = {
    .light_azimuth_deg = slot->sim.light_azimuth_deg,
    .light_elevation_deg = slot->sim.light_elevation_deg,
    .shading = kSimBackgroundVoxelShading_AmbientOcclusion,
    .biome = (uint8_t)biome,
  };
  const SimBackgroundVoxelModelShading *shading = NULL;
  Sim3DPerformanceScope compile =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthVoxel);
  const SimBackgroundVoxelModelView *model = SimBackgroundVoxelModelCache_Get(
      &object, visible->detail,
      (SimBackgroundVoxelStyle)slot->sim.background_voxel_style,
      slot->sim.world_navigation_lighting ? &light : NULL, &shading);
  Sim3DPerformance_End(compile);
  if (!model || (slot->sim.world_navigation_lighting && !shading) ||
      model->overflow || !model->face_count)
    return false;
  SimBackgroundVoxelPalette palette;
  SimBackgroundVoxelPalette_Build(&object, biome, &palette);
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object.kind);
  const SimBackgroundBridgeBounds bounds = WorldNavigationObjectBounds(&object);
  int town_x, town_y;
  if (!SimWorldMap_OriginForTown(object.town, &town_x, &town_y)) return false;
  const float pixel_to_world_source =
      (float)kSimWorldMapTilePixels / kSimTownCellPixels;
  const float source_x = town_x * kSimWorldMapTilePixels +
      bounds.origin_x * pixel_to_world_source;
  const float source_y = town_y * kSimWorldMapTilePixels +
      bounds.origin_y * pixel_to_world_source;
  const float centre_x = bounds.width * 0.5f;
  const float centre_y = bounds.depth * 0.5f;
  const float anchor_x = source_x + centre_x * pixel_to_world_source;
  const float anchor_y = source_y + centre_y * pixel_to_world_source;
  const float anchor_height = projection->height_world_per_unit > 0.0f
      ? WorldNavigationTerrainHeightAt(anchor_x, anchor_y, NULL) : 0.0f;
  const float base = (anchor_height - projection->reference_height_units) *
      projection->height_world_per_unit;
  float anchor_normal[3], local_scale;
  if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles,
          anchor_x / kSimWorldMapTilePixels, anchor_y / kSimWorldMapTilePixels,
          anchor_normal, &local_scale)) return false;
  const float height_scale = local_scale * projection->tile_world / kSimTownCellPixels *
      proportions->height_scale *
      slot->sim.height_scale_x100 / (float)kPercentScale;
  /* Bound the actual compiled geometry, including overhanging roofs, blades
   * and tree crowns. The chart's metric is at most one, so the flat source
   * diagonal / globe radius bounds every column's angular displacement.
   * Use an inscribed sphere below the inset, faceted ocean shell; testing
   * against the nominal sea radius could remove a visible limb silhouette. */
  const float extent_x = fmaxf(fabsf(model->min_x - centre_x), fabsf(model->max_x - centre_x));
  const float extent_y = fmaxf(fabsf(model->min_y - centre_y), fabsf(model->max_y - centre_y));
  const float angular_radius = hypotf(extent_x, extent_y) * proportions->footprint_scale /
      (kSimTownCellPixels * projection->chart_radius_tiles);
  const float maximum_radius = projection->globe_radius_world +
      anchor_height * projection->height_world_per_unit + model->max_z * height_scale;
  float transformed_anchor[3];
  SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, anchor_normal, transformed_anchor);
  const float camera[3] = {projection->camera_world[0], projection->camera_world[1],
      projection->camera_world[2] + projection->globe_radius_world +
          projection->reference_height_units * projection->height_world_per_unit};
  const float occluder_radius = projection->globe_radius_world * 0.9975f *
      cosf(kPi / kWorldNavigationOceanRings + 2 * kPi / kWorldNavigationOceanSectors);
  if (SimWorldNavigationGlobe_CapOccluded(camera, transformed_anchor,
          angular_radius, maximum_radius, occluder_radius)) return true;
  /* Facades, roof steps and foliage repeatedly use the same XY columns at
   * different heights. Evaluate the expensive globe normal once per column,
   * without approximating curvature or merging any authored geometry. */
  enum { kColumnCacheCount = 512 };
  typedef struct ColumnProjection {
    uint32_t stamp, x_bits, y_bits;
    float normal[3];
  } ColumnProjection;
  static ColumnProjection columns[kColumnCacheCount];
  static uint32_t column_stamp;
  if (++column_stamp == 0) {
    memset(columns, 0, sizeof(columns));
    column_stamp = 1;
  }
  Sim3DPerformanceScope project =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  for (uint16_t face = 0; face < model->face_count; face++) {
    const SimBackgroundVoxelModelFace *authored = &model->faces[face];
    const SimBackgroundVoxelMaterial material =
        shading ? (SimBackgroundVoxelMaterial)shading->material[face]
        : SimBackgroundVoxelBiome_SurfaceMaterial(
            biome, visible->detail,
            (SimBackgroundVoxelMaterial)authored->material, authored);
    const uint32_t argb = SimBackgroundVoxelPalette_Base(&palette, material);
    Sim3DDepthVertex vertices[4] = {0};
    Scene3DClipPoint clip[4];
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      const SimBackgroundVoxelModelPoint *p = &authored->points[point];
      const float x = centre_x +
          (p->x - centre_x) * proportions->footprint_scale;
      const float y = centre_y +
          (p->y - centre_y) * proportions->footprint_scale;
      uint32_t x_bits, y_bits;
      memcpy(&x_bits, &p->x, sizeof(x_bits));
      memcpy(&y_bits, &p->y, sizeof(y_bits));
      const uint32_t column_hash = DeterministicHash_Mix32(
          x_bits ^ DeterministicHash_Mix32(y_bits));
      ColumnProjection *column = &columns[column_hash & (kColumnCacheCount - 1)];
      if (column->stamp != column_stamp || column->x_bits != x_bits ||
          column->y_bits != y_bits) {
        if (!WorldNavigationSurfaceNormal(
                slot, viewport, projection,
                source_x + x * pixel_to_world_source,
                source_y + y * pixel_to_world_source, column->normal)) {
          valid = false;
          break;
        }
        column->stamp = column_stamp;
        column->x_bits = x_bits;
        column->y_bits = y_bits;
      }
      float world[3];
      WorldNavigationRadialPoint(
          projection, column->normal, base + p->z * height_scale, world);
      Scene3DPoint projected;
      if (!WorldNavigationProjectPoint(projection, viewport, world,
              &projected, &vertices[point].depth, &clip[point])) {
        valid = false;
        break;
      }
      vertices[point].x = projected.x;
      vertices[point].y = projected.y;
      vertices[point].uv = (ArRenderPointF){-1.0f, -1.0f};
      /* Retain palette identity with a restrained continuous light response.
       * Clamping the brightest response prevents tiny distant metal, trim
       * and roof faces from sparkling against the original map artwork. */
      const float shade = slot->sim.world_navigation_lighting
          ? 0.74f + 0.18f * shading->brightness[face][point] / 255.0f
          : 0.88f;
      vertices[point].color = (ArRenderColorF){
        ((argb >> 16) & 255) / 255.0f * shade,
        ((argb >> 8) & 255) / 255.0f * shade,
        (argb & 255) / 255.0f * shade,
        (argb >> 24) / 255.0f,
      };
    }
    if (valid && !WorldNavigationAppendModelQuad(vertices,
            projection->clip_frustum ? clip : NULL, viewport)) {
      Sim3DPerformance_End(project);
      return false;
    }
  }
  Sim3DPerformance_End(project);
  return true;
}

/* Use local projected area for distance selection: a tile at the globe limb
 * occupies much less screen area than the tile under the Palace. */
static bool WorldNavigationTownFootprintPixels(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, float *out_pixels,
    ArRenderPointF *out_centre) {
  ArRenderPointF centre, east, south;
  if (!out_pixels ||
      !WorldNavigationProjectSurface(
          slot, viewport, projection, source_x, source_y,
          true, 0.0f, &centre) ||
      !WorldNavigationProjectSurface(
          slot, viewport, projection,
          source_x + kSimWorldMapTilePixels, source_y,
          true, 0.0f, &east) ||
      !WorldNavigationProjectSurface(
          slot, viewport, projection,
          source_x, source_y + kSimWorldMapTilePixels,
          true, 0.0f, &south))
    return false;
  const float east_x = east.x - centre.x;
  const float east_y = east.y - centre.y;
  const float south_x = south.x - centre.x;
  const float south_y = south.y - centre.y;
  *out_pixels = sqrtf(fabsf(
      east_x * south_y - east_y * south_x));
  if (out_centre) *out_centre = centre;
  return isfinite(*out_pixels);
}

static bool PrepareWorldNavigationMountainSamples(const WorldNavigationProjection *projection) {
  if (s_world_mountains.samples_ready) return true;
  if (s_world_mountains.projection_unavailable) return false;
  const size_t count = s_world_mountains.scene.face_count;
  if (s_world_mountains.projection_capacity < count) {
    void *points = realloc(s_world_mountains.projection,
        count * sizeof(*s_world_mountains.projection));
    if (!points) {
      s_world_mountains.projection_unavailable = true;
      return false; /* Same geometry, uncached path; no per-frame retry. */
    }
    s_world_mountains.projection = points;
    s_world_mountains.projection_capacity = count;
  }
  for (size_t i = 0; i < count; i++) {
    const SimWorldNavigationMountainFace *face = &s_world_mountains.scene.faces[i];
    WorldNavigationMountainProjection *sample = &s_world_mountains.projection[i];
    for (int p = 0; p < 4; p++) {
      float metric;
      if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles,
              face->x[p], face->y[p], sample->normal[p], &metric))
        return false;
      sample->rise[p] = face->z[p] * metric;
      sample->floor[p] = WorldNavigationTerrainHeightAtImpl(
          face->x[p] * kSimWorldMapTilePixels, face->y[p] * kSimWorldMapTilePixels, NULL, true);
    }
  }
  s_world_mountains.samples_ready = true;
  s_world_mountains.projection_ready = false;
  return true;
}

static bool ProjectWorldNavigationMountainFace(
    const SimWorldNavigationMountainFace *face, const WorldNavigationMountainProjection *sample,
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, Sim3DDepthVertex vertices[4],
    Scene3DClipPoint clip[4]) {
  for (int p = 0; p < 4; p++) {
    float normal[3], metric, floor, rise, world[3];
    if (sample) {
      memcpy(normal, sample->normal[p], sizeof(normal));
      floor = sample->floor[p]; rise = sample->rise[p];
    } else {
      if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles,
              face->x[p], face->y[p], normal, &metric)) return false;
      rise = face->z[p] * metric;
      floor = WorldNavigationTerrainHeightAtImpl(
          face->x[p] * kSimWorldMapTilePixels, face->y[p] * kSimWorldMapTilePixels, NULL, true);
    }
    SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, normal, normal);
    /* Native relief is above the registered ground, never above the
     * independent inferred ridge. Keep footprint/owned-cliff conventions
     * identical to the ground grid while removing only that rock rise. */
    WorldNavigationRadialPoint(projection, normal,
        (floor - projection->reference_height_units) * projection->height_world_per_unit +
            rise * projection->tile_world, world);
    Scene3DPoint screen;
    if (!WorldNavigationProjectPoint(projection, viewport, world,
            &screen, &vertices[p].depth, &clip[p])) return false;
    const float shade = face->brightness[p] / 255.0f *
        (slot->sim.world_navigation_lighting ? 0.90f : 1.0f);
    vertices[p].x = screen.x; vertices[p].y = screen.y;
    vertices[p].uv = (ArRenderPointF){face->uv[p].x, face->uv[p].y};
    vertices[p].color = (ArRenderColorF){shade, shade, shade, 1};
  }
  return true;
}

static bool DrawWorldNavigationMountains(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!s_world_mountains.active) return true;
  const bool cached = PrepareWorldNavigationMountainSamples(projection);
  WorldNavigationMountainProjectionKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection;
  key.width = viewport.w; key.height = viewport.h;
  key.lighting = slot->sim.world_navigation_lighting;
  const bool project = !cached || !s_world_mountains.projection_ready ||
      memcmp(&key, &s_world_mountains.projection_key, sizeof(key));
  Sim3DDepthVertex batch[64 * 4];
  Scene3DClipPoint clip[64 * 4];
  size_t count = 0;
  for (size_t at = 0; at < s_world_mountains.scene.face_count; at++) {
    const SimWorldNavigationMountainFace *face = &s_world_mountains.scene.faces[at];
    Sim3DDepthVertex *vertices = batch + count * 4;
    bool valid;
    if (cached) {
      WorldNavigationMountainProjection *sample = &s_world_mountains.projection[at];
      if (project) sample->visible = ProjectWorldNavigationMountainFace(
          face, sample, slot, viewport, projection, sample->points, sample->clip);
      valid = sample->visible;
      if (valid) memcpy(vertices, sample->points, sizeof(sample->points));
      if (valid && projection->clip_frustum)
        memcpy(clip + count * 4, sample->clip, sizeof(sample->clip));
    } else {
      valid = ProjectWorldNavigationMountainFace(face, NULL, slot, viewport, projection,
          vertices, clip + count * 4);
    }
    if (valid && ++count == 64) {
      if (!WorldNavigationAppendProjectedQuads(kSim3DDepthPass_WorldMountain, batch,
              projection->clip_frustum ? clip : NULL, count, viewport))
        return false;
      count = 0;
    }
  }
  if (cached) {
    s_world_mountains.projection_key = key;
    s_world_mountains.projection_ready = true;
  }
  return !count || WorldNavigationAppendProjectedQuads(kSim3DDepthPass_WorldMountain, batch,
      projection->clip_frustum ? clip : NULL, count, viewport);
}

typedef struct WorldNavigationViewportPlanes {
  float planes[4][4];
} WorldNavigationViewportPlanes;

static WorldNavigationViewportPlanes WorldNavigationBuildViewportPlanes(
    const float matrix[16]) {
  WorldNavigationViewportPlanes out;
  for (int side = 0; side < 4; side++) {
    const int axis = side / 2;
    const float sign = side & 1 ? -1.0f : 1.0f;
    for (int i = 0; i < 4; i++)
      out.planes[side][i] = matrix[i * 4 + 3] + sign * matrix[i * 4 + axis];
    const float length = hypotf(hypotf(out.planes[side][0], out.planes[side][1]),
        out.planes[side][2]);
    for (int i = 0; i < 4; i++) out.planes[side][i] /= length;
  }
  return out;
}

static bool WorldNavigationModelOutsideViewport(
    const FrameSlot *slot, const WorldNavigationProjection *projection,
    const WorldNavigationViewportPlanes *viewport_planes, uint16_t object_index,
    float centre_x, float centre_y) {
  const WorldNavigationModelBounds *bound = &s_world_models.bounds[object_index];
  float normal[3], metric;
  if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles,
          centre_x, centre_y, normal, &metric)) return false;
  SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, normal, normal);
  const float anchor_height = projection->height_world_per_unit > 0
      ? WorldNavigationTerrainHeightAt(centre_x * kSimWorldMapTilePixels,
          centre_y * kSimWorldMapTilePixels, NULL) : 0.0f;
  const float base = (anchor_height - projection->reference_height_units) *
      projection->height_world_per_unit;
  const float scale = metric * projection->tile_world * slot->sim.height_scale_x100 / kPercentScale;
  const float low = bound->minimum_rise * scale, high = bound->maximum_rise * scale;
  float centre[3];
  WorldNavigationRadialPoint(projection, normal, base + (low + high) * .5f, centre);
  const float maximum_radius = projection->globe_radius_world +
      anchor_height * projection->height_world_per_unit + high;
  /* Every model column lies within this angular cap, since the chart metric
   * is <= 1. A chord bounds its displacement at any authored height. The
   * enclosing sphere contains triangle interiors as well as their vertices.
   * A small rounding allowance avoids edge flicker from projection precision. */
  const float radius = (high - low) * .5f + 2 * maximum_radius *
      sinf(fminf(kPi, bound->angular_radius) * .5f) + .0001f;
  for (int side = 0; side < 4; side++) {
    const float *plane = viewport_planes->planes[side];
    if (plane[0] * centre[0] + plane[1] * centre[1] +
        plane[2] * centre[2] + plane[3] < -radius) return true;
  }
  return false;
}

static bool DrawWorldNavigationTowns(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!slot->sim.world_navigation_models || !slot->sim.background_voxel_enabled)
    return true;
  const SimWorldNavigationTowns *towns = &slot->sim.world_navigation_towns;
  if (towns->overflow || towns->object_count > kSimWorldNavigationTownObjectCapacity) return false;
  if (!towns->object_count) return true;
  WorldNavigationModelProjectionKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection;
  key.viewport = viewport;
  key.model_revision = s_world_models.revision;
  key.surface_revision = s_world_terrain.cliff_serial;
  key.height_scale = slot->sim.height_scale_x100;
  key.light_azimuth = slot->sim.light_azimuth_deg;
  key.light_elevation = slot->sim.light_elevation_deg;
  key.lighting = slot->sim.world_navigation_lighting;
  key.windmill_phase = s_world_models.windmills ? (slot->sim.game_frame / 12) % 3 : 0;
  const bool same_projection = s_world_models.projection_key_ready &&
      !memcmp(&key, &s_world_models.projection_key, sizeof(key));
  if (s_world_models.projected_valid && same_projection) {
    const Sim3DPerformanceScope project = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
    const bool ready = !s_world_models.projected_count || Sim3DDepthPass_AppendQuads(
        kSim3DDepthPass_Solid, s_world_models.projected, s_world_models.projected_count / 4);
    Sim3DPerformance_End(project);
    return ready;
  }
  s_world_models.projected_valid = false;
  s_world_models.projected_count = 0;
  /* Do not stage an extra copy on every frame of continuous camera motion.
   * A second matching view warms the cache; later held frames replay it. */
  s_world_models.capturing = same_projection && !s_world_models.projection_unavailable;
  WorldNavigationVisibleTownObject
      visible[kSimWorldNavigationTownObjectCapacity];
  const WorldNavigationViewportPlanes viewport_planes =
      WorldNavigationBuildViewportPlanes(projection->matrix);
  int visible_count = 0;
  for (uint16_t i = 0; i < towns->object_count; i++) {
    const SimWorldNavigationTownObject *object = &towns->objects[i];
    /* Unmodelled plot classes retain their authored map art. */
    if (object->kind >= kSimBackgroundVoxelKindCount) continue;
    const bool foliage = object->kind == kSimBackgroundVoxel_Tree ||
        object->kind == kSimBackgroundVoxel_BroadTree ||
        object->kind == kSimBackgroundVoxel_Palm ||
        object->kind == kSimBackgroundVoxel_Shrub;
    const bool landmark = object->kind == kSimBackgroundVoxel_Cathedral ||
        object->kind == kSimBackgroundVoxel_StoryTree ||
        object->kind == kSimBackgroundVoxel_BloodpoolCastle ||
        object->kind == kSimBackgroundVoxel_MarahnaTemple ||
        object->kind == kSimBackgroundVoxel_Pyramid;
    const bool major_structure = landmark ||
        object->kind == kSimBackgroundVoxel_Windmill ||
        object->kind == kSimBackgroundVoxel_Factory;
    int origin_x = 0, origin_y = 0;
    if (!SimWorldMap_OriginForTown(object->town, &origin_x, &origin_y))
      continue;
    const SimBackgroundBridgeBounds bounds = WorldNavigationObjectBounds(object);
    const float centre_x = origin_x +
        (bounds.origin_x + bounds.width * 0.5f) / kSimTownCellPixels;
    const float centre_y = origin_y +
        (bounds.origin_y + bounds.depth * 0.5f) / kSimTownCellPixels;
    ArRenderPointF centre;
    float tile_pixels = 0.0f;
    if (!WorldNavigationTownFootprintPixels(
            slot, viewport, projection,
            centre_x * kSimWorldMapTilePixels,
            centre_y * kSimWorldMapTilePixels,
            &tile_pixels, &centre)) {
      if (!projection->clip_frustum || WorldNavigationModelOutsideViewport(
              slot, projection, &viewport_planes, i, centre_x, centre_y)) continue;
      /* An anchor behind the eye is not proof that a tall model is hidden.
       * Retain uncertain bounds at full permitted detail, then clip its faces. */
      tile_pixels = (float)viewport.h;
      centre = (ArRenderPointF){viewport.w * .5f, viewport.h * .5f};
    }
    const float object_pixels = tile_pixels * sqrtf(
        bounds.width * bounds.depth) / kSimTownCellPixels;
    /* Travel and inspection share the same view-driven selection. Native
     * destination labels and a zero/nonzero orbit must not change geometry. */
    const float minimum_pixels = foliage ? 1.65f :
        major_structure ? 0.78f : 1.25f;
    if (object_pixels < minimum_pixels) continue;
    const float margin = fmaxf(12.0f, object_pixels * 2.0f);
    if ((centre.x < viewport.x - margin ||
        centre.y < viewport.y - margin ||
        centre.x > viewport.x + viewport.w + margin ||
        centre.y > viewport.y + viewport.h + margin) &&
        WorldNavigationModelOutsideViewport(slot, projection, &viewport_planes,
            i, centre_x, centre_y))
      continue;
    /* The real town Low models are sufficient for the overhead view.
     * Recover finer authored detail only as the footprint becomes large. */
    SimBackgroundVoxelDetail detail = object_pixels >= 96.0f
        ? kSimBackgroundVoxelDetail_Ultra : object_pixels >= 64.0f
        ? kSimBackgroundVoxelDetail_High : object_pixels >= 32.0f
        ? kSimBackgroundVoxelDetail_Balanced : kSimBackgroundVoxelDetail_Low;
    if (detail > slot->sim.background_voxel_detail)
      detail = (SimBackgroundVoxelDetail)slot->sim.background_voxel_detail;
    visible[visible_count++] = (WorldNavigationVisibleTownObject){
      .object = object,
      .detail = detail,
    };
  }
  if (!visible_count) {
    s_world_models.capturing = false;
    s_world_models.projected_valid = true;
    s_world_models.projection_key_ready = true;
    s_world_models.projection_key = key;
    return true;
  }
  /* Several towns share the same authored compiler/cache. Leave headroom for
   * LOD and animated variants instead of evicting the next frame's working
   * set while iterating this one. Failure only reduces cache effectiveness. */
  (void)SimBackgroundVoxelModelCache_Reserve((uint32_t)visible_count * 2);
  bool valid = true;
  for (int i = 0; i < visible_count; i++)
    if (!WorldNavigationAppendAuthoredModel(
            slot, viewport, projection, &visible[i])) valid = false;
  s_world_models.projected_valid = valid && s_world_models.capturing;
  s_world_models.capturing = false;
  s_world_models.projection_key = key;
  s_world_models.projection_key_ready = true;
  return valid;
}

static bool DrawWorldNavigationLightTreatment(
    const FrameSlot *slot, ArRenderRectI viewport) {
  if (!slot->sim.world_navigation_lighting) return true;
  const float elevation =
      (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  const float low_sun = 1.0f - sinf(elevation);
  if (low_sun <= 0.001f) return true;

  /* The mesh itself already carries directional per-vertex light from its
   * globe and terrain normals. This restrained warm grade supplies the
   * low-sun colour shift shared by the complete scene. */
  /* Preserve the original byte quantization before crossing the portable
   * float-color boundary. Otherwise every non-integral value would subtly
   * change the dusk treatment. */
  const uint8_t alpha_byte = (uint8_t)(low_sun * 72.0f + 0.5f);
  const float alpha = alpha_byte / 255.0f;
  const ArRenderRectF area = {
    (float)viewport.x, (float)viewport.y,
    (float)viewport.w, (float)viewport.h,
  };
  return ArRenderDevice_DrawSolidRect(
      &g_render_device, &area,
      (ArRenderColorF){42.0f / 255.0f, 24.0f / 255.0f,
                       12.0f / 255.0f, alpha},
      kArRenderBlendMode_Alpha);
}

/* The original label selector owns the clear 256x256 region. Outside it, the
 * already-downsampled world texture supplies depth blur and the shared haze
 * setting supplies blue aerial haze independently of the space backdrop.
 * Both passes share the globe's terrain mesh. */
static bool DrawWorldNavigationActiveRegionHaze(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!slot->sim.world_navigation_haze ||
      (!slot->sim.underlay_defocus_pct && !slot->sim.underlay_haze_pct))
    return true;
  /* The atmosphere must share every terrain vertex. The former sparse
   * rectangular overlay cut across the curved ocean and left large straight
   * edges, which made the sphere look like a map pasted onto a blue ball. */
  static ArRenderVertex2D vertices[kWorldNavigationTerrainVertexCount];
  const float lead = fmaxf(1.0f, slot->sim.cull_haze_lead_px * 0.5f);
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++) {
    vertices[i] = s_world_terrain.vertices[i];
    const float haze = SimWorldNavigationScene_LocationHaze(
        &slot->sim.world_navigation_scene,
        vertices[i].tex_coord.x * kSimWorldMapPixels,
        vertices[i].tex_coord.y * kSimWorldMapPixels, lead);
    vertices[i].color.a *= haze *
        slot->sim.underlay_defocus_pct / (float)kPercentScale;
  }
  if (slot->sim.underlay_defocus_pct &&
      (!EnsureWorldNavigationBlur(slot) ||
       !WorldNavigationAppendGroundLayer(kSim3DDepthPass_GroundBlur, vertices, projection, viewport) ||
       !WorldNavigationAppendCliffLayer(kSim3DDepthPass_GroundBlur, slot, projection, viewport)))
    return false;
  if (!slot->sim.underlay_haze_pct) return true;
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++) {
    const float haze = SimWorldNavigationScene_LocationHaze(
        &slot->sim.world_navigation_scene,
        vertices[i].tex_coord.x * kSimWorldMapPixels,
        vertices[i].tex_coord.y * kSimWorldMapPixels, lead);
    vertices[i].color = (ArRenderColorF){
      0.24f, 0.37f, 0.56f,
      s_world_terrain.vertices[i].color.a * haze *
          slot->sim.underlay_haze_pct / (float)kPercentScale * 0.35f,
    };
  }
  return WorldNavigationAppendGroundLayer(kSim3DDepthPass_GroundHaze, vertices, projection, viewport) &&
      WorldNavigationAppendCliffLayer(kSim3DDepthPass_GroundHaze, slot, projection, viewport);
}


static const ArRenderPointF *WorldNavigationCloudUV(
    int bank, WorldNavigationCloudSurface surface,
    const SimWorldNavigationCloudRotation *rotation,
    const WorldNavigationProjection *projection, ArRenderRectI viewport) {
  const bool terrain = surface == kWorldNavigationCloudSurface_Ground;
  const bool ocean = surface == kWorldNavigationCloudSurface_Ocean;
  if (!s_world_weather.normals_ready ||
      s_world_weather.chart_radius_tiles != projection->chart_radius_tiles) {
    for (int y = 0; y <= kWorldNavigationTerrainCells; y++)
      for (int x = 0; x <= kWorldNavigationTerrainCells; x++)
        SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles, (float)x, (float)y,
            s_world_weather.ground_normals[y * kWorldNavigationTerrainAxis + x], NULL);
    s_world_weather.normals_ready = true;
    s_world_weather.chart_radius_tiles = projection->chart_radius_tiles;
    for (int layer = 0; layer < kSimCloudLayerCount; layer++)
      s_world_weather.uv[layer].ground_ready = false;
  }
  WorldNavigationCloudUVCache *cache = &s_world_weather.uv[bank];
  bool *ready = terrain ? &cache->ground_ready : ocean ? &cache->ocean_ready : &cache->body_ready;
  ArRenderPointF *uv = terrain ? cache->ground : ocean ? cache->ocean : cache->body;
  if (terrain && memcmp(rotation, &cache->ground_rotation, sizeof(*rotation)))
    *ready = false;
  WorldNavigationCloudShellKey key;
  if (!terrain) {
    memset(&key, 0, sizeof(key));
    key.projection = *projection;
    key.viewport = viewport;
    key.rotation = *rotation;
    const WorldNavigationCloudShellKey *previous = ocean ? &cache->ocean_key : &cache->body_key;
    if (memcmp(&key, previous, sizeof(key))) *ready = false;
  }
  if (!*ready) {
    const int count = terrain ? kWorldNavigationTerrainVertexCount : kWorldNavigationOceanVertexCount;
    const float (*normal)[3] = terrain ? s_world_weather.ground_normals
        : ocean ? s_world_shells.ocean.normal : s_world_shells.cloud.normal;
    for (int i = 0; i < count; i++) {
      if (!terrain && !ocean) {
        const SimWorldNavigationCloudCoordinate c = SimWorldNavigationClouds_Coordinate(normal[i], rotation);
        cache->body_direction[i][0] = c.x;
        cache->body_direction[i][1] = c.y;
        cache->body_direction[i][2] = c.z;
        uv[i] = (ArRenderPointF){c.u, c.v};
      } else SimWorldNavigationClouds_UV(normal[i], rotation, &uv[i].x, &uv[i].y);
    }
    *ready = true;
    if (terrain) cache->ground_rotation = *rotation;
    else if (ocean) cache->ocean_key = key;
    else cache->body_key = key;
  }
  return uv;
}

static ArRenderPointF WorldNavigationCloudAtlasUV(int bank, float u, float v) {
  const float latitude = fminf(1, fmaxf(0, v));
  return (ArRenderPointF){
    (u * (kSimWorldNavigationCloudWidth - 1) + .5f) / (kSimWorldNavigationCloudWidth * 2),
    (bank * kSimWorldNavigationCloudHeight + latitude * (kSimWorldNavigationCloudHeight - 1) + .5f) /
        (kSimWorldNavigationCloudHeight * kSimCloudLayerCount)};
}

static bool AppendWorldNavigationCloudSplit(
    Sim3DDepthPassLayer material, int bank, ArRenderRectI viewport,
    const SimWorldNavigationCloudCoordinate coordinates[4], const Sim3DDepthVertex input[4],
    const Scene3DClipPoint *input_clip, float offset_u, float offset_v,
    Sim3DDepthVertex *batch, Scene3DClipPoint *batch_clip, size_t *batch_count) {
  Sim3DDepthVertex original[4];
  Scene3DClipPoint original_clip[4];
  memcpy(original, input, sizeof(original));
  if (input_clip) memcpy(original_clip, input_clip, sizeof(original_clip));
  for (int triangle = 0; triangle < 2; triangle++) {
    const int at[3] = {0, triangle + 1, triangle + 2};
    const SimWorldNavigationCloudCoordinate source[3] = {
      coordinates[at[0]], coordinates[at[1]], coordinates[at[2]]};
    SimWorldNavigationCloudPatch patches[kSimWorldNavigationCloudMaxPatches];
    const int count = SimWorldNavigationClouds_SplitTriangle(source, patches);
    Sim3DDepthVertex output[kSimWorldNavigationCloudMaxPatches * 4];
    Scene3DClipPoint output_clip[kSimWorldNavigationCloudMaxPatches * 4];
    size_t visible = 0;
    for (int face = 0; face < count; face++) {
      const SimWorldNavigationCloudPatch *patch = &patches[face];
      float u[4];
      for (int p = 0; p < 4; p++) {
        u[p] = patch->u[p] + offset_u;
        u[p] -= floorf(u[p]);
      }
      SimWorldNavigationClouds_Unwrap(u);
      uint8_t outside = 0xff;
      for (int p = 0; p < 4; p++) {
        Sim3DDepthVertex *v = &output[visible * 4 + p];
        *v = (Sim3DDepthVertex){0};
        /* Round only once after interpolation. Float intermediate products
         * on oppositely traversed edges caused single-pixel raster cracks. */
        double position[3] = {0};
        double color[4] = {0};
        for (int j = 0; j < 3; j++) {
          const Sim3DDepthVertex *s = &original[at[j]];
          const double w = patch->weight[p][j];
          position[0] += w * s->x;
          position[1] += w * s->y;
          position[2] += w * s->depth;
          color[0] += w * s->color.r; color[1] += w * s->color.g;
          color[2] += w * s->color.b; color[3] += w * s->color.a;
        }
        v->x = (float)position[0]; v->y = (float)position[1]; v->depth = (float)position[2];
        v->color = (ArRenderColorF){(float)color[0], (float)color[1], (float)color[2], (float)color[3]};
        v->uv = WorldNavigationCloudAtlasUV(bank, u[p], patch->v[p] + offset_v);
        if (input_clip) {
          double point[4] = {0};
          for (int j = 0; j < 3; j++) {
            const Scene3DClipPoint *c = &original_clip[at[j]];
            const double weight = patch->weight[p][j];
            point[0] += weight * c->x; point[1] += weight * c->y;
            point[2] += weight * c->z; point[3] += weight * c->w;
          }
          Scene3DClipPoint *c = &output_clip[visible * 4 + p];
          *c = (Scene3DClipPoint){(float)point[0], (float)point[1], (float)point[2], (float)point[3]};
          v->x = v->y = v->depth = 0;
          if (c->w > kScene3DMinimumProjectionDepth) {
            const float inverse = 1.0f / c->w;
            v->x = (c->x * inverse * .5f + .5f) * viewport.w;
            v->y = (1 - (c->y * inverse * .5f + .5f)) * viewport.h;
            v->depth = c->z * inverse * .5f + .5f;
          }
          outside &= WorldNavigationClipOutside(*c);
        } else {
          outside &= WorldNavigationViewportOutside(v->x, v->y, viewport.w, viewport.h);
        }
      }
      if (!outside) visible++;
    }
    for (size_t first = 0; first < visible;) {
      const size_t room = kWorldNavigationTerrainCells - *batch_count;
      const size_t take = visible - first < room ? visible - first : room;
      memcpy(batch + *batch_count * 4, output + first * 4, take * 4 * sizeof(*batch));
      if (batch_clip)
        memcpy(batch_clip + *batch_count * 4, output_clip + first * 4,
            take * 4 * sizeof(*batch_clip));
      *batch_count += take; first += take;
      if (*batch_count == kWorldNavigationTerrainCells) {
        if (!WorldNavigationAppendProjectedQuads(material, batch, batch_clip,
                *batch_count, viewport)) return false;
        *batch_count = 0;
      }
    }
  }
  return true;
}

static void WorldNavigationPrepareCliffCloudUV(int bank,
    const SimWorldNavigationCloudRotation *rotation) {
  WorldNavigationCloudUVCache *cache = &s_world_weather.uv[bank];
  if (cache->cliff_serial == s_world_terrain.cliff_serial &&
      !memcmp(rotation, &cache->cliff_rotation, sizeof(*rotation))) return;
  for (size_t i = 0; i < s_world_terrain.cliffs.face_count; i++)
    for (int p = 0; p < 4; p++) {
      WorldNavigationCliffProjection *face = &s_world_terrain.cliff_projection[i];
      SimWorldNavigationClouds_UV(face->normal[p], rotation,
          &face->cloud_uv[bank][p].x, &face->cloud_uv[bank][p].y);
    }
  cache->cliff_serial = s_world_terrain.cliff_serial;
  cache->cliff_rotation = *rotation;
}

static bool AppendWorldNavigationCloudGrid(
    int bank, WorldNavigationCloudSurface surface, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const SimWorldNavigationCloudRotation *rotation,
    float offset_u, float offset_v, ArRenderColorF colour) {
  const bool terrain = surface == kWorldNavigationCloudSurface_Ground;
  const bool ocean = surface == kWorldNavigationCloudSurface_Ocean;
  const ArRenderPointF *coordinates = WorldNavigationCloudUV(bank, surface, rotation, projection, viewport);
  WorldNavigationCloudUVCache *cache = &s_world_weather.uv[bank];
  if (terrain) WorldNavigationPrepareCliffCloudUV(bank, rotation);
  const WorldNavigationShellGeometry *shell = ocean
      ? &s_world_shells.ocean : &s_world_shells.cloud;
  const int rows = terrain ? kWorldNavigationTerrainCells : kWorldNavigationOceanRings;
  const int columns = terrain ? kWorldNavigationTerrainCells : kWorldNavigationOceanSectors;
  const Sim3DDepthPassLayer material = terrain || ocean
      ? kSim3DDepthPass_CloudShadow : kSim3DDepthPass_Cloud;
  Sim3DDepthVertex row[kWorldNavigationTerrainCells * 4];
  Scene3DClipPoint row_clip[kWorldNavigationTerrainCells * 4];
  size_t row_count = 0;
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < columns; x++) {
      int at[4];
      if (terrain) {
        if (s_world_terrain.cliffs.replacement[y * kWorldNavigationTerrainCells + x])
          continue;
        at[0] = y * kWorldNavigationTerrainAxis + x;
        at[1] = at[0] + 1;
        at[2] = at[1] + kWorldNavigationTerrainAxis;
        at[3] = at[0] + kWorldNavigationTerrainAxis;
      } else {
        const int next = (x + 1) % columns;
        at[0] = y ? 1 + (y - 1) * columns + x : 0;
        at[1] = y ? 1 + (y - 1) * columns + next : 0;
        at[2] = 1 + y * columns + next;
        at[3] = 1 + y * columns + x;
      }
      /* Back-side ocean shadow triangles cannot contribute through the
       * opaque sphere. Cull only when all four corners are behind it. */
      if (ocean && !shell->front[at[0]] && !shell->front[at[1]] &&
          !shell->front[at[2]] && !shell->front[at[3]]) continue;
      const uint8_t *outside = terrain ? s_world_terrain.outside : shell->outside;
      if (outside[at[0]] & outside[at[1]] & outside[at[2]] & outside[at[3]]) continue;
      float u[4];
      for (int p = 0; p < 4; p++) {
        u[p] = coordinates[at[p]].x + offset_u;
        u[p] -= floorf(u[p]);
      }
      SimWorldNavigationClouds_Unwrap(u);
      for (int p = 0; p < 4; p++) {
        Sim3DDepthVertex *v = &row[row_count * 4 + p];
        *v = terrain ? s_world_terrain.depth[at[p]] : shell->points[at[p]];
        v->color = colour;
        if (!terrain) v->color.a *= shell->alpha[at[p]];
        v->uv = WorldNavigationCloudAtlasUV(bank, u[p], coordinates[at[p]].y + offset_v);
      }
      if (projection->clip_frustum) {
        const Scene3DClipPoint *clip = terrain ? s_world_terrain.clip : shell->clip;
        for (int p = 0; p < 4; p++) row_clip[row_count * 4 + p] = clip[at[p]];
      }
      if (!terrain && !ocean) {
        /* Cloud bodies may subdivide their own triangles. Shadow receivers
         * deliberately retain the exact opaque vertices/depth topology. */
        SimWorldNavigationCloudCoordinate quad[4];
        for (int p = 0; p < 4; p++) {
          const float *n = cache->body_direction[at[p]];
          quad[p] = (SimWorldNavigationCloudCoordinate){n[0], n[1], n[2],
              coordinates[at[p]].x, coordinates[at[p]].y};
        }
        if (SimWorldNavigationClouds_NeedsSplit(quad)) {
          if (!AppendWorldNavigationCloudSplit(material, bank, viewport, quad, row + row_count * 4,
                  projection->clip_frustum ? row_clip + row_count * 4 : NULL,
                  offset_u, offset_v, row, projection->clip_frustum ? row_clip : NULL,
                  &row_count)) return false;
          continue;
        }
      }
      if (++row_count == kWorldNavigationTerrainCells) {
        if (!WorldNavigationAppendProjectedQuads(material, row,
                projection->clip_frustum ? row_clip : NULL, row_count, viewport)) return false;
        row_count = 0;
      }
    }
  }
  if (row_count && !WorldNavigationAppendProjectedQuads(material, row,
          projection->clip_frustum ? row_clip : NULL, row_count, viewport)) return false;
  if (terrain) {
    size_t count = 0;
    for (size_t i = 0; i < s_world_terrain.cliffs.face_count; i++) {
      const WorldNavigationCliffProjection *face = &s_world_terrain.cliff_projection[i];
      if (face->outside[0] & face->outside[1] & face->outside[2] & face->outside[3]) continue;
      float u[4];
      for (int p = 0; p < 4; p++) {
        u[p] = face->cloud_uv[bank][p].x + offset_u;
        u[p] -= floorf(u[p]);
      }
      SimWorldNavigationClouds_Unwrap(u);
      for (int p = 0; p < 4; p++) {
        Sim3DDepthVertex *v = &row[count * 4 + p];
        *v = face->depth[p]; v->color = colour;
        v->uv = WorldNavigationCloudAtlasUV(bank, u[p], face->cloud_uv[bank][p].y + offset_v);
      }
      if (projection->clip_frustum)
        memcpy(row_clip + count * 4, face->clip, sizeof(face->clip));
      if (++count == kWorldNavigationTerrainCells) {
        if (!WorldNavigationAppendProjectedQuads(material, row,
                projection->clip_frustum ? row_clip : NULL, count, viewport)) return false;
        count = 0;
      }
    }
    if (count && !WorldNavigationAppendProjectedQuads(material, row,
            projection->clip_frustum ? row_clip : NULL, count, viewport)) return false;
  }
  return true;
}

static bool CacheWorldNavigationReceiver(WorldNavigationReceiverSurface surface, uint32_t cliff,
    const int corners[4], const Sim3DDepthVertex input[4], const Scene3DClipPoint *clip,
    ArRenderRectI viewport) {
  WorldNavigationClipPlan plans[kWorldNavigationClippedQuads];
  size_t count;
  if (!WorldNavigationPrepareClipPlan(input, clip, viewport, plans, &count)) return false;
  const size_t needed = s_world_weather.receiver_count + count;
  enum { kMaximumReceivers = 4 * 1024 * 1024 / sizeof(WorldNavigationShadowReceiver) };
  if (needed > kMaximumReceivers) return false;
  if (needed > s_world_weather.receiver_capacity) {
    size_t capacity = s_world_weather.receiver_capacity ? s_world_weather.receiver_capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > kMaximumReceivers) capacity = kMaximumReceivers;
    void *receivers = realloc(s_world_weather.receivers, capacity * sizeof(*s_world_weather.receivers));
    if (!receivers) return false;
    s_world_weather.receivers = receivers;
    s_world_weather.receiver_capacity = capacity;
  }
  for (size_t i = 0; i < count; i++) {
    WorldNavigationShadowReceiver *receiver = &s_world_weather.receivers[s_world_weather.receiver_count++];
    receiver->geometry = plans[i];
    receiver->surface = surface;
    receiver->cliff = cliff;
    for (int p = 0; p < 4; p++) receiver->corners[p] = (uint16_t)corners[p];
  }
  return true;
}

/* Visibility and clipping are independent of wind, softness and cloud bank.
 * Prepare each exact opaque receiver once per view/surface revision, then
 * vary only its UVs and constant shadow color for the nine weighted samples.
 * Failure leaves the original uncached path available at identical quality. */
static bool PrepareWorldNavigationReceivers(const WorldNavigationProjection *projection,
    ArRenderRectI viewport) {
  if (s_world_weather.receivers_unavailable) return false;
  WorldNavigationReceiverKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection; key.viewport = viewport;
  key.terrain_serial = s_world_terrain.serial; key.cliff_serial = s_world_terrain.cliff_serial;
  if (s_world_weather.receivers_ready &&
      !memcmp(&key, &s_world_weather.receiver_key, sizeof(key))) return true;
  s_world_weather.receivers_ready = false;
  s_world_weather.receiver_count = 0;
  Sim3DDepthVertex input[4];
  Scene3DClipPoint clip[4];
  for (int y = 0; y < kWorldNavigationTerrainCells; y++)
    for (int x = 0; x < kWorldNavigationTerrainCells; x++) {
      if (s_world_terrain.cliffs.replacement[y * kWorldNavigationTerrainCells + x]) continue;
      const int start = y * kWorldNavigationTerrainAxis + x;
      const int at[4] = {start, start + 1, start + 1 + kWorldNavigationTerrainAxis,
          start + kWorldNavigationTerrainAxis};
      if (s_world_terrain.outside[at[0]] & s_world_terrain.outside[at[1]] &
          s_world_terrain.outside[at[2]] & s_world_terrain.outside[at[3]]) continue;
      for (int p = 0; p < 4; p++) {
        input[p] = s_world_terrain.depth[at[p]];
        if (projection->clip_frustum) clip[p] = s_world_terrain.clip[at[p]];
      }
      if (!CacheWorldNavigationReceiver(kWorldNavigationReceiver_Ground, 0, at, input,
              projection->clip_frustum ? clip : NULL, viewport)) goto unavailable;
    }
  for (size_t i = 0; i < s_world_terrain.cliffs.face_count; i++) {
    const WorldNavigationCliffProjection *face = &s_world_terrain.cliff_projection[i];
    if (face->outside[0] & face->outside[1] & face->outside[2] & face->outside[3]) continue;
    const int at[4] = {0, 1, 2, 3};
    if (!CacheWorldNavigationReceiver(kWorldNavigationReceiver_Cliff, (uint32_t)i, at,
            face->depth, projection->clip_frustum ? face->clip : NULL, viewport)) goto unavailable;
  }
  const WorldNavigationShellGeometry *shell = &s_world_shells.ocean;
  for (int y = 0; y < kWorldNavigationOceanRings; y++)
    for (int x = 0; x < kWorldNavigationOceanSectors; x++) {
      const int next = (x + 1) % kWorldNavigationOceanSectors;
      const int at[4] = {
        y ? 1 + (y - 1) * kWorldNavigationOceanSectors + x : 0,
        y ? 1 + (y - 1) * kWorldNavigationOceanSectors + next : 0,
        1 + y * kWorldNavigationOceanSectors + next, 1 + y * kWorldNavigationOceanSectors + x};
      if (!shell->front[at[0]] && !shell->front[at[1]] && !shell->front[at[2]] && !shell->front[at[3]]) continue;
      if (shell->outside[at[0]] & shell->outside[at[1]] & shell->outside[at[2]] & shell->outside[at[3]]) continue;
      for (int p = 0; p < 4; p++) {
        input[p] = shell->points[at[p]];
        if (projection->clip_frustum) clip[p] = shell->clip[at[p]];
      }
      if (!CacheWorldNavigationReceiver(kWorldNavigationReceiver_Ocean, 0, at, input,
              projection->clip_frustum ? clip : NULL, viewport)) goto unavailable;
    }
  s_world_weather.receiver_key = key;
  s_world_weather.receivers_ready = true;
  return true;
unavailable:
  s_world_weather.receivers_unavailable = true; /* No per-frame allocation retries. */
  s_world_weather.receiver_count = 0;
  return false;
}

static bool AppendWorldNavigationReceivers(int bank,
    const SimWorldNavigationCloudRotation *rotation,
    const WorldNavigationProjection *projection, ArRenderRectI viewport,
    float offset_u, float offset_v, ArRenderColorF color) {
  const ArRenderPointF *ground = WorldNavigationCloudUV(bank, kWorldNavigationCloudSurface_Ground,
      rotation, projection, viewport);
  const ArRenderPointF *ocean = WorldNavigationCloudUV(bank, kWorldNavigationCloudSurface_Ocean,
      rotation, projection, viewport);
  WorldNavigationPrepareCliffCloudUV(bank, rotation);
  enum { kBatch = 128 };
  Sim3DDepthVertex vertices[kBatch * 4];
  size_t count = 0;
  for (size_t i = 0; i < s_world_weather.receiver_count; i++) {
    const WorldNavigationShadowReceiver *receiver = &s_world_weather.receivers[i];
    ArRenderPointF source[4], uv[4];
    float u[4];
    for (int p = 0; p < 4; p++) {
      source[p] = receiver->surface == kWorldNavigationReceiver_Cliff
          ? s_world_terrain.cliff_projection[receiver->cliff].cloud_uv[bank][p]
          : (receiver->surface == kWorldNavigationReceiver_Ocean ? ocean : ground)[receiver->corners[p]];
      u[p] = source[p].x + offset_u;
      u[p] -= floorf(u[p]);
    }
    SimWorldNavigationClouds_Unwrap(u);
    for (int p = 0; p < 4; p++) uv[p] = WorldNavigationCloudAtlasUV(bank, u[p], source[p].y + offset_v);
    WorldNavigationApplyShadowPlan(&receiver->geometry, uv, color, vertices + count * 4);
    if (++count == kBatch) {
      if (!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_CloudShadow, vertices, count)) return false;
      count = 0;
    }
  }
  return !count || Sim3DDepthPass_AppendQuads(kSim3DDepthPass_CloudShadow, vertices, count);
}

static bool DrawWorldNavigationCloudLayer(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const SimCloudLayer *layer, uint64_t elapsed_ms, float drift,
    float source_offset_x, float source_offset_y,
    bool follow_terrain, ArRenderColorF colour) {
  const int bank = (int)(layer - kSimCloudLayers);
  if (bank < 0 || bank >= kSimCloudLayerCount) return false;
  const float phase_u = layer->offset_x +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_x, drift);
  const float phase_v = layer->offset_y +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_y, drift);
  const SimWorldNavigationCloudRotation rotation =
      SimWorldNavigationClouds_Rotation(phase_u, phase_v);
  const float offset_u = source_offset_x / kSimWorldMapPixels;
  const float offset_v = source_offset_y / kSimWorldMapPixels;
  if (!follow_terrain)
    return AppendWorldNavigationCloudGrid(bank, kWorldNavigationCloudSurface_Body, viewport, projection, &rotation, 0, 0, colour);
  if (PrepareWorldNavigationReceivers(projection, viewport))
    return AppendWorldNavigationReceivers(bank, &rotation, projection, viewport, offset_u, offset_v, colour);
  /* Reuse exact opaque vertices on both mapped land and the complete ocean
   * sphere. There is no rectangular fade or camera-relative cloud boundary. */
  return AppendWorldNavigationCloudGrid(bank, kWorldNavigationCloudSurface_Ground, viewport, projection, &rotation, offset_u, offset_v, colour) &&
      AppendWorldNavigationCloudGrid(bank, kWorldNavigationCloudSurface_Ocean, viewport, projection, &rotation, offset_u, offset_v, colour);
}


static PresentationOutcome OmitWorldNavigationWeather(const char *reason) {
  if (!s_world_weather.failure_reported) {
    s_world_weather.failure_reported = true;
    fprintf(stderr, "[world-navigation] optional weather omitted: %s\n",
            reason && reason[0] ? reason : "renderer rejected the effect");
  }
  return kPresentationOutcome_OptionalOmitted;
}

static PresentationOutcome DrawWorldNavigationWeather(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!slot->sim.world_navigation_clouds ||
      !slot->sim.cloud_opacity_pct)
    return kPresentationOutcome_Complete;
  if (!EnsureWorldNavigationCloudTexture())
    return OmitWorldNavigationWeather(
        ArRenderDevice_LastError(&g_render_device));

  const float opacity =
      (float)slot->sim.cloud_opacity_pct / (float)kPercentScale;
  const float body_visibility = slot->sim.view == kSimView_SkyPalace ? 1.0f
      : SimWorldNavigationScene_CloudVisibility(
      slot->sim.world_navigation.zoom_current,
      slot->sim.cloud_altitude_px);
  const float drift =
      (float)slot->sim.cloud_drift_pct / (float)kPercentScale;
  const uint64_t elapsed_ms = HostClock_Milliseconds();

  /* A cloud's altitude is invisible to an orthographic top-down camera until
   * it casts a displaced shadow. Reuse the town light's world-space shear so
   * the shadow rotates and zooms with the scripted Mode-7 event. The
   * procedural alpha already supplies a soft edge; the softness dial spreads
   * three low-alpha samples across the light-perpendicular axis. */
  if (slot->sim.world_navigation_cloud_shadows && slot->sim.world_navigation_lighting &&
      slot->sim.shadow_opacity_pct) {
    float light_x = 0.0f, light_y = 0.0f;
    SimShadowLight(slot, &light_x, &light_y);
    const float shadow_x =
        light_x * (float)slot->sim.cloud_altitude_px / 8.0f;
    const float shadow_y =
        light_y * (float)slot->sim.cloud_altitude_px / 8.0f;
    const float blur =
        (float)slot->sim.shadow_softness_pct * 0.08f;
    const int sample_count = blur > 0.01f ? 3 : 1;
    const float perpendicular_x = -sinf(
        (float)slot->sim.light_azimuth_deg * kPi / 180.0f);
    const float perpendicular_y = cosf(
        (float)slot->sim.light_azimuth_deg * kPi / 180.0f);
    for (unsigned layer_index = 0;
         layer_index <
             (size_t)kSimCloudLayerCount;
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      for (int sample = 0; sample < sample_count; sample++) {
        const float spread =
            sample_count == 1 ? 0.0f : (float)(sample - 1) * blur;
        const float sample_weight =
            sample_count == 1 ? 1.0f : sample == 1 ? 0.5f : 0.25f;
        const float alpha = opacity * layer->weight *
            ((float)slot->sim.shadow_opacity_pct / (float)kPercentScale) *
            0.35f * sample_weight;
        if (!DrawWorldNavigationCloudLayer(
                viewport, projection, layer, elapsed_ms, drift,
                shadow_x + perpendicular_x * spread,
                shadow_y + perpendicular_y * spread,
                true,
                (ArRenderColorF){0.0f, 0.0f, 0.0f, alpha}))
          return OmitWorldNavigationWeather(
              ArRenderDevice_LastError(&g_render_device));
      }
    }
  }

  if (slot->sim.view == kSimView_SkyPalace &&
      !PresentWorldNavSky_DrawClouds(&g_render_device, slot, viewport, projection, elapsed_ms, drift, opacity))
    return OmitWorldNavigationWeather(ArRenderDevice_LastError(&g_render_device));

  if (body_visibility > 0.001f) {
    if (!DrawWorldNavigationSphereShell(
            viewport, projection, kWorldNavigationShell_Cloud))
      return OmitWorldNavigationWeather("cloud shell projection");
    for (unsigned layer_index = 0;
         layer_index < (size_t)kSimCloudLayerCount;
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      if (!DrawWorldNavigationCloudLayer(
              viewport, projection, layer, elapsed_ms, drift,
              0.0f, 0.0f, false,
              (ArRenderColorF){
                1.0f, 1.0f, 1.0f,
                opacity * layer->weight * body_visibility,
              })) {
        return OmitWorldNavigationWeather(
            ArRenderDevice_LastError(&g_render_device));
      }
    }
  }
  return kPresentationOutcome_Complete;
}

static bool DrawWorldNavigationCompositionLayer(
    const FrameSlot *slot, ArRenderRectI viewport,
    const SimWorldNavigationCompositionLayer *layer,
    ArRenderTexture texture, ArRenderPointF offset) {
  if (!layer || !layer->visible) return true;
  if (!ArRenderTexture_IsValid(texture) ||
      !layer->width || !layer->height)
    return false;
  const ArRenderPointF top_left = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x, layer->screen_y);
  const ArRenderPointF bottom_right = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x + layer->width,
      layer->screen_y + layer->height);
  ArRenderRectF source = {0.0f, 0.0f, layer->width, layer->height};
  ArRenderRectF destination = {
    top_left.x + offset.x, top_left.y + offset.y,
    bottom_right.x - top_left.x,
    bottom_right.y - top_left.y,
  };
  return ArRenderDevice_DrawTexture(
      &g_render_device, texture, &source, &destination);
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
    if (!WorldNavigationSurfaceNormal(slot, viewport, projection,
            slot->sim.world_navigation.focus_x, slot->sim.world_navigation.focus_y, normal)) return false;
    WorldNavigationRadialPoint(projection, normal, 0, world);
    float facing = 0;
    for (int i = 0; i < 3; i++) facing += normal[i] * (projection->camera_world[i] - world[i]);
    if (facing <= 0) return true;
    Scene3DPoint focus;
    if (!Scene3D_ProjectWorldPoint(projection->matrix, world[0], world[1], world[2],
            viewport.w, viewport.h, &focus)) return false;
    /* The authored marker is relative to the unmodified travel centre, not
     * the newly aimed projection of (0,0,0). Subtracting the latter would
     * cancel framing motion and detach the Palace from its real location. */
    offset = (ArRenderPointF){focus.x - viewport.w * .5f, focus.y - viewport.h * .5f};
  }
  return DrawWorldNavigationCompositionLayer(slot, viewport,
      &slot->sim.world_navigation_scene.composition.palace,
      s_world_composition.palace, offset);
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

static bool EnsureWorldNavigationResources(const FrameSlot *slot) {
  Sim3DPerformanceScope art_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Upload);
  const bool setup_timing = Sim3DPerformance_Enabled() &&
      ((!s_world_mountains.ready && WorldNavigationMountainsEnabled(slot)) ||
       !s_world_terrain.cliffs_ready ||
       !s_world_art.serial);
  const uint64_t setup_started = setup_timing ? HostClock_Nanoseconds() : 0;
  EnsureWorldNavigationMountains(slot);
  const uint64_t mountains_done = setup_timing ? HostClock_Nanoseconds() : 0;
  EnsureWorldNavigationCliffs(slot);
  const uint64_t cliffs_done = setup_timing ? HostClock_Nanoseconds() : 0;
  const bool art_ready = EnsureWorldNavigationArt(slot);
  const uint64_t art_done = setup_timing ? HostClock_Nanoseconds() : 0;
  if (setup_timing && art_done - setup_started > 5000000)
    fprintf(stderr, "[world-navigation-setup] mountains=%.3fms cliffs=%.3fms art=%.3fms\n",
        (double)(mountains_done - setup_started) / 1000000.0,
        (double)(cliffs_done - mountains_done) / 1000000.0,
        (double)(art_done - cliffs_done) / 1000000.0);
  /* Allocation fallback in the art bake must restore the matching old mesh. */
  if (s_world_art.unavailable) EnsureWorldNavigationCliffs(slot);
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
  Sim3DPerformance_End(projection_performance);
  if (!projection_ok) {
    return kPresentationOutcome_CoreFailure;
  }
  if (slot->sim.view == kSimView_SkyPalace || slot->sim.world_navigation_backdrop) {
    const Sim3DPerformanceScope backdrop_performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Backdrop);
    const bool backdrop_ok = slot->sim.view == kSimView_SkyPalace
        ? PresentWorldNavSky_DrawBackdrop(&g_render_device, viewport, slot->sim.world_navigation_backdrop,
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
        viewport, projection, kWorldNavigationShell_Atmosphere);
    Sim3DPerformance_End(atmosphere_performance);
    if (!atmosphere_ok) {
      return kPresentationOutcome_CoreFailure;
    }
  }
  const Sim3DPerformanceScope ocean_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_WorldOcean);
  const bool ocean_ok = Sim3DDepthPass_Begin(
      &g_render_device, viewport.w, viewport.h, kArRenderFilter_Linear) &&
      DrawWorldNavigationSphereShell(viewport, projection, kWorldNavigationShell_Ocean);
  Sim3DPerformance_End(ocean_performance);
  if (!ocean_ok) {
    return kPresentationOutcome_CoreFailure;
  }
  Sim3DPerformanceScope terrain_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
  bool ground_ok = DrawWorldNavigationGround(slot, viewport, projection);
  Sim3DPerformance_End(terrain_performance);
  if (!ground_ok) {
    return kPresentationOutcome_CoreFailure;
  }
  if (!DrawWorldNavigationActiveRegionHaze(
          slot, viewport, projection)) {
    return kPresentationOutcome_CoreFailure;
  }
  const Sim3DPerformanceScope mountain_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthMountain);
  const bool mountains_ok = DrawWorldNavigationMountains(slot, viewport, projection);
  Sim3DPerformance_End(mountain_performance);
  if (!mountains_ok || !DrawWorldNavigationTowns(slot, viewport, projection)) {
    return kPresentationOutcome_CoreFailure;
  }
  /* Whole-world weather follows the same curved perspective surface as the
   * ground. It has no town sprite-window hole or cull boundary: every part of
   * this world is intentional content. Palace and labels stay screen-space. */
  Sim3DPerformanceScope weather_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Cloud);
  outcome = PresentationOutcome_Combine(
      outcome, DrawWorldNavigationWeather(slot, viewport, projection));
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
      !PresentWorldNavSky_DrawMist(&g_render_device, viewport, PresentWorldNavSky_Horizon(viewport, projection)))
    return kPresentationOutcome_CoreFailure;
  if (!DrawWorldNavigationLightTreatment(slot, viewport)) {
    return kPresentationOutcome_CoreFailure;
  }

  /* INIDISP is a master brightness applied after the PPU has composed every
   * layer. Do the same for the host-owned world and all its effects. The
   * Palace/UI captures are drawn afterward because PpuRasterizeObjRange has
   * already applied this frame's brightness to their pixels. */
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
      !s_world_composition.uploaded)
    return kPresentationOutcome_CoreFailure;
  if (!EnsureWorldNavigationResources(slot)) return kPresentationOutcome_CoreFailure;
  if (!composition->empty_animation &&
      (!ArRenderTexture_IsValid(s_world_composition.palace) ||
       !ArRenderTexture_IsValid(s_world_composition.ui)))
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
  WorldNavigationProjection projection;
  const PresentationOutcome outcome = DrawWorldNavigationScene(slot, viewport, &projection);
  if (!PresentationOutcome_IsUsable(outcome)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return outcome;
  }
  if (!composition->empty_animation &&
      (!DrawWorldNavigationPalace(slot, viewport, &projection) ||
       !DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->ui,
           s_world_composition.ui, (ArRenderPointF){0}))) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }
  if (!ArRenderOutputFrame_Finish(&output_frame))
    return kPresentationOutcome_CoreFailure;
  Sim3DPerformance_EndPresentation();
  return outcome;
}

/* The world-map half of the presentation-resource reset. PresentSim3D_ResetResources
 * keeps the town half and calls this; see the comment on
 * PresentRendererResources_Reset in present.c for why any of it exists. */
void PresentWorldNav_ResetResources(void) {
  free(s_world_weather.receivers);
  s_world_weather.receivers = NULL;
  s_world_weather.receiver_count = s_world_weather.receiver_capacity = 0;
  s_world_weather.receivers_ready = s_world_weather.receivers_unavailable = false;
  free(s_world_models.projected);
  s_world_models.projected = NULL;
  s_world_models.projected_count = s_world_models.projected_capacity = 0;
  s_world_models.projected_valid = s_world_models.capturing = false;
  s_world_models.projection_key_ready = false;
  s_world_models.projection_unavailable = false;
  s_world_models.object_count = 0;
  s_world_models.detail = s_world_models.style = -1;
  s_world_models.maximum_rise = 0.0f;
  DestroyWorldNavigationMountainProjection();
  SimWorldNavigationMountains_Destroy(&s_world_mountains.scene);
  SimWorldNavigationCliffs_Destroy(&s_world_terrain.cliffs);
  free(s_world_terrain.cliff_projection);
  s_world_terrain.cliff_projection = NULL;
  s_world_terrain.cliffs_ready = false;
  SimWorldNavigationMountainTransition_Destroy(&s_world_mountains.transition);
  s_world_mountains.transition_ready = false;
  s_world_mountains.transition_serial = 0;
  s_world_mountains.relief_pct = 0;
  s_world_mountains.ready = false;
  s_world_mountains.active = false;
  s_world_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  s_world_mountains.developed = false;
  SimWorldNavigationTerrain_SetMountainReplacement(NULL);
  SimWorldNavigationTerrain_SetMountainTransition(NULL);
  SimWorldNavigationTerrain_SetMountainJoin(NULL, NULL, 0);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(NULL, NULL, 0);
  s_world_terrain.projection_ready = false;
  s_world_terrain.ready = false;
  InvalidateWorldNavigationArtPublication();
  s_world_art.blur_serial = 0;
  s_world_art.geography = 0;
  s_world_art.unavailable = false;
  free(s_world_art.pixels);
  s_world_art.pixels = NULL;
  free(s_world_art.baseline);
  s_world_art.baseline = NULL;
  s_world_weather.ready = false;
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_composition.palace);
  s_world_composition.palace = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_composition.ui);
  s_world_composition.ui = ArRenderTexture_Invalid();
  s_world_composition.uploaded = false;
  s_world_weather.unavailable = false;
  PresentWorldNavSky_Reset();
  s_world_weather.failure_reported = false;
}
