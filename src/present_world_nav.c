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
#include "present_world_nav_model_mesh.h"
#include "present_sim_globe_focus.h"
#include "present_world_nav_test.h"
#include "host/parallel_work.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present.h"
#include "action/action_effect_render.h"
#include "actraiser/actraiser_localization_world_navigation.h"
#include "constants.h"
#include "deterministic_hash.h"
#include "snesrecomp/game/types.h"
#include "diorama/diorama.h"
#include "host/host_clock.h"
#include "presentation_outcome.h"
#include "render/render_device.h"
#include "render/render_output.h"
#include "render/localized_text_presenter.h"
#include "scene3d_math.h"
#include "sim/sim3d_camera_limits.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim3d_mesh_set.h"
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
#include "present_sim_globe_mapping.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
extern ArRenderDevice g_render_device;
#include "present_sim3d_internal.h"
#include "present_sim_globe.h"
#include "present_sim_globe_mountains.h"
#include "present_sim_globe_terrain.h"
#include "present_sim_globe_water.h"
#include "present_sim3d_project.h"
#include "sim/sim_background_voxels.h"


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
static struct {
  Sim3DMeshSet surface;
  SimGlobeMapping map;
  uint32_t geography, mountains, cliffs;
  uint16_t height_percent;
  size_t quads, mountain_first;
  bool ready, detailed_town;
} s_sim_globe;


enum {
  kWorldNavigationTerrainCells = kSimWorldMapTiles,
  kWorldNavigationTerrainAxis = kWorldNavigationTerrainCells + 1,
  kWorldNavigationTerrainVertexCount =
      kWorldNavigationTerrainAxis * kWorldNavigationTerrainAxis,
  kWorldNavigationOceanRings = 48,
  kWorldNavigationOceanSectors = 96,
  kWorldNavigationOceanQuads = kWorldNavigationOceanRings*kWorldNavigationOceanSectors,
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
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  bool ready;
  Sim3DDepthVertex points[kWorldNavigationOceanVertexCount];
  bool occlusion;
  int first_index, first_vertex;
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

typedef struct WorldNavigationAtmosphereDrawCache {
  ArRenderVertex2D *vertices;
  int32_t *indices;
  size_t quad_count, capacity;
  bool ready, repeated, unavailable;
} WorldNavigationAtmosphereDrawCache;

static const float kWorldNavigationTerrainAmbient = 0.68f;

/* Presentation choices, not native coordinates or renderer settings. Keep
 * the local tile/relief scale while giving navigation a broader landscape
 * and the Palace backdrop its separately art-directed horizon. */
static float WorldNavigationChartRadius(const FrameSlot *slot) {
  return kSimWorldNavigationGlobeRadiusTiles *
      (slot->sim.view == kSimView_SkyPalace ? 3.0f : 2.0f);
}

typedef struct WorldNavigationGroundKey {
  WorldNavigationProjection projection;
  float source_to_screen[6];
  ArRenderRectI viewport;
  uint32_t geography_serial;
  int snes_width, snes_height, visible_width, visible_x0;
  int light_azimuth, light_elevation, lighting;
} WorldNavigationGroundKey;

typedef struct WorldNavigationGroundSample {
  float normal[3][3]; /* centre, half-cell east, half-cell south */
  float height[3];
  float edge_alpha;
} WorldNavigationGroundSample;
typedef struct WorldNavigationGroundSampleKey {
  float chart_radius_tiles;
  uint32_t geography_serial, cliff_serial;
  bool heights;
} WorldNavigationGroundSampleKey;
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
  int height_scale, light_azimuth, light_elevation, lighting;
} WorldNavigationModelProjectionKey;

/* Static spans retain their original position around animated objects.
 * End indexes the existing 8 MiB-bounded vertex cache; object indexes the
 * current immutable capture, never a retained FrameSlot/model-cache pointer. */
typedef struct WorldNavigationAnimatedModel {
  uint32_t static_end;
  uint16_t object;
  uint8_t detail;
} WorldNavigationAnimatedModel;

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
  ArRenderTexture label;
  ArRenderTexture plaque;
  bool uploaded;
} s_world_composition;

static struct {
  uint32_t serial;
  uint64_t image_revision;
  uint32_t geography;
  SimWorldNavigationTownGround sources;
  bool detailed;
  bool models;
  uint8_t phase;
  uint32_t blur_serial;
  bool unavailable;
  uint32_t *pixels;
  uint32_t *baseline;
  SimWorldNavigationArtAnimation *animation;
  bool animation_unavailable;
  bool cliffs;
  Sim3DDepthAtlasCache *atlas_cache;
  bool atlas_cache_unavailable;
  int displayed_version; /* Per-frame selection only; never a CPU publication key. */
} s_world_art;

static struct {
  SimWorldNavigationMountainScene scene;
  SimWorldNavigationTownGround sources;
  bool ready;
  SimWorldNavigationMountainAtlasUpdate lava_upload;
  uint64_t atlas_revision;
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
  uint32_t geometry_revision;
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
  WorldNavigationModelSource *gpu_sources;
  size_t gpu_source_capacity;
  bool gpu_sources_unavailable;
  bool gpu_current_ready;
  bool gpu_current_rejected;
  WorldNavigationModelProjectionKey projection_key;
  Sim3DDepthVertex *projected;
  size_t projected_count, projected_capacity;
  bool projected_valid, projection_key_ready, capturing, projection_unavailable;
  bool capture_static;
  Sim3DDepthMesh *solid_mesh;
  bool solid_mesh_published, solid_mesh_unavailable, solid_mesh_attempted;
  bool solid_mesh_opt_out;
  uint16_t animated_count;
  WorldNavigationAnimatedModel animated[kSimWorldNavigationTownObjectCapacity];
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
  WorldNavigationGroundSample samples[kWorldNavigationTerrainVertexCount];
  WorldNavigationGroundSampleKey sample_key;
  bool samples_ready;
} s_world_terrain;

/* Ground and native mountain cutouts are separate materials in one bounded
 * mesh. Their projected source is immutable on a held view; animated atlas
 * pixels and weather samples are deliberately not part of this cache. */
static struct {
  Sim3DDepthMesh *mesh;
  Sim3DDepthGeometryRange ranges[2];
  WorldNavigationGroundKey retained_key;
  uint32_t cliff_serial, mountain_revision;
  bool key_ready, published, unavailable, attempted, opt_out, mountains_retained;
} s_world_surfaces;

/* Shared ocean/land/cliff/mountain source replaces (never adds to) the world's
 * opaque retention slot. Each material keeps its own transform and atlas. */
enum {
  kWorldNavigationGridChunkCells = 16,
  kWorldNavigationGridChunkAxis = kWorldNavigationTerrainCells / kWorldNavigationGridChunkCells,
  kWorldNavigationGridChunks = kWorldNavigationGridChunkAxis * kWorldNavigationGridChunkAxis,
  kWorldNavigationSurfaceChunks = kWorldNavigationGridChunks + 3, /* ocean, land, cliffs, mountains */
  kWorldNavigationSurfaceMaximumQuads = kSim3DMeshSetMaximumQuads,
};
_Static_assert(kWorldNavigationTerrainCells % kWorldNavigationGridChunkCells == 0 &&
    (kWorldNavigationSurfaceChunks + 1) / 2 <= 64,
    "Adjacent source chunks merge into at most 64 selected runs");
_Static_assert(kWorldNavigationOceanQuads + kWorldNavigationTerrainCells*kWorldNavigationTerrainCells +
    kSimWorldNavigationCliffMaximumFaces + kSimWorldNavigationMountainMaximumFaces <=
    kWorldNavigationSurfaceMaximumQuads, "all authored world surfaces must fit the partitioned source");
static struct {
  Sim3DMeshSet meshes;
  WorldNavigationGroundSampleKey key;
  WorldNavigationProjection rejected_projection;
  uint32_t rejected_geography, rejected_cliffs, rejected_mountains;
  float height_ratio;
  size_t source_quads, selected_quads, mountain_quads;
  uint32_t mountain_revision;
  struct {
    WorldNavigationRadialBounds bounds;
    Sim3DDepthMeshRange range;
  } chunks[kWorldNavigationSurfaceChunks];
  Sim3DDepthRadialTransform selection_transform;
  bool cull, cull_unavailable, selection_ready;
  bool attempted, enabled, unavailable, ready, drawn;
} s_world_gpu_grid;

static size_t WorldNavigationShadowSamples(const FrameSlot *slot, uint64_t elapsed_ms,
    Sim3DDepthSphericalSample samples[kSimCloudLayerCount * 3]);
static bool EnsureWorldNavigationCloudTexture(void);

/* One presentation-owned fork/join group for immutable geometry math. It is
 * independent of weather enablement; jobs never overlap or own resources. */
static HostParallelWork *s_world_workers;
static bool s_world_workers_attempted;

static HostParallelWork *WorldNavigationWorkers(void) {
  if (!s_world_workers_attempted) {
    s_world_workers_attempted = true;
    s_world_workers = HostParallelWork_Create(3);
  }
  return s_world_workers;
}

static struct {
  WorldNavigationShellGeometry cloud;
  WorldNavigationShellGeometry ocean;
  WorldNavigationShellGeometry atmosphere;
  WorldNavigationAtmosphereDrawCache atmosphere_draw;
  bool indices_ready;
  float longitude_cos[kWorldNavigationOceanSectors];
  float longitude_sin[kWorldNavigationOceanSectors];
  int32_t indices[
      kWorldNavigationOceanIndexCount];
  /* Navigation submits only the retained atmosphere vertex suffix. Keep
   * its rebased indices separate from the ocean/cloud topology. */
  int32_t atmosphere_indices[kWorldNavigationOceanIndexCount];
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
  Sim3DDepthMesh *receiver_mesh;
  Sim3DDepthPosition *receiver_positions;
  ArRenderPointF *receiver_uv;
  size_t receiver_mesh_capacity;
  bool receiver_mesh_ready, receiver_mesh_unavailable;
  Sim3DDepthMesh *spherical_mesh;
  Sim3DDepthSphericalQuad *spherical_quads;
  size_t spherical_capacity;
  bool spherical_ready, spherical_unavailable;
  Sim3DDepthMesh *body_mesh;
  Sim3DDepthSphericalBodyVertex *body_vertices;
  float body_radius, body_distance;
  bool body_unavailable;
} s_world_weather;

#if AR_WORLD_NAV_CACHE_TESTING
static size_t s_model_test_bytes = 8 * 1024 * 1024, s_receiver_test_bytes = 4 * 1024 * 1024;
void PresentWorldNav_TestCacheBudgets(size_t model_bytes, size_t receiver_bytes) {
  s_model_test_bytes = model_bytes; s_receiver_test_bytes = receiver_bytes;
}
WorldNavigationCacheTestState PresentWorldNav_TestCacheState(void) {
  return (WorldNavigationCacheTestState){s_world_models.projection_unavailable,
      s_world_weather.receivers_unavailable, s_world_models.projected_count, s_world_weather.receiver_count};
}
#endif

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
  ArRenderTexture plaque = EnsureWorldNavigationCompositionTexture(
      &s_world_composition.plaque);
  ArRenderTexture label = composition->label.visible
      ? EnsureWorldNavigationCompositionTexture(&s_world_composition.label)
      : ArRenderTexture_Invalid();
  if (!ArRenderTexture_IsValid(palace) ||
      !ArRenderTexture_IsValid(plaque) ||
      (composition->label.visible && !ArRenderTexture_IsValid(label)))
    return;
  ArRenderRectI palace_rect = {
    0, 0, composition->palace.width, composition->palace.height,
  };
  ArRenderRectI plaque_rect = {
    0, 0, composition->plaque.width, composition->plaque.height,
  };
  ArRenderRectI label_rect = {
    0, 0, composition->label.width, composition->label.height,
  };
  if (!ArRenderDevice_UpdateTexture(
          &g_render_device, palace, &palace_rect,
          g_sim_world_navigation_palace_pixels,
          kSimWorldNavigationCompositionPitch) ||
      !ArRenderDevice_UpdateTexture(
          &g_render_device, plaque, &plaque_rect,
          g_sim_world_navigation_plaque_pixels,
          kSimWorldNavigationCompositionPitch) ||
      (composition->label.visible &&
       !ArRenderDevice_UpdateTexture(
           &g_render_device, label, &label_rect,
           g_sim_world_navigation_label_pixels,
           kSimWorldNavigationCompositionPitch)))
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
  ++s_world_art.image_revision;
  s_world_art.serial = 0;
  Sim3DDepthPass_DestroyAtlasCache(s_world_art.atlas_cache);
  s_world_art.atlas_cache = NULL;
}

static void RefreshWorldNavigationMountainGeometry(void) {
  ++s_world_mountains.geometry_revision;
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
    ++s_world_mountains.atlas_revision;
    Sim3DPerformance_AddUpload((uint64_t)region.w * region.h * sizeof(uint32_t));
    s_world_mountains.lava_upload = (SimWorldNavigationMountainAtlasUpdate){0};
  }
}

static void EnsureWorldNavigationMountains(const FrameSlot *slot, float radius_tiles) {
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

static void RebuildWorldNavigationAnimationRange(void *context, size_t first, size_t end) {
  SimWorldNavigationArt_RenderAnimationRows(context, first, end);
}

static bool UpdateWorldNavigationAnimation(const uint32_t *developed,
    const uint8_t *world_cells, const SimWorldNavigationTownGround *ground,
    bool models, uint8_t phase, SimWorldNavigationArtChanges *changes) {
  if (!s_world_art.animation && !s_world_art.animation_unavailable) {
    s_world_art.animation = malloc(sizeof(*s_world_art.animation));
    s_world_art.animation_unavailable = !s_world_art.animation;
  }
  if (!s_world_art.animation)
    return SimWorldNavigationArt_UpdateAnimation(s_world_art.pixels, kSimWorldNavigationArtPixels,
        developed, kSimWorldMapPixels, s_world_art.baseline, kSimWorldMapPixels,
        world_cells, ground, models, s_world_terrain.cliffs.town_mask != 0,
        s_world_art.phase, phase, changes);
  SimWorldNavigationArtAnimation *work = s_world_art.animation;
  if (!SimWorldNavigationArt_PrepareAnimation(work,
          s_world_art.pixels, kSimWorldNavigationArtPixels,
          developed, kSimWorldMapPixels, s_world_art.baseline, kSimWorldMapPixels,
          world_cells, ground, models, s_world_terrain.cliffs.town_mask != 0,
          s_world_art.phase, phase)) return false;
  HostParallelWork_Run(WorldNavigationWorkers(), kSimWorldMapTiles, 16,
      RebuildWorldNavigationAnimationRange, work);
  *changes = work->changes;
  /* The reusable storage must not advertise borrowed inputs between frames. */
  work->ready = false;
  return true;
}

static bool WorldNavigationGpuGridEnabled(void);

static bool WorldNavigationArtVersion(const FrameSlot *slot, uint8_t phase,
    unsigned *version) {
  _Static_assert(kWorldWaterFrameCount * kSimTownGroundAnimationFrames <=
      kSim3DDepthAtlasVersionLimit, "Ground animation fits bounded GPU snapshots");
  uint8_t water;
  if (s_world_art.atlas_cache_unavailable || s_world_art.unavailable ||
      !WorldNavigationGpuGridEnabled() ||
      slot->sim.underlay_serial != SimWorldMap_Serial() ||
      !SimWorldMap_WaterAnimationFrame(&water)) return false;
  *version = water * kSimTownGroundAnimationFrames + phase;
  return true;
}

static void CaptureWorldNavigationArtVersion(const FrameSlot *slot, uint8_t phase) {
  unsigned version;
  if (!WorldNavigationArtVersion(slot, phase, &version)) return;
  if (!s_world_art.atlas_cache)
    s_world_art.atlas_cache = Sim3DDepthPass_CreateAtlasCache();
  if (s_world_art.atlas_cache &&
      Sim3DDepthPass_CaptureAtlasVersion(s_world_art.atlas_cache, version)) return;
  Sim3DDepthPass_DestroyAtlasCache(s_world_art.atlas_cache);
  s_world_art.atlas_cache = NULL;
  s_world_art.atlas_cache_unavailable = true;
  fprintf(stderr, "[world-navigation] GPU ground snapshots unavailable; using mutable atlas\n");
}

static bool EnsureWorldNavigationArt(const FrameSlot *slot, float radius_tiles) {
  s_world_art.displayed_version = -1;
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
  unsigned version;
  if (same_style && s_world_art.serial &&
      s_world_art.geography == SimWorldMap_GeographySerial() &&
      WorldNavigationArtVersion(slot, phase, &version) && s_world_art.atlas_cache &&
      Sim3DDepthPass_HasAtlasVersion(s_world_art.atlas_cache, version)) {
    /* These keys certify CPU pixels AND the mutable atlas, not the image
     * selected for display. Never advance them when only a snapshot is used. */
    Sim3DPerformance_AddAtlasReuse();
    s_world_art.displayed_version = (int)version;
    return true;
  }
  if (same_style && same_image && s_world_art.phase == phase) {
    CaptureWorldNavigationArtVersion(slot, phase);
    return true;
  }
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
    const bool updated = world_ready && UpdateWorldNavigationAnimation(developed,
            same_image ? NULL : world_cells, detailed ? ground : NULL, models, phase, &changes) &&
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
      ++s_world_art.image_revision;
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
  s_world_art.serial = slot->sim.underlay_serial;
  s_world_art.geography = SimWorldMap_GeographySerial();
  s_world_art.detailed = detailed;
  s_world_art.models = models;
  s_world_art.phase = phase;
  s_world_art.cliffs = s_world_terrain.cliffs.town_mask != 0;
  if (detailed || s_world_mountains.active)
    s_world_art.sources = *ground;
  CaptureWorldNavigationArtVersion(slot, phase);
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

static void EnsureWorldNavigationCliffs(const FrameSlot *slot, float radius_tiles) {
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
  s_world_terrain.samples_ready = false;
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

/* Read-only after owner preparation; source-building workers share the frozen
 * height/cap arrays only until their bounded fork/join completes. */
static float WorldNavigationTerrainHeightAtPrepared(float source_x, float source_y,
                                                float *authored_weight, bool floor_only) {
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

static float WorldNavigationTerrainHeightAtImpl(float source_x, float source_y,
    float *authored_weight, bool floor_only) {
  PrepareWorldNavigationTerrain();
  return WorldNavigationTerrainHeightAtPrepared(source_x,source_y,authored_weight,floor_only);
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
  return slot->sim_manual_orbit_yaw != 0 || slot->sim_manual_orbit_pitch != 0;
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
  for (uint16_t i = 0; i < towns->object_count; i++) {
    const SimWorldNavigationTownObject *object = &towns->objects[i];
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

static bool WorldNavigationSurfaceNormal(
    const WorldNavigationProjection *projection,
    float source_x, float source_y, float normal[3]) {
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
    const WorldNavigationProjection *projection,
    float source_x, float source_y, bool terrain,
    float height_offset_world, float out[3]) {
  float normal[3];
  if (!out || !WorldNavigationSurfaceNormal(
          projection, source_x, source_y, normal)) return false;
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
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, bool terrain,
    float height_offset_world, ArRenderPointF *out) {
  float world[3];
  Scene3DPoint projected;
  if (!out || !WorldNavigationSurfaceWorldPoint(
          projection, source_x, source_y, terrain,
          height_offset_world, world) ||
      !Scene3D_ProjectWorldPoint(
          projection->matrix, world[0], world[1], world[2],
          viewport.w, viewport.h, &projected))
    return false;
  *out = (ArRenderPointF){viewport.x + projected.x,
                         viewport.y + projected.y};
  return true;
}

static float WorldNavigationShadeFromPoints(const float light[3],
    const float centre[3], const float east[3], const float south[3]) {
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

static float WorldNavigationSurfaceShade(
    bool lighting,
    const WorldNavigationProjection *projection,
    const float light[3], float source_x, float source_y) {
  if (!lighting) return 1.0f;
  const float step = (float)kSimWorldMapTilePixels * 0.5f;
  float centre[3], east[3], south[3];
  if (!WorldNavigationSurfaceWorldPoint(projection, source_x, source_y,
          true, 0.0f, centre) ||
      !WorldNavigationSurfaceWorldPoint(projection, source_x + step, source_y,
          true, 0.0f, east) ||
      !WorldNavigationSurfaceWorldPoint(projection, source_x, source_y + step,
          true, 0.0f, south)) return 1.0f;
  return WorldNavigationShadeFromPoints(light, centre, east, south);
}

static bool PrepareWorldNavigationGroundSamples(const WorldNavigationProjection *projection) {
  WorldNavigationGroundSampleKey key;
  memset(&key, 0, sizeof(key));
  key.chart_radius_tiles = projection->chart_radius_tiles;
  key.geography_serial = SimWorldMap_GeographySerial();
  key.cliff_serial = s_world_terrain.cliff_serial;
  key.heights = projection->height_world_per_unit > 0;
  if (s_world_terrain.samples_ready && !memcmp(&key, &s_world_terrain.sample_key, sizeof(key))) return true;
  s_world_terrain.samples_ready = false;
  for (int y = 0; y <= kWorldNavigationTerrainCells; ++y)
    for (int x = 0; x <= kWorldNavigationTerrainCells; ++x) {
      WorldNavigationGroundSample *sample = &s_world_terrain.samples[WorldNavigationTerrainVertexIndex(x,y)];
      for (int p = 0; p < 3; ++p) {
        const float source_x = (x + (p == 1 ? .5f : 0)) * kSimWorldMapTilePixels;
        const float source_y = (y + (p == 2 ? .5f : 0)) * kSimWorldMapTilePixels;
        if (!SimWorldNavigationGlobe_SampleAtRadius(key.chart_radius_tiles,
                source_x / kSimWorldMapTilePixels, source_y / kSimWorldMapTilePixels,
                sample->normal[p], NULL)) return false;
        sample->height[p] = key.heights ? WorldNavigationTerrainHeightAt(source_x, source_y, NULL) : 0;
      }
      const float edge_tiles = fminf(fminf((float)x, (float)y),
          fminf((float)(kWorldNavigationTerrainCells - x), (float)(kWorldNavigationTerrainCells - y)));
      sample->edge_alpha = WorldNavigationSmoothstep(edge_tiles / 10.0f);
      /* Preserve every land-adjacent corner of the chart's ocean blend strip. */
      if (sample->edge_alpha < 1)
        for (int dy = -1; dy <= 0; ++dy) for (int dx = -1; dx <= 0; ++dx) {
          const int cx = x + dx, cy = y + dy;
          if (cx >= 0 && cy >= 0 && cx < kWorldNavigationTerrainCells &&
              cy < kWorldNavigationTerrainCells && !SimWorldMap_CellIsOpenWater(cx, cy)) sample->edge_alpha = 1;
        }
    }
  s_world_terrain.sample_key = key;
  s_world_terrain.samples_ready = true;
  return true;
}

typedef struct WorldNavigationGroundWork {
  const WorldNavigationGroundSample *samples;
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  float light[3];
  bool lighting;
  ArRenderVertex2D *vertices;
  Sim3DDepthVertex *depth;
  Scene3DClipPoint *clip;
  uint8_t *outside;
  bool *valid;
} WorldNavigationGroundWork;

typedef struct WorldNavigationGridSourceWork {
  const WorldNavigationGroundSample *samples;
  Sim3DDepthSurfaceVertex *vertices;
  float height_ratio;
} WorldNavigationGridSourceWork;

static void WorldNavigationSourceShadeNormal(const WorldNavigationGroundSample *s,
    float height_ratio, float out[3]) {
  float point[3][3], tx[3], ty[3];
  for (int p = 0; p < 3; ++p) for (int axis = 0; axis < 3; ++axis)
    point[p][axis] = s->normal[p][axis] * (1 + s->height[p] * height_ratio);
  for (int axis = 0; axis < 3; ++axis) {
    tx[axis] = point[1][axis] - point[0][axis];
    ty[axis] = point[2][axis] - point[0][axis];
  }
  float normal[3] = {ty[1]*tx[2]-ty[2]*tx[1], ty[2]*tx[0]-ty[0]*tx[2], ty[0]*tx[1]-ty[1]*tx[0]};
  const float length = sqrtf(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
  for (int axis = 0; axis < 3; ++axis)
    out[axis] = length > 0 ? normal[axis]/length : s->normal[0][axis];
}

static void BuildWorldNavigationGridSourceRange(void *context, size_t first, size_t end) {
  WorldNavigationGridSourceWork *work = context;
  for (size_t i = first; i < end; ++i) {
    const WorldNavigationGroundSample *s = &work->samples[i];
    Sim3DDepthSurfaceVertex *v = &work->vertices[i];
    *v = (Sim3DDepthSurfaceVertex){.elevation = {s->height[0], 0},
      .color = {1,1,1,s->edge_alpha},
      .uv = {(i % kWorldNavigationTerrainAxis) / (float)kWorldNavigationTerrainCells,
             (i / kWorldNavigationTerrainAxis) / (float)kWorldNavigationTerrainCells}};
    memcpy(v->normal, s->normal[0], sizeof(v->normal));
    WorldNavigationSourceShadeNormal(s,work->height_ratio,v->shade_normal);
  }
}

typedef struct WorldNavigationCliffSourceWork {
  const SimWorldNavigationCliffFace *faces;
  Sim3DDepthSurfaceVertex *vertices;
  ArRenderPointF *mask_uv;
  bool *valid;
  float chart_radius, height_ratio;
} WorldNavigationCliffSourceWork;

static void BuildWorldNavigationCliffSourceRange(void *context, size_t first, size_t end) {
  WorldNavigationCliffSourceWork *work = context;
  for (size_t i = first; i < end; ++i) {
    const SimWorldNavigationCliffFace *face = &work->faces[i];
    work->valid[i] = true;
    for (unsigned p = 0; p < 4; ++p) {
      WorldNavigationGroundSample sample = {0};
      for (unsigned s = 0; s < 3; ++s) {
        const float x = face->x[p] + (s == 1 ? .5f : 0);
        const float y = face->y[p] + (s == 2 ? .5f : 0);
        work->valid[i] &= SimWorldNavigationGlobe_SampleAtRadius(work->chart_radius,
            x,y,sample.normal[s],NULL);
        sample.height[s] = WorldNavigationTerrainHeightAtPrepared(
            x*kSimWorldMapTilePixels,y*kSimWorldMapTilePixels,NULL,false);
      }
      Sim3DDepthSurfaceVertex *v = &work->vertices[i*4+p];
      *v = (Sim3DDepthSurfaceVertex){.elevation = {face->height[p],0},
        .color = {face->shade,face->shade,face->shade,1}, .uv = {face->u[p],face->v[p]}};
      memcpy(v->normal,sample.normal[0],sizeof(v->normal));
      WorldNavigationSourceShadeNormal(&sample,work->height_ratio,v->shade_normal);
      work->mask_uv[i*4+p] = (ArRenderPointF){face->x[p]/kWorldNavigationTerrainCells,
        face->y[p]/kWorldNavigationTerrainCells};
    }
  }
}

typedef struct WorldNavigationMountainSourceWork {
  const SimWorldNavigationMountainFace *faces;
  Sim3DDepthSurfaceVertex *vertices;
  ArRenderPointF *mask_uv;
  bool *valid;
  float chart_radius;
} WorldNavigationMountainSourceWork;

/* The owner prepares terrain before dispatch. Helpers read only immutable
 * source/terrain and write disjoint faces, including the original art UVs. */
static void BuildWorldNavigationMountainSourceRange(void *context, size_t first, size_t end) {
  WorldNavigationMountainSourceWork *work = context;
  for (size_t i = first; i < end; ++i) {
    const SimWorldNavigationMountainFace *face = &work->faces[i];
    work->valid[i] = true;
    for (unsigned p = 0; p < 4; ++p) {
      const float shade = face->brightness[p]/255.0f;
      Sim3DDepthSurfaceVertex *v = &work->vertices[i*4+p];
      *v = (Sim3DDepthSurfaceVertex){.color = {shade,shade,shade,1},
        .uv = {face->uv[p].x,face->uv[p].y}};
      float metric = 0;
      work->valid[i] &= SimWorldNavigationGlobe_SampleAtRadius(work->chart_radius,
          face->x[p],face->y[p],v->normal,&metric);
      v->elevation[0] = WorldNavigationTerrainHeightAtPrepared(
          face->x[p]*kSimWorldMapTilePixels,face->y[p]*kSimWorldMapTilePixels,NULL,true);
      v->elevation[1] = face->z[p]*metric;
      work->mask_uv[i*4+p] = v->uv; /* Cutout batches never use ground overlays. */
    }
  }
}

static WorldNavigationRadialBounds WorldNavigationSourceBounds(
    const Sim3DDepthSurfaceVertex *vertices, size_t quads) {
  WorldNavigationRadialBounds bounds = {0};
  for (size_t i = 0; i < quads*4; ++i) {
    const Sim3DDepthSurfaceVertex *v = &vertices[i];
    if (!i) {
      memcpy(bounds.normal_min,v->normal,sizeof(bounds.normal_min));
      memcpy(bounds.normal_max,v->normal,sizeof(bounds.normal_max));
      bounds.height_min = bounds.height_max = v->elevation[0];
    } else {
      for (unsigned axis = 0; axis < 3; ++axis) {
        bounds.normal_min[axis] = fminf(bounds.normal_min[axis],v->normal[axis]);
        bounds.normal_max[axis] = fmaxf(bounds.normal_max[axis],v->normal[axis]);
      }
      bounds.height_min = fminf(bounds.height_min,v->elevation[0]);
      bounds.height_max = fmaxf(bounds.height_max,v->elevation[0]);
    }
  }
  return bounds;
}

/* One immutable camera-local sphere. Each opaque/shadow consumer transforms
 * the same original quad, including the degenerate pole. Only uniforms follow
 * the camera; the scene explicitly supplies the chart-space shadow frame. */
static bool BuildWorldNavigationOceanSource(Sim3DDepthSurfaceVertex *quads, ArRenderPointF *mask) {
  Sim3DDepthSurfaceVertex *points = malloc(kWorldNavigationOceanVertexCount*sizeof(*points));
  if (!points) return false;
  size_t at = 0;
  for (int ring = 0; ring <= kWorldNavigationOceanRings; ++ring) {
    const float angle = ring/(float)kWorldNavigationOceanRings*kPi;
    const float sine = sinf(angle), cosine = cosf(angle);
    const float light = .72f+fmaxf(0,cosine)*.22f;
    for (int sector = 0; sector < (ring ? kWorldNavigationOceanSectors : 1); ++sector) {
      const float longitude = 2*kPi*sector/kWorldNavigationOceanSectors;
      points[at++] = (Sim3DDepthSurfaceVertex){
        .normal = {sine*cosf(longitude),sine*sinf(longitude),cosine},
        .elevation = {0,-.0025f}, .color = {.05f*light,.15f*light,.84f*light,1}, .uv = {-1,-1}};
    }
  }
  at = 0;
  for (int y = 0; y < kWorldNavigationOceanRings; ++y)
    for (int x = 0; x < kWorldNavigationOceanSectors; ++x) {
      const int next = (x+1)%kWorldNavigationOceanSectors;
      const int corners[4] = {y ? 1+(y-1)*kWorldNavigationOceanSectors+x : 0,
        y ? 1+(y-1)*kWorldNavigationOceanSectors+next : 0,
        1+y*kWorldNavigationOceanSectors+next,1+y*kWorldNavigationOceanSectors+x};
      for (unsigned p = 0; p < 4; ++p, ++at) {
        quads[at] = points[corners[p]]; mask[at] = (ArRenderPointF){0,0};
      }
    }
  free(points);
  return true;
}

static bool WorldNavigationShellFrame(const WorldNavigationProjection *projection,
    float radius, float centre_z, float outward[3], float right[3], float up[3], float *distance) {
  outward[0] = projection->camera_world[0]; outward[1] = projection->camera_world[1];
  outward[2] = projection->camera_world[2]-centre_z;
  const float eye_distance = hypotf(hypotf(outward[0],outward[1]),outward[2]);
  if (!isfinite(eye_distance) || eye_distance <= radius) return false;
  for (int i = 0; i < 3; ++i) outward[i] /= eye_distance;
  right[0] = outward[2]; right[1] = 0; right[2] = -outward[0];
  float length = hypotf(right[0],right[2]);
  if (length < .0001f) { right[0] = 1; right[2] = 0; length = 1; }
  for (int i = 0; i < 3; ++i) right[i] /= length;
  up[0] = outward[1]*right[2]-outward[2]*right[1];
  up[1] = outward[2]*right[0]-outward[0]*right[2];
  up[2] = outward[0]*right[1]-outward[1]*right[0];
  *distance = eye_distance;
  return true;
}

static bool WorldNavigationOceanTransform(const WorldNavigationProjection *projection,
    Sim3DDepthSurfaceTransform *out) {
  float outward[3], right[3], up[3], distance;
  const float reference = projection->reference_height_units*projection->height_world_per_unit;
  if (!WorldNavigationShellFrame(projection,projection->globe_radius_world*.9975f,
      -projection->globe_radius_world-reference,outward,right,up,&distance)) return false;
  *out = (Sim3DDepthSurfaceTransform){.radial = {.sphere_radius = projection->globe_radius_world,
    .reference_height = projection->reference_height_units, .height_scale = projection->height_world_per_unit},
    .extra_scale = projection->globe_radius_world, .ambient = 1};
  memcpy(out->radial.matrix,projection->matrix,sizeof(out->radial.matrix));
  for (unsigned row = 0; row < 3; ++row) {
    out->radial.basis[row][0] = right[row]; out->radial.basis[row][1] = up[row];
    out->radial.basis[row][2] = outward[row];
  }
  for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 3; ++col)
    out->shadow_basis[row][col] = projection->globe_frame.right[row]*out->radial.basis[0][col] +
      projection->globe_frame.up[row]*out->radial.basis[1][col] +
      projection->globe_frame.outward[row]*out->radial.basis[2][col];
  return true;
}

static bool WorldNavigationGpuGridEnabled(void) {
  if (!s_world_gpu_grid.attempted) {
    const char *enabled = getenv("AR_SIM3D_WORLD_GPU_GRID");
    /* Normal GPU world path; retain one explicit compatibility opt-out. */
    s_world_gpu_grid.enabled = !enabled || strcmp(enabled, "0");
    const char *cull = getenv("AR_SIM3D_WORLD_GPU_GRID_CULL");
    /* Reduce offscreen GPU work by default within the source-grid path;
     * keep an opt-out for backend comparisons and memory-constrained hosts. */
    s_world_gpu_grid.cull = !cull || strcmp(cull,"0");
    s_world_gpu_grid.attempted = true;
  }
  return s_world_gpu_grid.enabled;
}

static bool DrawWorldNavigationGpuGrid(const FrameSlot *slot,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms) {
  if (!WorldNavigationGpuGridEnabled()) return false;
  if (s_world_gpu_grid.unavailable) {
    if (!memcmp(&s_world_gpu_grid.rejected_projection, projection, sizeof(*projection)) &&
        s_world_gpu_grid.rejected_geography == SimWorldMap_GeographySerial() &&
        s_world_gpu_grid.rejected_cliffs == s_world_terrain.cliff_serial &&
        s_world_gpu_grid.rejected_mountains == s_world_mountains.geometry_revision) return false;
    s_world_gpu_grid.unavailable = false;
  }
  if (!PrepareWorldNavigationGroundSamples(projection)) goto unavailable;
  /* Avoid camera-scale floating noise in the source key. Above the radius
   * floor the ratio is exactly a scene setting divided by chart radius. */
  const float ratio = projection->globe_radius_world > .25f
      ? (slot->sim.world_navigation_relief ? slot->sim.landscape_height_pct / (float)kPercentScale : 0) /
          projection->chart_radius_tiles
      : projection->height_world_per_unit / projection->globe_radius_world;
  if (!s_world_gpu_grid.meshes.count) {
    Sim3DDepthPass_DestroyMesh(s_world_surfaces.mesh);
    s_world_surfaces.mesh = NULL; s_world_surfaces.published = false;
  }
  if (!s_world_gpu_grid.ready || !Sim3DMeshSet_Ready(&s_world_gpu_grid.meshes) ||
      memcmp(&s_world_gpu_grid.key, &s_world_terrain.sample_key, sizeof(s_world_gpu_grid.key)) ||
      s_world_gpu_grid.mountain_revision != s_world_mountains.geometry_revision ||
      ratio != s_world_gpu_grid.height_ratio) {
    const size_t cliff_count = s_world_terrain.cliffs.face_count;
    const size_t mountain_count = s_world_mountains.active ? s_world_mountains.scene.face_count : 0;
    size_t grid_capacity = kWorldNavigationOceanQuads;
    for (size_t i = 0; i < kWorldNavigationTerrainCells*kWorldNavigationTerrainCells; ++i)
      grid_capacity += !s_world_terrain.cliffs.replacement[i];
    if (cliff_count > kWorldNavigationSurfaceMaximumQuads-grid_capacity) goto unavailable;
    if (mountain_count > kWorldNavigationSurfaceMaximumQuads-grid_capacity-cliff_count) goto unavailable;
    const size_t capacity = grid_capacity+cliff_count+mountain_count;
    const size_t authored_count = cliff_count+mountain_count;
    Sim3DDepthSurfaceVertex *points = malloc(kWorldNavigationTerrainVertexCount*sizeof(*points));
    Sim3DDepthSurfaceVertex *quads = malloc(capacity*4*sizeof(*quads));
    ArRenderPointF *mask_uv = malloc(capacity*4*sizeof(*mask_uv));
    bool *valid = authored_count ? malloc(authored_count*sizeof(*valid)) : NULL;
    if (!points || !quads || !mask_uv || (authored_count && !valid)) {
      free(points); free(quads); free(mask_uv); free(valid); goto unavailable;
    }
    WorldNavigationGridSourceWork work = {s_world_terrain.samples, points, ratio};
    HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainVertexCount, 2048,
        BuildWorldNavigationGridSourceRange, &work);
    if (!BuildWorldNavigationOceanSource(quads,mask_uv)) {
      free(points); free(quads); free(mask_uv); free(valid); goto unavailable;
    }
    size_t count = kWorldNavigationOceanQuads;
    s_world_gpu_grid.chunks[0].range = (Sim3DDepthMeshRange){0,count};
    unsigned chunk = 1;
    for (int cy = 0; cy < kWorldNavigationTerrainCells; cy += kWorldNavigationGridChunkCells)
      for (int cx = 0; cx < kWorldNavigationTerrainCells; cx += kWorldNavigationGridChunkCells, ++chunk) {
        const size_t first = count;
        for (int y = cy; y < cy+kWorldNavigationGridChunkCells; ++y)
          for (int x = cx; x < cx+kWorldNavigationGridChunkCells; ++x) {
            if (s_world_terrain.cliffs.replacement[y*kWorldNavigationTerrainCells+x]) continue;
            const int at = WorldNavigationTerrainVertexIndex(x,y);
            const int corners[4] = {at,at+1,at+kWorldNavigationTerrainAxis+1,at+kWorldNavigationTerrainAxis};
            for (int p = 0; p < 4; ++p) {
              const Sim3DDepthSurfaceVertex *v = &points[corners[p]];
              quads[count*4+p] = *v;
              mask_uv[count*4+p] = v->uv;
            }
            ++count;
          }
        s_world_gpu_grid.chunks[chunk].bounds = WorldNavigationSourceBounds(quads+first*4,count-first);
        s_world_gpu_grid.chunks[chunk].range = (Sim3DDepthMeshRange){first,count-first};
      }
    if (authored_count) PrepareWorldNavigationTerrain();
    WorldNavigationCliffSourceWork cliffs = {s_world_terrain.cliffs.faces,
      quads+count*4,mask_uv+count*4,valid,projection->chart_radius_tiles,ratio};
    HostParallelWork_Run(WorldNavigationWorkers(),cliff_count,256,BuildWorldNavigationCliffSourceRange,&cliffs);
    bool ok = true;
    for (size_t i = 0; i < cliff_count; ++i) ok &= valid[i];
    s_world_gpu_grid.chunks[chunk].bounds = WorldNavigationSourceBounds(quads+count*4,cliff_count);
    s_world_gpu_grid.chunks[chunk].range = (Sim3DDepthMeshRange){count,cliff_count};
    count += cliff_count;
    WorldNavigationMountainSourceWork mountains = {s_world_mountains.scene.faces,
      quads+count*4,mask_uv+count*4,valid ? valid+cliff_count : NULL,projection->chart_radius_tiles};
    HostParallelWork_Run(WorldNavigationWorkers(),mountain_count,512,
        BuildWorldNavigationMountainSourceRange,&mountains);
    for (size_t i = cliff_count; i < authored_count; ++i) ok &= valid[i];
    s_world_gpu_grid.chunks[++chunk].range = (Sim3DDepthMeshRange){count,mountain_count};
    count += mountain_count;
    ok = ok && Sim3DMeshSet_UpdateSurface(&s_world_gpu_grid.meshes,quads,mask_uv,count);
    free(points); free(quads); free(mask_uv); free(valid);
    if (!ok) goto unavailable;
    s_world_gpu_grid.key = s_world_terrain.sample_key;
    s_world_gpu_grid.height_ratio = ratio; s_world_gpu_grid.ready = true;
    s_world_gpu_grid.source_quads = s_world_gpu_grid.selected_quads = count;
    s_world_gpu_grid.mountain_quads = mountain_count;
    s_world_gpu_grid.mountain_revision = s_world_mountains.geometry_revision;
    s_world_gpu_grid.selection_ready = false;
    s_world_gpu_grid.cull_unavailable = false;
    Sim3DPerformance_AddPath(kSim3DPath_Publish);
  }
  Sim3DDepthSurfaceTransform t = {.radial = {.sphere_radius = projection->globe_radius_world,
      .reference_height = projection->reference_height_units, .height_scale = projection->height_world_per_unit},
      .ambient = slot->sim.world_navigation_lighting ? kWorldNavigationTerrainAmbient : 1,
      .diffuse = slot->sim.world_navigation_lighting ? 1-kWorldNavigationTerrainAmbient : 0};
  memcpy(t.radial.matrix, projection->matrix, sizeof(t.radial.matrix));
  memcpy(t.radial.basis[0], projection->globe_frame.right, sizeof(t.radial.basis[0]));
  memcpy(t.radial.basis[1], projection->globe_frame.up, sizeof(t.radial.basis[1]));
  memcpy(t.radial.basis[2], projection->globe_frame.outward, sizeof(t.radial.basis[2]));
  if (s_world_gpu_grid.cull && !s_world_gpu_grid.cull_unavailable &&
      (!s_world_gpu_grid.selection_ready || memcmp(&s_world_gpu_grid.selection_transform,&t.radial,sizeof(t.radial)))) {
    Sim3DDepthMeshRange ranges[(kWorldNavigationSurfaceChunks+1)/2] = {0};
    size_t count = 0;
    bool culled = false;
    for (unsigned i = 0; i < kWorldNavigationSurfaceChunks; ++i) {
      const Sim3DDepthMeshRange r = s_world_gpu_grid.chunks[i].range;
      if (!r.quad_count) continue;
      /* Ocean is camera-local. Mountains have an independent extra-rise
       * scale absent from these land bounds. Keep both conservatively and
       * let hardware clip/depth reject them, including eye-plane crossings. */
      if (i && i != kWorldNavigationSurfaceChunks-1 &&
          WorldNavigationRadialBoundsOutside(&s_world_gpu_grid.chunks[i].bounds,&t.radial)) {
        culled = true; continue;
      }
      if (count && ranges[count-1].first_quad+ranges[count-1].quad_count == r.first_quad)
        ranges[count-1].quad_count += r.quad_count;
      else ranges[count++] = r;
    }
    /* A single zero-length range explicitly selects nothing; NULL/zero means
     * the whole source. Adjacent chunks merge without adding draw calls. */
    const bool ok = Sim3DMeshSet_SelectSurface(&s_world_gpu_grid.meshes,
        culled ? ranges : NULL, culled ? (count ? count : 1) : 0);
    if (!ok) {
      if (!Sim3DMeshSet_Ready(&s_world_gpu_grid.meshes)) goto unavailable;
      s_world_gpu_grid.cull_unavailable = true;
      Sim3DPerformance_AddPath(kSim3DPath_Rejected);
      fprintf(stderr,"[world-navigation] GPU grid selection unavailable; drawing full source\n");
    }
    s_world_gpu_grid.selected_quads = s_world_gpu_grid.source_quads;
    if (ok && culled) {
      s_world_gpu_grid.selected_quads = 0;
      for (size_t i = 0; i < count; ++i) s_world_gpu_grid.selected_quads += ranges[i].quad_count;
    }
    s_world_gpu_grid.selection_transform = t.radial;
    s_world_gpu_grid.selection_ready = true;
  }
  const float azimuth = slot->sim.light_azimuth_deg * kPi/180, elevation = slot->sim.light_elevation_deg * kPi/180;
  const float light[3] = {-cosf(azimuth)*cosf(elevation), -sinf(azimuth)*cosf(elevation), sinf(elevation)};
  for (int axis = 0; axis < 3; ++axis)
    t.light[axis] = t.radial.basis[0][axis]*light[0] + t.radial.basis[1][axis]*light[1] + t.radial.basis[2][axis]*light[2];
  Sim3DDepthSphericalSample shadows[kSimCloudLayerCount*3];
  const size_t shadow_count = WorldNavigationShadowSamples(slot, elapsed_ms, shadows);
  if (shadow_count && !EnsureWorldNavigationCloudTexture()) goto unavailable;
  Sim3DDepthSurfaceOverlay overlays[2]; size_t overlay_count = 0;
  if (slot->sim.world_navigation_haze) {
    const SimWorldNavigationScene *scene = &slot->sim.world_navigation_scene;
    const Sim3DDepthSurfaceOverlay mask = {
      .clear_rect = {scene->active_region_x / (float)kSimWorldMapPixels,
        scene->active_region_y / (float)kSimWorldMapPixels,
        scene->active_region_width / (float)kSimWorldMapPixels,
        scene->active_region_height / (float)kSimWorldMapPixels},
      .feather = scene->active_region_valid ? fmaxf(1, slot->sim.cull_haze_lead_px*.5f) / kSimWorldMapPixels : 0,
    };
    if (slot->sim.underlay_defocus_pct) {
      if (!EnsureWorldNavigationBlur(slot)) goto unavailable;
      overlays[overlay_count] = mask;
      overlays[overlay_count].layer = kSim3DDepthPass_GroundBlur;
      overlays[overlay_count++].color = (ArRenderColorF){1,1,1,slot->sim.underlay_defocus_pct/(float)kPercentScale};
    }
    if (slot->sim.underlay_haze_pct) {
      overlays[overlay_count] = mask;
      overlays[overlay_count].layer = kSim3DDepthPass_GroundHaze;
      overlays[overlay_count++].color = (ArRenderColorF){.24f,.37f,.56f,slot->sim.underlay_haze_pct/(float)kPercentScale*.35f};
    }
  }
  const size_t mountain_first = s_world_gpu_grid.selected_quads-s_world_gpu_grid.mountain_quads;
  Sim3DDepthSurfaceBatch batches[3] = {
    {.layer = kSim3DDepthPass_Ground, .range = {0,kWorldNavigationOceanQuads},
      .shadows = shadows, .shadow_count = shadow_count},
    {.layer = kSim3DDepthPass_Ground, .range = {kWorldNavigationOceanQuads,mountain_first-kWorldNavigationOceanQuads},
      .transform = t, .shadows = shadows, .shadow_count = shadow_count,
      .overlays = overlays, .overlay_count = overlay_count},
    {.layer = kSim3DDepthPass_WorldMountain, .range = {mountain_first,s_world_gpu_grid.mountain_quads},
      .transform = {.radial = t.radial, .extra_scale = projection->tile_world,
        .ambient = slot->sim.world_navigation_lighting ? .90f : 1}}};
  if (!WorldNavigationOceanTransform(projection,&batches[0].transform) ||
      !Sim3DMeshSet_AppendSurface(&s_world_gpu_grid.meshes,batches,3))
    goto unavailable;
  Sim3DPerformance_AddPath(kSim3DPath_GpuReuse);
  return true;
unavailable:
  Sim3DMeshSet_Destroy(&s_world_gpu_grid.meshes);
  s_world_gpu_grid.rejected_projection = *projection;
  s_world_gpu_grid.rejected_geography = SimWorldMap_GeographySerial();
  s_world_gpu_grid.rejected_cliffs = s_world_terrain.cliff_serial;
  s_world_gpu_grid.rejected_mountains = s_world_mountains.geometry_revision;
  s_world_gpu_grid.unavailable = true; s_world_gpu_grid.ready = false;
  Sim3DPerformance_AddPath(kSim3DPath_Rejected);
  fprintf(stderr, "[world-navigation] GPU world source rejected; retaining CPU surfaces\n");
  return false;
}

static void ProjectWorldNavigationGroundRange(void *context, size_t first, size_t end) {
  WorldNavigationGroundWork *work = context;
  const WorldNavigationProjection *projection = &work->projection;
  for (size_t at = first; at < end; ++at) {
    const WorldNavigationGroundSample *sample = &work->samples[at];
    float world[3][3];
    bool centre_valid = true, shade_valid = true;
    for (int p = 0; p < (work->lighting ? 3 : 1); ++p) {
      float normal[3], radial_height = 0;
      SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, sample->normal[p], normal);
      if (projection->height_world_per_unit > 0)
        radial_height += (sample->height[p] - projection->reference_height_units) * projection->height_world_per_unit;
      WorldNavigationRadialPoint(projection, normal, radial_height, world[p]);
      for (int j = 0; j < 3; ++j) {
        if (!p) centre_valid &= isfinite(world[p][j]);
        shade_valid &= isfinite(world[p][j]);
      }
    }
    Scene3DPoint output;
    work->valid[at] = centre_valid && WorldNavigationProjectPoint(projection, work->viewport,
        world[0], &output, &work->depth[at].depth, &work->clip[at]);
    if (!work->valid[at]) continue;
    work->depth[at].x = output.x; work->depth[at].y = output.y;
    work->depth[at].uv = (ArRenderPointF){-1,-1};
    work->outside[at] = projection->clip_frustum ? WorldNavigationClipOutside(work->clip[at])
        : WorldNavigationViewportOutside(output.x, output.y, work->viewport.w, work->viewport.h);
    const float shade = work->lighting && shade_valid
        ? WorldNavigationShadeFromPoints(work->light, world[0], world[1], world[2]) : 1;
    work->vertices[at] = (ArRenderVertex2D){
      {output.x, output.y}, {shade, shade, shade, sample->edge_alpha},
      {(at % kWorldNavigationTerrainAxis) / (float)kWorldNavigationTerrainCells,
       (at / kWorldNavigationTerrainAxis) / (float)kWorldNavigationTerrainCells},
    };
  }
}

static void PrepareWorldNavigationOceanIndices(void) {
  if (s_world_shells.indices_ready) return;
  int index_count = 0;
  for (int sector = 0; sector < kWorldNavigationOceanSectors; sector++) {
    const float longitude = 2.0f * kPi * sector / kWorldNavigationOceanSectors;
    s_world_shells.longitude_cos[sector] = cosf(longitude);
    s_world_shells.longitude_sin[sector] = sinf(longitude);
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
static bool PrepareWorldNavigationSphereShell(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection, WorldNavigationShell kind,
    WorldNavigationShellGeometry *geometry) {
  const bool occlusion = kind == kWorldNavigationShell_Atmosphere && WorldNavigationGpuGridEnabled();
  if (geometry->ready && geometry->occlusion == occlusion &&
      !memcmp(&geometry->projection, projection, sizeof(*projection)) &&
      !memcmp(&geometry->viewport, &viewport, sizeof(viewport))) return true;
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  geometry->ready = false;
  const bool atmosphere = kind == kWorldNavigationShell_Atmosphere;
  const bool cloud = kind == kWorldNavigationShell_Cloud;
  if (atmosphere) {
    s_world_shells.atmosphere_draw.ready = s_world_shells.atmosphere_draw.repeated = false;
    s_world_shells.atmosphere_draw.unavailable = false;
    s_world_shells.atmosphere_draw.quad_count = 0;
  }
  PrepareWorldNavigationOceanIndices();
  const float reference = projection->reference_height_units *
      projection->height_world_per_unit;
  const float shell_height = atmosphere
      ? projection->atmosphere_height_world
      : cloud ? projection->cloud_height_world
      : -reference - projection->globe_radius_world * 0.0025f;
  const float radius = projection->globe_radius_world + reference + shell_height;
  const float centre_z = -projection->globe_radius_world - reference;
  float outward[3], right[3], up[3], eye_distance;
  if (!WorldNavigationShellFrame(projection,radius,centre_z,outward,right,up,&eye_distance)) return false;
  const float maximum_angle = atmosphere || cloud ? acosf(radius / eye_distance) : kPi;
  const int first_ring = occlusion ? WorldNavigationOccludedShellRings(projection,
      radius, eye_distance, kWorldNavigationOceanRings, kWorldNavigationOceanSectors) : 0;
  geometry->occlusion = occlusion;
  geometry->first_index = first_ring
      ? kWorldNavigationOceanSectors * (3 + (first_ring - 1) * 6) : 0;
  geometry->first_vertex = first_ring ? 1 + (first_ring - 1) * kWorldNavigationOceanSectors : 0;
  Sim3DDepthVertex *depth_vertices = geometry->points;
  Scene3DClipPoint *clip_vertices = geometry->clip;
  int vertex_count = 0;
  for (int ring = 0; ring <= kWorldNavigationOceanRings; ring++) {
    const int sectors = ring ? kWorldNavigationOceanSectors : 1;
    if (ring < first_ring) { vertex_count += sectors; continue; }
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
    for (int sector = 0; sector < sectors; sector++) {
      const float cx = s_world_shells.longitude_cos[sector];
      const float sy = s_world_shells.longitude_sin[sector];
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
      if (atmosphere)
        s_world_shells.vertices[vertex_count] =
            (ArRenderVertex2D){{output.x, output.y}, colour, {0, 0}};
      ++vertex_count;
    }
  }
  if (atmosphere && !projection->clip_frustum)
    for (int i = geometry->first_index; i < kWorldNavigationOceanIndexCount; ++i)
      s_world_shells.atmosphere_indices[i - geometry->first_index] =
          s_world_shells.indices[i] - geometry->first_vertex;
  geometry->projection = *projection;
  geometry->viewport = viewport;
  geometry->ready = true;
  return true;
}

/* Memoize the existing clipped quad stream on a repeated view only. Camera
 * motion keeps the bounded scratch path; allocation/size failures likewise
 * fall back to drawing it. Inputs are copied, never borrowed from a renderer. */
static void CacheWorldNavigationAtmosphereBatch(const ArRenderVertex2D *vertices, size_t quads) {
  WorldNavigationAtmosphereDrawCache *cache = &s_world_shells.atmosphere_draw;
  enum { kMaximumQuads = 32768 };
  if (cache->unavailable) return;
  if (quads > kMaximumQuads - cache->quad_count) goto unavailable;
  const size_t needed = cache->quad_count + quads;
  if (needed > cache->capacity) {
    size_t capacity = cache->capacity ? cache->capacity * 2 : 1024;
    if (capacity < needed) capacity = needed;
    if (capacity > kMaximumQuads) capacity = kMaximumQuads;
    void *points = realloc(cache->vertices, capacity * 4 * sizeof(*cache->vertices));
    if (!points) goto unavailable;
    cache->vertices = points;
    void *indices = realloc(cache->indices, capacity * 6 * sizeof(*cache->indices));
    if (!indices) goto unavailable;
    cache->indices = indices;
    for (size_t i = cache->capacity; i < capacity; ++i) {
      const int corners[6] = {0, 1, 2, 0, 2, 3};
      for (int p = 0; p < 6; ++p) cache->indices[i * 6 + p] = (int32_t)(i * 4 + corners[p]);
    }
    cache->capacity = capacity;
  }
  memcpy(cache->vertices + cache->quad_count * 4, vertices, quads * 4 * sizeof(*vertices));
  cache->quad_count = needed;
  return;
unavailable:
  cache->unavailable = true; /* Retry on a changed view, not every frame. */
  cache->ready = false;
  free(cache->vertices); free(cache->indices);
  cache->vertices = NULL; cache->indices = NULL;
  cache->quad_count = cache->capacity = 0;
}

/* Shell geometry is immutable between camera/viewport changes. Each kind
 * owns its snapshot, so drawing clouds cannot overwrite the ocean receiver
 * or the atmospheric backdrop. Wind UVs and effect opacity remain dynamic. */
static bool DrawWorldNavigationSphereShell(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection, WorldNavigationShell kind) {
  const bool atmosphere = kind == kWorldNavigationShell_Atmosphere;
  const bool cloud = kind == kWorldNavigationShell_Cloud;
  WorldNavigationShellGeometry *geometry = atmosphere ? &s_world_shells.atmosphere
      : cloud ? &s_world_shells.cloud : &s_world_shells.ocean;
  if (!PrepareWorldNavigationSphereShell(viewport, projection, kind, geometry)) return false;
  const Sim3DDepthVertex *depth_vertices = geometry->points;
  const Scene3DClipPoint *clip_vertices = geometry->clip;
  if (cloud) return true;
  if (atmosphere) {
    const ArRenderDrawState state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Alpha,
    };
    if (projection->clip_frustum) {
      WorldNavigationAtmosphereDrawCache *cache = &s_world_shells.atmosphere_draw;
      if (cache->ready)
        return !cache->quad_count || ArRenderDevice_DrawGeometryWithState(&g_render_device,
            ArRenderTexture_Invalid(), cache->vertices, (int)cache->quad_count * 4,
            cache->indices, (int)cache->quad_count * 6, &state);
      const bool capture = cache->repeated && !cache->unavailable;
      cache->repeated = true;
      if (capture) cache->quad_count = 0;
      enum { kBatch = 64 };
      ArRenderVertex2D vertices[kBatch * 4];
      int32_t indices[kBatch * 6];
      for (int i = 0; i < kBatch; i++) {
        const int corners[6] = {0, 1, 2, 0, 2, 3};
        for (int p = 0; p < 6; p++) indices[i * 6 + p] = i * 4 + corners[p];
      }
      size_t used = 0;
      for (int i = geometry->first_index; i < kWorldNavigationOceanIndexCount; i += 3) {
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
            if (capture) CacheWorldNavigationAtmosphereBatch(vertices, used);
            if (!ArRenderDevice_DrawGeometryWithState(&g_render_device,
                    ArRenderTexture_Invalid(), vertices, (int)used * 4,
                    indices, (int)used * 6, &state)) return false;
            used = 0;
          }
        }
      }
      if (used) {
        if (capture) CacheWorldNavigationAtmosphereBatch(vertices, used);
        if (!ArRenderDevice_DrawGeometryWithState(&g_render_device,
                ArRenderTexture_Invalid(), vertices, (int)used * 4,
                indices, (int)used * 6, &state)) return false;
      }
      cache->ready = capture && !cache->unavailable;
      return true;
    }
    return ArRenderDevice_DrawGeometryWithState(
        &g_render_device, ArRenderTexture_Invalid(),
        s_world_shells.vertices + geometry->first_vertex,
        kWorldNavigationOceanVertexCount - geometry->first_vertex,
        s_world_shells.atmosphere_indices,
        kWorldNavigationOceanIndexCount - geometry->first_index, &state);
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
    return WorldNavigationAppendProjectedQuads(kSim3DDepthPass_Ground,
        batch, clip_batch, batch_count, viewport);
  }
  for (int i = 0; i < kWorldNavigationOceanIndexCount; i += 3) {
    Sim3DDepthVertex *triangle = batch + batch_count * 4;
    for (int corner = 0; corner < 3; corner++) {
      triangle[corner] = depth_vertices[s_world_shells.indices[i + corner]];
    }
    triangle[3] = triangle[2];
    if (++batch_count == kOceanBatchQuads) {
      if (!WorldNavigationAppendProjectedQuads(kSim3DDepthPass_Ground,
              batch,NULL,batch_count,viewport))
        return false;
      batch_count = 0;
    }
  }
  return WorldNavigationAppendProjectedQuads(kSim3DDepthPass_Ground,
      batch,NULL,batch_count,viewport);
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
  enum { kBatch = 64 };
  Sim3DDepthVertex batch[kBatch * 4];
  Scene3DClipPoint clip[kBatch * 4];
  size_t count = 0;
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
      Sim3DDepthVertex *face = batch + count * 4;
      for (int i = 0; i < 4; i++) {
        const int vertex = corners[i];
        face[i] = s_world_terrain.depth[vertex];
        face[i].color = colours[vertex].color;
        face[i].uv = layer == kSim3DDepthPass_GroundHaze
            ? (ArRenderPointF){-1.0f, -1.0f} : colours[vertex].tex_coord;
        if (projection->clip_frustum) clip[count * 4 + i] = s_world_terrain.clip[vertex];
      }
      if (++count == kBatch) {
        if (!WorldNavigationAppendProjectedQuads(layer, batch,
                projection->clip_frustum ? clip : NULL, count, viewport)) return false;
        count = 0;
      }
    }
  return !count || WorldNavigationAppendProjectedQuads(layer, batch,
      projection->clip_frustum ? clip : NULL, count, viewport);
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

static WorldNavigationGroundKey WorldNavigationGroundKeyFor(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  const SimWorldNavigationScene *scene = &slot->sim.world_navigation_scene;
  WorldNavigationGroundKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection;
  memcpy(key.source_to_screen, scene->source_to_screen,
         sizeof(key.source_to_screen));
  key.viewport = viewport;
  /* Shore opacity still follows geography when optional relief is off. */
  key.geography_serial = SimWorldMap_GeographySerial();
  key.snes_width = slot->snes_width;
  key.snes_height = slot->snes_height;
  key.visible_width = slot->visible_width;
  key.visible_x0 = slot->visible_x0;
  key.light_azimuth = slot->sim.light_azimuth_deg;
  key.light_elevation = slot->sim.light_elevation_deg;
  key.lighting = slot->sim.world_navigation_lighting;
  return key;
}

static bool DrawWorldNavigationCompatibilityGround(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!slot->sim.world_navigation_scene.valid || slot->visible_width <= 0 || slot->snes_height <= 0)
    return false;
  if (projection->height_world_per_unit > 0.0f) PrepareWorldNavigationTerrain();
  const WorldNavigationGroundKey key = WorldNavigationGroundKeyFor(slot, viewport, projection);
  if (!s_world_terrain.projection_ready ||
      memcmp(&key, &s_world_terrain.projection_key, sizeof(key)) != 0) {
    Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
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
    {
      if (!PrepareWorldNavigationGroundSamples(projection)) return false;
      bool valid[kWorldNavigationTerrainVertexCount];
      WorldNavigationGroundWork work = {
        .samples = s_world_terrain.samples, .projection = *projection, .viewport = viewport,
        .light = {light[0], light[1], light[2]}, .lighting = slot->sim.world_navigation_lighting,
        .vertices = s_world_terrain.vertices, .depth = s_world_terrain.depth,
        .clip = s_world_terrain.clip, .outside = s_world_terrain.outside, .valid = valid,
      };
      HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainVertexCount, 2048,
          ProjectWorldNavigationGroundRange, &work);
      for (int i = 0; i < kWorldNavigationTerrainVertexCount; ++i) if (!valid[i]) return false;
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
        const float shade = face->shade * WorldNavigationSurfaceShade(slot->sim.world_navigation_lighting, projection,
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

static bool DrawWorldNavigationMountains(const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection);

static bool DrawWorldNavigationSurfaceLayers(const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms) {
  bool ok = Sim3DDepthPass_Begin(&g_render_device, viewport.w, viewport.h, kArRenderFilter_Linear);
  if (!ok) return false;
  if (s_world_art.displayed_version >= 0 &&
      !Sim3DDepthPass_SelectAtlasVersion(s_world_art.atlas_cache,
          (unsigned)s_world_art.displayed_version)) return false;
  const Sim3DPerformanceScope source = Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
  const bool gpu = DrawWorldNavigationGpuGrid(slot,projection,elapsed_ms);
  Sim3DPerformance_End(source);
  if (s_world_gpu_grid.drawn != gpu) s_world_terrain.projection_ready = false;
  s_world_gpu_grid.drawn = gpu;
  if (gpu) {
    /* All opaque surfaces and their permitted depth-matched consumers are
     * queued atomically. Cutout holes remain holes, with no CPU receivers. */
    return true;
  }
  const Sim3DPerformanceScope ocean = Sim3DPerformance_Begin(kSim3DPerformance_WorldOcean);
  const WorldNavigationGroundKey key = WorldNavigationGroundKeyFor(slot, viewport, projection);
  const bool repeated = s_world_surfaces.key_ready &&
      s_world_surfaces.cliff_serial == s_world_terrain.cliff_serial &&
      s_world_surfaces.mountain_revision == s_world_mountains.geometry_revision &&
      !memcmp(&key, &s_world_surfaces.retained_key, sizeof(key));
  if (!s_world_surfaces.attempted) {
    const char *enabled = getenv("AR_SIM3D_RETAINED_GROUND");
    s_world_surfaces.attempted = true;
    /* Same shader/geometry as ordinary submission; keep a startup opt-out
     * for driver diagnosis without changing saved graphics quality. */
    s_world_surfaces.opt_out = enabled && strcmp(enabled, "0") == 0;
    s_world_surfaces.unavailable = s_world_surfaces.opt_out;
  }
  /* Preserve ocean THEN mainland/cliff order within Ground, plus the separate
   * WorldMountain material. The adapter's atomic append queues all ranges or
   * none, so resource pressure cannot duplicate only half of the surfaces.
   * CPU arrays remain valid for exact-depth weather/haze consumers. */
  const bool retained = ok && !s_world_surfaces.unavailable && repeated &&
      s_world_surfaces.published && s_world_terrain.projection_ready &&
      s_world_shells.ocean.ready && Sim3DDepthPass_MeshReady(s_world_surfaces.mesh) &&
      Sim3DDepthPass_AppendGeometryRanges(s_world_surfaces.mesh, s_world_surfaces.ranges, 2);
  if (!retained) {
    Sim3DPerformance_AddPath(kSim3DPath_CpuStage);
    if (s_world_surfaces.unavailable)
      Sim3DPerformance_AddPath(s_world_surfaces.opt_out ? kSim3DPath_OptOut : kSim3DPath_Rejected);
    s_world_surfaces.published = false;
    ok = ok && DrawWorldNavigationSphereShell(viewport, projection, kWorldNavigationShell_Ocean);
  }
  Sim3DPerformance_End(ocean);
  if (!ok) return false;
  if (retained) {
    Sim3DPerformance_AddPath(kSim3DPath_GpuReuse);
  } else {
    const Sim3DPerformanceScope terrain = Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
    ok = DrawWorldNavigationCompatibilityGround(slot, viewport, projection);
    Sim3DPerformance_End(terrain);
  }
  if (ok && (!retained || !s_world_surfaces.mountains_retained)) {
    const Sim3DPerformanceScope mountain = Sim3DPerformance_Begin(kSim3DPerformance_DepthMountain);
    ok = DrawWorldNavigationMountains(slot, viewport, projection);
    Sim3DPerformance_End(mountain);
  }
  if (ok && !retained && repeated && !s_world_surfaces.unavailable) {
    const Sim3DPerformanceScope publication = Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
    const Sim3DDepthPassLayer layers[] = {kSim3DDepthPass_Ground, kSim3DDepthPass_WorldMountain};
    if (!s_world_surfaces.mesh) s_world_surfaces.mesh = Sim3DDepthPass_CreateGeometryMesh();
    s_world_surfaces.mountains_retained = s_world_surfaces.mesh &&
        Sim3DDepthPass_CaptureGeometryLayers(s_world_surfaces.mesh, layers, 2, s_world_surfaces.ranges);
    s_world_surfaces.published = s_world_surfaces.mountains_retained;
    if (!s_world_surfaces.published && s_world_surfaces.mesh) {
      /* An unusually large cutout set must not evict the old ground-only
       * optimization. Both captures reject before modifying queued draws. */
      s_world_surfaces.ranges[1] = (Sim3DDepthGeometryRange){.layer = kSim3DDepthPass_WorldMountain};
      s_world_surfaces.published = Sim3DDepthPass_CaptureGeometryLayers(
          s_world_surfaces.mesh, layers, 1, s_world_surfaces.ranges);
    }
    if (!s_world_surfaces.published) s_world_surfaces.unavailable = true;
    Sim3DPerformance_AddPath(s_world_surfaces.published ? kSim3DPath_Publish : kSim3DPath_Rejected);
    Sim3DPerformance_End(publication);
  }
  s_world_surfaces.key_ready = ok;
  s_world_surfaces.retained_key = key;
  s_world_surfaces.cliff_serial = s_world_terrain.cliff_serial;
  s_world_surfaces.mountain_revision = s_world_mountains.geometry_revision;
  return ok;
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
  if (!s_world_models.capturing || !s_world_models.capture_static)
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
  size_t maximum_vertices = (8 * 1024 * 1024 / sizeof(Sim3DDepthVertex)) & ~(size_t)3;
#if AR_WORLD_NAV_CACHE_TESTING
  if (s_model_test_bytes / sizeof(Sim3DDepthVertex) < maximum_vertices)
    maximum_vertices = (s_model_test_bytes / sizeof(Sim3DDepthVertex)) & ~(size_t)3;
#endif
  if (needed > maximum_vertices) {
    s_world_models.capturing = false;
    s_world_models.projection_unavailable = true; /* Do not recopy an oversized view every frame. */
    return true;
  }
  if (needed > s_world_models.projected_capacity) {
    size_t capacity = s_world_models.projected_capacity ? s_world_models.projected_capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > maximum_vertices) capacity = maximum_vertices;
    void *points = realloc(s_world_models.projected, capacity * sizeof(*s_world_models.projected));
    if (!points) {
      s_world_models.capturing = false;
      s_world_models.projection_unavailable = true; /* Retry when the view changes. */
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

/* Bounded, presentation-owned staging. Cache views are copied before another
 * Get can evict them; helpers only see these immutable values. Output ranges
 * are disjoint and the owner submits them in original object/face order. */
enum { kWorldModelBatchObjects = 128, kWorldModelBatchFaces = 8192 };
typedef struct WorldNavigationModelJob {
  SimBackgroundVoxelModelView model;
  SimBackgroundVoxelModelShading shading;
  SimBackgroundVoxelPalette palette;
  SimBackgroundVoxelBiome biome;
  SimBackgroundVoxelDetail detail;
  float source_x, source_y, centre_x, centre_y, footprint_scale, base, height_scale;
  size_t first;
  uint16_t object_index;
  bool animated;
} WorldNavigationModelJob;
typedef struct WorldNavigationModelFaceOutput {
  Sim3DDepthVertex vertices[4];
  Scene3DClipPoint clip[4];
  bool valid;
} WorldNavigationModelFaceOutput;
typedef struct WorldNavigationModelWork {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  bool lighting;
  const WorldNavigationModelJob *jobs;
  WorldNavigationModelFaceOutput *output;
} WorldNavigationModelWork;
typedef struct WorldNavigationModelBatch {
  WorldNavigationModelJob jobs[kWorldModelBatchObjects];
  SimBackgroundVoxelModelFace faces[kWorldModelBatchFaces];
  uint8_t material[kWorldModelBatchFaces], brightness[kWorldModelBatchFaces][4];
  WorldNavigationModelFaceOutput output[kWorldModelBatchFaces];
  size_t objects, face_count;
} WorldNavigationModelBatch;
_Static_assert(sizeof(WorldNavigationModelBatch) <= 3 * 1024 * 1024,
    "model worker staging must stay within its 3 MiB budget");
static WorldNavigationModelBatch *s_world_model_batch;
static bool s_world_model_batch_unavailable;

static void ProjectWorldNavigationModelRange(void *context, size_t first, size_t end) {
  WorldNavigationModelWork *work = context;
  const WorldNavigationProjection *projection = &work->projection;
  enum { kColumnCacheCount = 512 };
  struct ColumnProjection { uint32_t stamp, x_bits, y_bits; float normal[3]; };
  struct ColumnProjection columns[kColumnCacheCount] = {0};
  for (size_t i = first; i < end; ++i) {
    const WorldNavigationModelJob *job = &work->jobs[i];
    const uint32_t stamp = (uint32_t)i + 1;
    for (uint16_t face = 0; face < job->model.face_count; ++face) {
      const SimBackgroundVoxelModelFace *authored = &job->model.faces[face];
      const SimBackgroundVoxelMaterial material = work->lighting
          ? (SimBackgroundVoxelMaterial)job->shading.material[face]
          : SimBackgroundVoxelBiome_SurfaceMaterial(job->biome, job->detail,
              (SimBackgroundVoxelMaterial)authored->material, authored);
      const uint32_t argb = SimBackgroundVoxelPalette_Base(&job->palette, material);
      WorldNavigationModelFaceOutput *out = &work->output[job->first + face];
      out->valid = true;
      for (int point = 0; point < 4; ++point) {
        const SimBackgroundVoxelModelPoint *p = &authored->points[point];
        const float x = job->centre_x + (p->x - job->centre_x) * job->footprint_scale;
        const float y = job->centre_y + (p->y - job->centre_y) * job->footprint_scale;
        uint32_t x_bits, y_bits;
        memcpy(&x_bits, &p->x, sizeof(x_bits));
        memcpy(&y_bits, &p->y, sizeof(y_bits));
        const uint32_t hash = DeterministicHash_Mix32(x_bits ^ DeterministicHash_Mix32(y_bits));
        struct ColumnProjection *column = &columns[hash & (kColumnCacheCount - 1)];
        if (column->stamp != stamp || column->x_bits != x_bits || column->y_bits != y_bits) {
          if (!WorldNavigationSurfaceNormal(projection,
                  job->source_x + x * ((float)kSimWorldMapTilePixels / kSimTownCellPixels),
                  job->source_y + y * ((float)kSimWorldMapTilePixels / kSimTownCellPixels),
                  column->normal)) {
            out->valid = false;
            break;
          }
          column->stamp = stamp; column->x_bits = x_bits; column->y_bits = y_bits;
        }
        float world[3];
        WorldNavigationRadialPoint(projection, column->normal, job->base + p->z * job->height_scale, world);
        Scene3DPoint projected;
        Sim3DDepthVertex *vertex = &out->vertices[point];
        if (!WorldNavigationProjectPoint(projection, work->viewport, world,
                &projected, &vertex->depth, &out->clip[point])) {
          out->valid = false;
          break;
        }
        vertex->x = projected.x; vertex->y = projected.y;
        vertex->uv = (ArRenderPointF){-1, -1};
        const float shade = work->lighting
            ? 0.74f + 0.18f * job->shading.brightness[face][point] / 255.0f : 0.88f;
        vertex->color = (ArRenderColorF){
          ((argb >> 16) & 255) / 255.0f * shade,
          ((argb >> 8) & 255) / 255.0f * shade,
          (argb & 255) / 255.0f * shade, (argb >> 24) / 255.0f,
        };
      }
    }
  }
}

static bool SubmitWorldNavigationModelJob(const WorldNavigationModelJob *job,
    const WorldNavigationModelFaceOutput *output,
    const WorldNavigationProjection *projection, ArRenderRectI viewport) {
  s_world_models.capture_static = !job->animated;
  if (s_world_models.capturing && job->animated) {
    s_world_models.animated[s_world_models.animated_count++] = (WorldNavigationAnimatedModel){
      .static_end = (uint32_t)s_world_models.projected_count,
      .object = job->object_index, .detail = (uint8_t)job->detail,
    };
  }
  for (size_t face = job->first; face < job->first + job->model.face_count; ++face) {
    const WorldNavigationModelFaceOutput *out = &output[face];
    if (out->valid && !WorldNavigationAppendModelQuad(out->vertices,
            projection->clip_frustum ? out->clip : NULL, viewport)) return false;
  }
  return true;
}

static bool FlushWorldNavigationModels(WorldNavigationModelBatch *batch,
    const WorldNavigationProjection *projection, ArRenderRectI viewport, bool lighting) {
  if (!batch || !batch->objects) return true;
  WorldNavigationModelWork work = {.projection = *projection, .viewport = viewport,
    .lighting = lighting, .jobs = batch->jobs, .output = batch->output};
  const Sim3DPerformanceScope scope = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  HostParallelWork_Run(WorldNavigationWorkers(), batch->objects, 16,
      ProjectWorldNavigationModelRange, &work);
  bool valid = true;
  for (size_t i = 0; i < batch->objects; ++i)
    if (!SubmitWorldNavigationModelJob(&batch->jobs[i], batch->output, projection, viewport)) valid = false;
  batch->objects = batch->face_count = 0;
  Sim3DPerformance_End(scope);
  return valid;
}

static bool WorldNavigationAppendAuthoredModel(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const WorldNavigationVisibleTownObject *visible, WorldNavigationModelBatch *batch) {
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
      model->overflow || !model->face_count || model->face_count > kSimBackgroundVoxelModelMaxFaces)
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
  const bool occluded = SimWorldNavigationGlobe_CapOccluded(camera, transformed_anchor,
      angular_radius, maximum_radius, occluder_radius);
  if (occluded && object.kind != kSimBackgroundVoxel_Windmill) return true;
  WorldNavigationModelJob job = {
    .model = *model, .shading = shading ? *shading : (SimBackgroundVoxelModelShading){0},
    .palette = palette, .biome = biome, .detail = visible->detail,
    .source_x = source_x, .source_y = source_y, .centre_x = centre_x, .centre_y = centre_y,
    .footprint_scale = proportions->footprint_scale, .base = base, .height_scale = height_scale,
    .animated = object.kind == kSimBackgroundVoxel_Windmill,
    .object_index = (uint16_t)(visible->object - slot->sim.world_navigation_towns.objects),
  };
  /* Keep an empty animated span when this pose is occluded: another blade
   * pose may extend beyond its current cap while the camera remains held. */
  if (occluded) job.model.face_count = 0;
  if (batch) {
    /* Flushing performs no compiler/cache lookup, so this pending borrowed
     * model remains valid until copied, even when the preceding batch fills. */
    if (batch->objects == kWorldModelBatchObjects ||
        batch->face_count + job.model.face_count > kWorldModelBatchFaces)
      if (!FlushWorldNavigationModels(batch, projection, viewport, slot->sim.world_navigation_lighting))
        return false;
    job.first = batch->face_count;
    memcpy(batch->faces + job.first, model->faces, job.model.face_count * sizeof(*model->faces));
    job.model.faces = batch->faces + job.first;
    if (shading) {
      memcpy(batch->material + job.first, shading->material, job.model.face_count);
      memcpy(batch->brightness + job.first, shading->brightness, job.model.face_count * sizeof(*shading->brightness));
      job.shading.material = batch->material + job.first;
      job.shading.brightness = batch->brightness + job.first;
    }
    batch->jobs[batch->objects++] = job;
    batch->face_count += job.model.face_count;
    return true;
  }
  /* Small/held views and allocation failure use the same math synchronously.
   * Only this owner-only call borrows cache data; no Get occurs before return. */
  WorldNavigationModelFaceOutput output[kSimBackgroundVoxelModelMaxFaces];
  WorldNavigationModelWork work = {.projection = *projection, .viewport = viewport,
    .lighting = slot->sim.world_navigation_lighting, .jobs = &job, .output = output};
  const Sim3DPerformanceScope project = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  ProjectWorldNavigationModelRange(&work, 0, 1);
  const bool valid = SubmitWorldNavigationModelJob(&job, output, projection, viewport);
  Sim3DPerformance_End(project);
  return valid;
}

/* Use local projected area for distance selection: a tile at the globe limb
 * occupies much less screen area than the tile under the Palace. */
static bool WorldNavigationTownFootprintPixels(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, float *out_pixels,
    ArRenderPointF *out_centre) {
  ArRenderPointF centre, east, south;
  if (!out_pixels ||
      !WorldNavigationProjectSurface(
          viewport, projection, source_x, source_y,
          true, 0.0f, &centre) ||
      !WorldNavigationProjectSurface(
          viewport, projection,
          source_x + kSimWorldMapTilePixels, source_y,
          true, 0.0f, &east) ||
      !WorldNavigationProjectSurface(
          viewport, projection,
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

static bool SampleWorldNavigationMountainFace(const SimWorldNavigationMountainFace *face,
    const WorldNavigationProjection *projection, WorldNavigationMountainProjection *sample) {
  for (int p = 0; p < 4; ++p) {
    float metric;
    if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles,
            face->x[p], face->y[p], sample->normal[p], &metric)) return false;
    sample->rise[p] = face->z[p] * metric;
    sample->floor[p] = WorldNavigationTerrainHeightAtImpl(
        face->x[p] * kSimWorldMapTilePixels, face->y[p] * kSimWorldMapTilePixels, NULL, true);
  }
  return true;
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
    if (!SampleWorldNavigationMountainFace(&s_world_mountains.scene.faces[i],
            projection, &s_world_mountains.projection[i])) return false;
  }
  s_world_mountains.samples_ready = true;
  s_world_mountains.projection_ready = false;
  return true;
}

static bool ProjectWorldNavigationMountainFace(
    const SimWorldNavigationMountainFace *face, const WorldNavigationMountainProjection *sample,
    bool lighting, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, Sim3DDepthVertex vertices[4],
    Scene3DClipPoint clip[4]) {
  for (int p = 0; p < 4; p++) {
    float normal[3], world[3];
    memcpy(normal, sample->normal[p], sizeof(normal));
    const float floor = sample->floor[p], rise = sample->rise[p];
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
        (lighting ? 0.90f : 1.0f);
    vertices[p].x = screen.x; vertices[p].y = screen.y;
    vertices[p].uv = (ArRenderPointF){face->uv[p].x, face->uv[p].y};
    vertices[p].color = (ArRenderColorF){shade, shade, shade, 1};
  }
  return true;
}

typedef struct WorldNavigationMountainWork {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  bool lighting;
  const SimWorldNavigationMountainFace *faces;
  WorldNavigationMountainProjection *samples;
} WorldNavigationMountainWork;

static void ProjectWorldNavigationMountainRange(void *context, size_t first, size_t end) {
  WorldNavigationMountainWork *work = context;
  for (size_t i = first; i < end; ++i) {
    WorldNavigationMountainProjection *sample = &work->samples[i];
    sample->visible = ProjectWorldNavigationMountainFace(&work->faces[i], sample,
        work->lighting, work->viewport, &work->projection, sample->points, sample->clip);
  }
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
  if (cached && project) {
    WorldNavigationMountainWork work = {.projection = *projection, .viewport = viewport,
      .lighting = slot->sim.world_navigation_lighting, .faces = s_world_mountains.scene.faces,
      .samples = s_world_mountains.projection};
    HostParallelWork_Run(WorldNavigationWorkers(), s_world_mountains.scene.face_count, 512,
        ProjectWorldNavigationMountainRange, &work);
  }
  Sim3DDepthVertex batch[64 * 4];
  Scene3DClipPoint clip[64 * 4];
  size_t count = 0;
  for (size_t at = 0; at < s_world_mountains.scene.face_count; at++) {
    const SimWorldNavigationMountainFace *face = &s_world_mountains.scene.faces[at];
    Sim3DDepthVertex *vertices = batch + count * 4;
    bool valid;
    if (cached) {
      WorldNavigationMountainProjection *sample = &s_world_mountains.projection[at];
      valid = sample->visible;
      if (valid) memcpy(vertices, sample->points, sizeof(sample->points));
      if (valid && projection->clip_frustum)
        memcpy(clip + count * 4, sample->clip, sizeof(sample->clip));
    } else {
      WorldNavigationMountainProjection sample;
      valid = SampleWorldNavigationMountainFace(face, projection, &sample) &&
          ProjectWorldNavigationMountainFace(face, &sample, slot->sim.world_navigation_lighting,
              viewport, projection, vertices, clip + count * 4);
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
  return WorldNavigationAppendProjectedQuads(kSim3DDepthPass_WorldMountain, batch,
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

static Sim3DDepthRadialTransform WorldNavigationRadialTransform(const FrameSlot *slot,
    const WorldNavigationProjection *projection) {
  Sim3DDepthRadialTransform transform = {
    .sphere_radius = projection->globe_radius_world,
    .reference_height = projection->reference_height_units,
    .height_scale = projection->height_world_per_unit,
    .variant = (unsigned)((slot->sim.game_frame / 12) % 3) + 1,
  };
  memcpy(transform.matrix, projection->matrix, sizeof(transform.matrix));
  memcpy(transform.basis[0], projection->globe_frame.right, sizeof(transform.basis[0]));
  memcpy(transform.basis[1], projection->globe_frame.up, sizeof(transform.basis[1]));
  memcpy(transform.basis[2], projection->globe_frame.outward, sizeof(transform.basis[2]));
  return transform;
}

static bool DrawWorldNavigationGpuModels(const FrameSlot *slot,
    const WorldNavigationProjection *projection,
    const WorldNavigationVisibleTownObject *visible, size_t count) {
  s_world_models.gpu_current_ready = false;
  if (count > s_world_models.gpu_source_capacity) {
    void *sources = realloc(s_world_models.gpu_sources, count * sizeof(*s_world_models.gpu_sources));
    if (!sources) { s_world_models.gpu_sources_unavailable = true; return false; }
    s_world_models.gpu_sources = sources;
    s_world_models.gpu_source_capacity = count;
  }
  WorldNavigationModelSource *sources = s_world_models.gpu_sources;
  size_t source_count = 0;
  const float source_scale = (float)kSimWorldMapTilePixels / kSimTownCellPixels;
  const float camera[3] = {projection->camera_world[0], projection->camera_world[1],
    projection->camera_world[2] + projection->globe_radius_world +
        projection->reference_height_units * projection->height_world_per_unit};
  const float occluder = projection->globe_radius_world * .9975f *
      cosf(kPi / kWorldNavigationOceanRings + 2 * kPi / kWorldNavigationOceanSectors);
  for (size_t i = 0; i < count; ++i) {
    const SimWorldNavigationTownObject *object = visible[i].object;
    const SimBackgroundBridgeBounds bounds = WorldNavigationObjectBounds(object);
    int town_x, town_y;
    if (!SimWorldMap_OriginForTown(object->town, &town_x, &town_y)) return false;
    WorldNavigationModelSource source;
    memset(&source, 0, sizeof(source));
    source.object = *object;
    source.object_index = (uint16_t)(object - slot->sim.world_navigation_towns.objects);
    /* All three authored poses are in the retained source, not its key. */
    if (object->kind == kSimBackgroundVoxel_Windmill) source.object.animation_phase = 0;
    source.detail = visible[i].detail;
    source.source_x = town_x * kSimWorldMapTilePixels + bounds.origin_x * source_scale;
    source.source_y = town_y * kSimWorldMapTilePixels + bounds.origin_y * source_scale;
    source.centre_x = bounds.width * .5f;
    source.centre_y = bounds.depth * .5f;
    const float anchor_x = source.source_x + source.centre_x * source_scale;
    const float anchor_y = source.source_y + source.centre_y * source_scale;
    source.anchor_height = projection->height_world_per_unit > 0
        ? WorldNavigationTerrainHeightAt(anchor_x, anchor_y, NULL) : 0;
    float normal[3], metric;
    if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles,
            anchor_x / kSimWorldMapTilePixels, anchor_y / kSimWorldMapTilePixels, normal, &metric)) return false;
    SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, normal, normal);
    const WorldNavigationModelBounds *bound = &s_world_models.bounds[
        object - slot->sim.world_navigation_towns.objects];
    const float maximum_radius = projection->globe_radius_world +
        source.anchor_height * projection->height_world_per_unit +
        bound->maximum_rise * metric * projection->tile_world * slot->sim.height_scale_x100 / kPercentScale;
    /* These cached bounds include every authored pose/LOD. Keep this cheap
     * whole-object test on the CPU; uncertain/partially visible faces go GPU. */
    if (bound->angular_radius > 0 && SimWorldNavigationGlobe_CapOccluded(camera, normal, bound->angular_radius,
            maximum_radius, occluder)) continue;
    sources[source_count++] = source;
  }
  if (!source_count) return true;
  WorldNavigationModelSourceStyle style;
  memset(&style, 0, sizeof(style));
  style.model_revision = s_world_models.revision;
  style.surface_revision = s_world_terrain.cliff_serial;
  style.chart_radius_tiles = projection->chart_radius_tiles;
  style.tile_world = projection->tile_world;
  style.height_percent = slot->sim.height_scale_x100;
  style.light_azimuth = slot->sim.light_azimuth_deg;
  style.light_elevation = slot->sim.light_elevation_deg;
  style.style = (SimBackgroundVoxelStyle)slot->sim.background_voxel_style;
  style.lighting = slot->sim.world_navigation_lighting;
  const Sim3DDepthRadialTransform transform = WorldNavigationRadialTransform(slot, projection);
  s_world_models.gpu_current_ready = WorldNavigationModelMesh_Draw(sources, source_count, &style, &transform);
  return s_world_models.gpu_current_ready;
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
  const bool same_projection = s_world_models.projection_key_ready &&
      !memcmp(&key, &s_world_models.projection_key, sizeof(key));
  if (!same_projection) {
    s_world_models.projection_unavailable = false;
    s_world_models.solid_mesh_unavailable = false;
  }
  /* A declined selection still gets the held CPU/multicore cache. Retry GPU
   * selection on a changed view, not every frame of the same oversized view. */
  const bool gpu_models = WorldNavigationModelMesh_Enabled() && !s_world_models.gpu_sources_unavailable &&
      !(same_projection && s_world_models.gpu_current_rejected);
  if (gpu_models && same_projection && s_world_models.gpu_current_ready) {
    const Sim3DDepthRadialTransform transform = WorldNavigationRadialTransform(slot, projection);
    if (WorldNavigationModelMesh_Repeat(&transform)) return true;
  }
  s_world_models.gpu_current_ready = false;
  if (!gpu_models && s_world_models.projected_valid && same_projection) {
    /* Bound draw-call amplification, not model count or visual quality.
     * Many interleaved windmills can turn one ordinary batch into hundreds
     * of small draws; those views keep the existing projected CPU cache. */
    enum { kMaximumRetainedStaticSpans = 16 };
    unsigned static_spans = 0;
    size_t previous = 0;
    for (unsigned i = 0; i <= s_world_models.animated_count; ++i) {
      const size_t end = i < s_world_models.animated_count
          ? s_world_models.animated[i].static_end : s_world_models.projected_count;
      if (end > previous && ++static_spans > kMaximumRetainedStaticSpans) break;
      previous = end;
    }
    const bool retain = static_spans <= kMaximumRetainedStaticSpans;
    /* Retain the existing clipped/facing/projection result byte-for-byte;
     * moving views still use the unchanged multicore path. No saved setting
     * or frame/runner contract is extended. Zero is a diagnostic opt-out. */
    if (!s_world_models.solid_mesh_attempted) {
      const char *enabled = getenv("AR_SIM3D_RETAINED_SOLIDS");
      s_world_models.solid_mesh_attempted = true;
      s_world_models.solid_mesh_opt_out = enabled && strcmp(enabled, "0") == 0;
      s_world_models.solid_mesh_unavailable = s_world_models.solid_mesh_opt_out;
    }
    if (!retain) Sim3DPerformance_AddPath(kSim3DPath_Limit);
    if (s_world_models.solid_mesh_unavailable)
      Sim3DPerformance_AddPath(s_world_models.solid_mesh_opt_out ? kSim3DPath_OptOut : kSim3DPath_Rejected);
    if (retain && !s_world_models.solid_mesh_unavailable && s_world_models.projected_count) {
      const Sim3DPerformanceScope publication = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
      if (!s_world_models.solid_mesh)
        s_world_models.solid_mesh = Sim3DDepthPass_CreateGeometryMesh();
      if (!s_world_models.solid_mesh) {
        s_world_models.solid_mesh_unavailable = true;
        Sim3DPerformance_AddPath(kSim3DPath_Rejected);
      }
      else if (!s_world_models.solid_mesh_published ||
          !Sim3DDepthPass_MeshReady(s_world_models.solid_mesh)) {
        s_world_models.solid_mesh_published = Sim3DDepthPass_UpdateGeometryMesh(
            s_world_models.solid_mesh, s_world_models.projected, s_world_models.projected_count / 4);
        if (!s_world_models.solid_mesh_published) s_world_models.solid_mesh_unavailable = true;
        Sim3DPerformance_AddPath(s_world_models.solid_mesh_published ? kSim3DPath_Publish : kSim3DPath_Rejected);
      }
      Sim3DPerformance_End(publication);
    }
    size_t first = 0;
    for (unsigned i = 0; i <= s_world_models.animated_count; ++i) {
      const size_t end = i < s_world_models.animated_count
          ? s_world_models.animated[i].static_end : s_world_models.projected_count;
      const Sim3DPerformanceScope project = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
      bool ready = true;
      if (first != end) {
        const bool can_reuse = retain && !s_world_models.solid_mesh_unavailable && s_world_models.solid_mesh_published;
        const bool reused = can_reuse && Sim3DDepthPass_AppendGeometryMeshRange(
            kSim3DDepthPass_Solid, s_world_models.solid_mesh, first / 4, (end - first) / 4);
        Sim3DPerformance_AddPath(reused ? kSim3DPath_GpuReuse : kSim3DPath_CpuStage);
        if (can_reuse && !reused) Sim3DPerformance_AddPath(kSim3DPath_Rejected);
        ready = reused || Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Solid,
            s_world_models.projected + first, (end - first) / 4);
      }
      Sim3DPerformance_End(project);
      if (!ready) return false;
      if (i < s_world_models.animated_count) {
        const WorldNavigationAnimatedModel *animated = &s_world_models.animated[i];
        const WorldNavigationVisibleTownObject visible = {
          .object = &towns->objects[animated->object],
          .detail = (SimBackgroundVoxelDetail)animated->detail,
        };
        if (!WorldNavigationAppendAuthoredModel(slot, viewport, projection, &visible, NULL)) return false;
      }
      first = end;
    }
    return true;
  }
  s_world_models.projected_valid = false;
  s_world_models.solid_mesh_published = false;
  s_world_models.projected_count = 0;
  s_world_models.animated_count = 0;
  /* Do not stage an extra copy on every frame of continuous camera motion.
   * A second matching view warms the cache; later held frames replay it. */
  s_world_models.capturing = !gpu_models && same_projection && !s_world_models.projection_unavailable;
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
            viewport, projection,
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
  if (gpu_models) {
    s_world_models.gpu_current_rejected =
        !DrawWorldNavigationGpuModels(slot, projection, visible, (size_t)visible_count);
    if (!s_world_models.gpu_current_rejected) {
      s_world_models.projection_key = key;
      s_world_models.projection_key_ready = true;
      return true;
    }
  }
  WorldNavigationModelBatch *batch = NULL;
  if (visible_count >= 32 && !s_world_model_batch_unavailable && WorldNavigationWorkers()) {
    if (!s_world_model_batch) s_world_model_batch = malloc(sizeof(*s_world_model_batch));
    s_world_model_batch_unavailable = !s_world_model_batch;
    batch = s_world_model_batch;
    if (batch) batch->objects = batch->face_count = 0;
  }
  bool valid = true;
  for (int i = 0; i < visible_count; i++) {
    if (!WorldNavigationAppendAuthoredModel(
            slot, viewport, projection, &visible[i], batch)) valid = false;
  }
  if (!FlushWorldNavigationModels(batch, projection, viewport, slot->sim.world_navigation_lighting)) valid = false;
  s_world_models.projected_valid = valid && s_world_models.capturing;
  s_world_models.capturing = false;
  s_world_models.capture_static = false;
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
  if (s_world_gpu_grid.drawn) return true;
  /* The atmosphere must share every terrain vertex. The former sparse
   * rectangular overlay cut across the curved ocean and left large straight
   * edges, which made the sphere look like a map pasted onto a blue ball. */
  static ArRenderVertex2D vertices[kWorldNavigationTerrainVertexCount];
  static float haze_samples[kWorldNavigationTerrainVertexCount];
  const float lead = fmaxf(1.0f, slot->sim.cull_haze_lead_px * 0.5f);
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++) {
    vertices[i] = s_world_terrain.vertices[i];
    const float haze = SimWorldNavigationScene_LocationHaze(
        &slot->sim.world_navigation_scene,
        vertices[i].tex_coord.x * kSimWorldMapPixels,
        vertices[i].tex_coord.y * kSimWorldMapPixels, lead);
    haze_samples[i] = haze;
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
    const float haze = haze_samples[i];
    vertices[i].color = (ArRenderColorF){
      0.24f, 0.37f, 0.56f,
      s_world_terrain.vertices[i].color.a * haze *
          slot->sim.underlay_haze_pct / (float)kPercentScale * 0.35f,
    };
  }
  return WorldNavigationAppendGroundLayer(kSim3DDepthPass_GroundHaze, vertices, projection, viewport) &&
      WorldNavigationAppendCliffLayer(kSim3DDepthPass_GroundHaze, slot, projection, viewport);
}


typedef struct WorldNavigationCloudCoordinateWork {
  const float (*normals)[3];
  SimWorldNavigationCloudRotation rotation;
  ArRenderPointF *uv;
  float (*direction)[3];
} WorldNavigationCloudCoordinateWork;

/* Only disjoint array math leaves the presentation thread. Cache selection,
 * publication, allocation and all renderer calls stay on its owner. */
static void BuildWorldNavigationCloudCoordinates(void *context, size_t first, size_t end) {
  WorldNavigationCloudCoordinateWork *work = context;
  for (size_t i = first; i < end; ++i) {
    const SimWorldNavigationCloudCoordinate c =
        SimWorldNavigationClouds_Coordinate(work->normals[i], &work->rotation);
    work->uv[i] = (ArRenderPointF){c.u, c.v};
    if (work->direction) {
      work->direction[i][0] = c.x;
      work->direction[i][1] = c.y;
      work->direction[i][2] = c.z;
    }
  }
}

static void PrepareWorldNavigationCloudNormals(const WorldNavigationProjection *projection) {
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
}

static const ArRenderPointF *WorldNavigationCloudUV(
    int bank, WorldNavigationCloudSurface surface,
    const SimWorldNavigationCloudRotation *rotation,
    const WorldNavigationProjection *projection, ArRenderRectI viewport) {
  const bool terrain = surface == kWorldNavigationCloudSurface_Ground;
  const bool ocean = surface == kWorldNavigationCloudSurface_Ocean;
  if (terrain) PrepareWorldNavigationCloudNormals(projection);
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
    WorldNavigationCloudCoordinateWork work = {
      .normals = normal, .rotation = *rotation, .uv = uv,
      .direction = !terrain && !ocean ? cache->body_direction : NULL,
    };
    HostParallelWork_Run(WorldNavigationWorkers(), (size_t)count, 2048,
        BuildWorldNavigationCloudCoordinates, &work);
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
  size_t maximum_receivers = 4 * 1024 * 1024 / sizeof(WorldNavigationShadowReceiver);
#if AR_WORLD_NAV_CACHE_TESTING
  if (s_receiver_test_bytes / sizeof(WorldNavigationShadowReceiver) < maximum_receivers)
    maximum_receivers = s_receiver_test_bytes / sizeof(WorldNavigationShadowReceiver);
#endif
  if (needed > maximum_receivers) return false;
  if (needed > s_world_weather.receiver_capacity) {
    size_t capacity = s_world_weather.receiver_capacity ? s_world_weather.receiver_capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > maximum_receivers) capacity = maximum_receivers;
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
  WorldNavigationReceiverKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection; key.viewport = viewport;
  key.terrain_serial = s_world_terrain.serial; key.cliff_serial = s_world_terrain.cliff_serial;
  const bool same = !memcmp(&key, &s_world_weather.receiver_key, sizeof(key));
  if (same && s_world_weather.receivers_unavailable) return false;
  if (same && s_world_weather.receivers_ready) return true;
  s_world_weather.receiver_key = key;
  s_world_weather.receivers_unavailable = false;
  s_world_weather.receiver_mesh_unavailable = false;
  s_world_weather.spherical_unavailable = false;
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  s_world_weather.receivers_ready = false;
  s_world_weather.receiver_mesh_ready = false;
  s_world_weather.spherical_ready = false;
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
  s_world_weather.receivers_unavailable = true; /* Retry when this view/source changes. */
  s_world_weather.receiver_count = 0;
  return false;
}

static bool PrepareWorldNavigationReceiverMesh(void) {
  if (s_world_weather.receiver_mesh_unavailable || !s_world_weather.receiver_count) return false;
  if (s_world_weather.receiver_mesh_ready &&
      Sim3DDepthPass_MeshReady(s_world_weather.receiver_mesh)) return true;
  if (!s_world_weather.receiver_mesh)
    s_world_weather.receiver_mesh = Sim3DDepthPass_CreateMesh();
  if (!s_world_weather.receiver_mesh) goto unavailable;
  const size_t count = s_world_weather.receiver_count;
  if (count > s_world_weather.receiver_mesh_capacity) {
    /* Receiver storage already has a strict 4 MiB bound. Allocate both
     * parallel arrays before replacing the previous complete publication. */
    const size_t capacity = s_world_weather.receiver_capacity;
    Sim3DDepthPosition *positions = malloc(capacity * 4 * sizeof(*positions));
    ArRenderPointF *uv = malloc(capacity * 4 * sizeof(*uv));
    if (!positions || !uv) { free(positions); free(uv); goto unavailable; }
    free(s_world_weather.receiver_positions); free(s_world_weather.receiver_uv);
    s_world_weather.receiver_positions = positions; s_world_weather.receiver_uv = uv;
    s_world_weather.receiver_mesh_capacity = capacity;
  }
  for (size_t i = 0; i < count; ++i)
    for (int p = 0; p < 4; ++p) {
      const WorldNavigationClipPlan *geometry = &s_world_weather.receivers[i].geometry;
      s_world_weather.receiver_positions[i * 4 + p] = (Sim3DDepthPosition){
        geometry->points[p].x, geometry->points[p].y, geometry->points[p].depth,
      };
    }
  if (!Sim3DDepthPass_UpdateMesh(s_world_weather.receiver_mesh,
          s_world_weather.receiver_positions, count)) goto unavailable;
  s_world_weather.receiver_mesh_ready = true;
  return true;
unavailable:
  s_world_weather.receiver_mesh_unavailable = true;
  return false;
}

static bool PrepareWorldNavigationSphericalMesh(const WorldNavigationProjection *projection) {
  if (s_world_weather.spherical_unavailable || !s_world_weather.receiver_count) return false;
  if (s_world_weather.spherical_ready && Sim3DDepthPass_MeshReady(s_world_weather.spherical_mesh))
    return true;
  if (!s_world_weather.spherical_mesh)
    s_world_weather.spherical_mesh = Sim3DDepthPass_CreateSphericalMesh();
  if (!s_world_weather.spherical_mesh) goto unavailable;
  const size_t count = s_world_weather.receiver_count;
  if (count > s_world_weather.spherical_capacity) {
    const size_t capacity = s_world_weather.receiver_capacity;
    void *quads = realloc(s_world_weather.spherical_quads, capacity * sizeof(*s_world_weather.spherical_quads));
    if (!quads) goto unavailable;
    s_world_weather.spherical_quads = quads;
    s_world_weather.spherical_capacity = capacity;
  }
  PrepareWorldNavigationCloudNormals(projection);
  for (size_t i = 0; i < count; ++i) {
    const WorldNavigationShadowReceiver *receiver = &s_world_weather.receivers[i];
    Sim3DDepthSphericalQuad *quad = &s_world_weather.spherical_quads[i];
    quad->triangle = receiver->geometry.triangle;
    for (int p = 0; p < 4; ++p) {
      quad->positions[p] = (Sim3DDepthPosition){receiver->geometry.points[p].x,
        receiver->geometry.points[p].y, receiver->geometry.points[p].depth};
      memcpy(quad->weights[p], receiver->geometry.points[p].weights, sizeof(quad->weights[p]));
      const float *normal = receiver->surface == kWorldNavigationReceiver_Cliff
          ? s_world_terrain.cliff_projection[receiver->cliff].normal[p]
          : (receiver->surface == kWorldNavigationReceiver_Ocean
              ? s_world_shells.ocean.normal : s_world_weather.ground_normals)[receiver->corners[p]];
      memcpy(quad->normals[p], normal, sizeof(quad->normals[p]));
    }
  }
  if (!Sim3DDepthPass_UpdateSphericalMesh(s_world_weather.spherical_mesh,
          s_world_weather.spherical_quads, count)) goto unavailable;
  Sim3DPerformance_AddPath(kSim3DPath_Publish);
  s_world_weather.spherical_ready = true;
  return true;
unavailable:
  s_world_weather.spherical_unavailable = true;
  return false;
}

static bool AppendWorldNavigationReceivers(int bank,
    const SimWorldNavigationCloudRotation *rotation,
    const WorldNavigationProjection *projection, ArRenderRectI viewport,
    float offset_u, float offset_v, ArRenderColorF color) {
  if (!s_world_weather.receiver_count) return true;
  if (PrepareWorldNavigationSphericalMesh(projection)) {
    const Sim3DDepthSphericalSample sample = {
      .rotation = {rotation->cos_u, rotation->sin_u, rotation->cos_v, rotation->sin_v},
      .offset = {offset_u, offset_v},
      .atlas = {0, bank * kSimWorldNavigationCloudHeight,
        kSimWorldNavigationCloudWidth, kSimWorldNavigationCloudHeight},
      .texture_size = {kSimWorldNavigationCloudWidth * 2,
        kSimWorldNavigationCloudHeight * kSimCloudLayerCount},
      .color = color,
    };
    const bool ok = Sim3DDepthPass_AppendSphericalSample(kSim3DDepthPass_CloudShadow,
        s_world_weather.spherical_mesh, &sample);
    Sim3DPerformance_AddPath(ok ? kSim3DPath_GpuReuse : kSim3DPath_Rejected);
    return ok;
  }
  if (s_world_weather.spherical_unavailable) Sim3DPerformance_AddPath(kSim3DPath_Rejected);
  Sim3DPerformance_AddPath(kSim3DPath_CpuStage);
  const ArRenderPointF *ground = WorldNavigationCloudUV(bank, kWorldNavigationCloudSurface_Ground,
      rotation, projection, viewport);
  const ArRenderPointF *ocean = WorldNavigationCloudUV(bank, kWorldNavigationCloudSurface_Ocean,
      rotation, projection, viewport);
  WorldNavigationPrepareCliffCloudUV(bank, rotation);
  const bool retained = PrepareWorldNavigationReceiverMesh();
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
    if (retained) {
      WorldNavigationApplyShadowUV(&receiver->geometry, uv, s_world_weather.receiver_uv + i * 4);
      continue;
    }
    WorldNavigationApplyShadowPlan(&receiver->geometry, uv, color, vertices + count * 4);
    if (++count == kBatch) {
      if (!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_CloudShadow, vertices, count)) return false;
      count = 0;
    }
  }
  if (retained)
    return Sim3DDepthPass_AppendMeshSample(kSim3DDepthPass_CloudShadow,
        s_world_weather.receiver_mesh, s_world_weather.receiver_uv, s_world_weather.receiver_count, color);
  return !count || Sim3DDepthPass_AppendQuads(kSim3DDepthPass_CloudShadow, vertices, count);
}

static bool DrawWorldNavigationCloudBody(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const SimCloudLayer *layer, uint64_t elapsed_ms, float drift,
    ArRenderColorF colour) {
  const int bank = (int)(layer - kSimCloudLayers);
  if (bank < 0 || bank >= kSimCloudLayerCount) return false;
  const float phase_u = layer->offset_x +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_x, drift);
  const float phase_v = layer->offset_y +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_y, drift);
  const SimWorldNavigationCloudRotation rotation =
      SimWorldNavigationClouds_Rotation(phase_u, phase_v);
  return AppendWorldNavigationCloudGrid(bank, kWorldNavigationCloudSurface_Body,
      viewport, projection, &rotation, 0, 0, colour);
}


static bool DrawWorldNavigationGpuCloudBodies(const WorldNavigationProjection *projection,
    uint64_t elapsed_ms, float drift, float opacity) {
  if (!s_world_gpu_grid.drawn || s_world_weather.body_unavailable) return false;
  const float reference = projection->reference_height_units*projection->height_world_per_unit;
  const float radius = projection->globe_radius_world+reference+projection->cloud_height_world;
  const float centre_z = -projection->globe_radius_world-reference;
  float outward[3], right[3], up[3], distance;
  if (!WorldNavigationShellFrame(projection,radius,centre_z,outward,right,up,&distance)) return false;
  if (!s_world_weather.body_mesh)
    s_world_weather.body_mesh = Sim3DDepthPass_CreateSphericalBodyMesh();
  if (!s_world_weather.body_mesh) goto unavailable;
  if (!Sim3DDepthPass_MeshReady(s_world_weather.body_mesh) ||
      s_world_weather.body_radius != radius || s_world_weather.body_distance != distance) {
    /* Cap shape and limb opacity depend only on radius/eye distance, not the
     * globe orientation, wind, viewport, or bank. Keep the original 48x96
     * topology. Only shape changes republish this one shared compact stream. */
    PrepareWorldNavigationOceanIndices();
    Sim3DDepthSphericalBodyVertex points[kWorldNavigationOceanVertexCount];
    const float angle = acosf(radius/distance);
    for (int ring = 0; ring <= kWorldNavigationOceanRings; ++ring) {
      const float a = (ring/(float)kWorldNavigationOceanRings)*angle;
      const float sine = sinf(a), cosine = cosf(a);
      const float ray = hypotf(radius*sine,distance-radius*cosine);
      const float alpha = SimWorldNavigationScene_CloudLimbOpacity((distance*cosine-radius)/ray);
      for (int x = 0; x < (ring ? kWorldNavigationOceanSectors : 1); ++x) {
        const int at = ring ? 1+(ring-1)*kWorldNavigationOceanSectors+x : 0;
        points[at] = (Sim3DDepthSphericalBodyVertex){
          {sine*s_world_shells.longitude_cos[x],sine*s_world_shells.longitude_sin[x],cosine},alpha};
      }
    }
    const size_t count = kWorldNavigationOceanRings*kWorldNavigationOceanSectors;
    if (!s_world_weather.body_vertices)
      s_world_weather.body_vertices = malloc(count*4*sizeof(*s_world_weather.body_vertices));
    Sim3DDepthSphericalBodyVertex *vertices = s_world_weather.body_vertices;
    if (!vertices) goto unavailable;
    size_t used = 0;
    for (int y = 0; y < kWorldNavigationOceanRings; ++y)
      for (int x = 0; x < kWorldNavigationOceanSectors; ++x) {
        const int next = (x+1)%kWorldNavigationOceanSectors;
        const int at[4] = {y ? 1+(y-1)*kWorldNavigationOceanSectors+x : 0,
          y ? 1+(y-1)*kWorldNavigationOceanSectors+next : 0,
          1+y*kWorldNavigationOceanSectors+next,1+y*kWorldNavigationOceanSectors+x};
        for (unsigned p = 0; p < 4; ++p) vertices[used++] = points[at[p]];
      }
    const bool ok = Sim3DDepthPass_UpdateSphericalBodyMesh(s_world_weather.body_mesh,vertices,count);
    if (!ok) goto unavailable;
    s_world_weather.body_radius = radius; s_world_weather.body_distance = distance;
  }
  Sim3DDepthSphericalBodyTransform transform = {.centre = {0,0,centre_z}, .radius = radius};
  memcpy(transform.matrix,projection->matrix,sizeof(transform.matrix));
  for (unsigned r = 0; r < 3; ++r) {
    transform.basis[r][0] = right[r]; transform.basis[r][1] = up[r];
    transform.basis[r][2] = outward[r];
  }
  for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c)
    transform.texture_basis[r][c] = projection->globe_frame.right[r]*transform.basis[0][c] +
      projection->globe_frame.up[r]*transform.basis[1][c] +
      projection->globe_frame.outward[r]*transform.basis[2][c];
  Sim3DDepthSphericalSample samples[kSimCloudLayerCount];
  for (unsigned bank = 0; bank < kSimCloudLayerCount; ++bank) {
    const SimCloudLayer *layer = &kSimCloudLayers[bank];
    const SimWorldNavigationCloudRotation r = SimWorldNavigationClouds_Rotation(
        layer->offset_x+Scene3D_WrappedTextureOffset(elapsed_ms,layer->drift_x,drift),
        layer->offset_y+Scene3D_WrappedTextureOffset(elapsed_ms,layer->drift_y,drift));
    samples[bank] = (Sim3DDepthSphericalSample){.rotation = {r.cos_u,r.sin_u,r.cos_v,r.sin_v},
      .atlas = {0,bank*kSimWorldNavigationCloudHeight,kSimWorldNavigationCloudWidth,kSimWorldNavigationCloudHeight},
      .texture_size = {kSimWorldNavigationCloudWidth*2,kSimWorldNavigationCloudHeight*kSimCloudLayerCount},
      .color = {1,1,1,opacity*layer->weight}};
  }
  if (Sim3DDepthPass_AppendSphericalBodies(s_world_weather.body_mesh,&transform,samples,kSimCloudLayerCount))
    return true;
unavailable:
  s_world_weather.body_unavailable = true;
  Sim3DPerformance_AddPath(kSim3DPath_Rejected);
  fprintf(stderr,"[world-navigation] GPU cloud body unavailable; using compatible clouds\n");
  return false; /* The group queues nothing on rejection. */
}

static PresentationOutcome OmitWorldNavigationWeather(const char *reason) {
  if (!s_world_weather.failure_reported) {
    s_world_weather.failure_reported = true;
    fprintf(stderr, "[world-navigation] optional weather omitted: %s\n",
            reason && reason[0] ? reason : "renderer rejected the effect");
  }
  return kPresentationOutcome_OptionalOmitted;
}

/* Immutable per-presentation shadow parameters shared by the GPU source grid
 * and the remaining CPU-projected ocean/cliff receivers. */
static size_t WorldNavigationShadowSamples(const FrameSlot *slot, uint64_t elapsed_ms,
    Sim3DDepthSphericalSample samples[kSimCloudLayerCount * 3]) {
  if (!slot->sim.world_navigation_clouds || !slot->sim.cloud_opacity_pct ||
      !slot->sim.world_navigation_cloud_shadows || !slot->sim.world_navigation_lighting ||
      !slot->sim.shadow_opacity_pct) return 0;
  float light_x, light_y; SimShadowLight(slot, &light_x, &light_y);
  const float shadow_x = light_x * slot->sim.cloud_altitude_px / 8.0f;
  const float shadow_y = light_y * slot->sim.cloud_altitude_px / 8.0f;
  const float blur = slot->sim.shadow_softness_pct * .08f;
  const int count = blur > .01f ? 3 : 1;
  const float px = -sinf(slot->sim.light_azimuth_deg * kPi/180), py = cosf(slot->sim.light_azimuth_deg * kPi/180);
  const float opacity = slot->sim.cloud_opacity_pct / (float)kPercentScale;
  const float drift = slot->sim.cloud_drift_pct / (float)kPercentScale;
  size_t used = 0;
  for (int bank = 0; bank < kSimCloudLayerCount; ++bank) {
    const SimCloudLayer *layer = &kSimCloudLayers[bank];
    const SimWorldNavigationCloudRotation r = SimWorldNavigationClouds_Rotation(
        layer->offset_x + Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_x, drift),
        layer->offset_y + Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_y, drift));
    for (int i = 0; i < count; ++i) {
      const float spread = count == 1 ? 0 : (i-1)*blur;
      const float weight = count == 1 ? 1 : i == 1 ? .5f : .25f;
      samples[used++] = (Sim3DDepthSphericalSample){.rotation = {r.cos_u,r.sin_u,r.cos_v,r.sin_v},
        .offset = {(shadow_x+px*spread)/kSimWorldMapPixels, (shadow_y+py*spread)/kSimWorldMapPixels},
        .atlas = {0,bank*kSimWorldNavigationCloudHeight,kSimWorldNavigationCloudWidth,kSimWorldNavigationCloudHeight},
        .texture_size = {kSimWorldNavigationCloudWidth*2,kSimWorldNavigationCloudHeight*kSimCloudLayerCount},
        .color = {0,0,0,opacity*layer->weight*(slot->sim.shadow_opacity_pct/(float)kPercentScale)*.35f*weight}};
    }
  }
  return used;
}

static PresentationOutcome DrawWorldNavigationWeather(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms) {
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

  /* A cloud's altitude is invisible to an orthographic top-down camera until
   * it casts a displaced shadow. Reuse the town light's world-space shear so
   * the shadow rotates and zooms with the scripted Mode-7 event. The
   * procedural alpha already supplies a soft edge; the softness dial spreads
   * three low-alpha samples across the light-perpendicular axis. */
  Sim3DDepthSphericalSample shadows[kSimCloudLayerCount * 3];
  const size_t shadow_count = s_world_gpu_grid.drawn ? 0 : WorldNavigationShadowSamples(slot, elapsed_ms, shadows);
  const bool receivers = shadow_count && PrepareWorldNavigationReceivers(projection, viewport);
  for (size_t i = 0; i < shadow_count; ++i) {
    const Sim3DDepthSphericalSample *sample = &shadows[i];
    const int bank = (int)sample->atlas.y / kSimWorldNavigationCloudHeight;
    const SimWorldNavigationCloudRotation rotation = {
      sample->rotation[0], sample->rotation[1], sample->rotation[2], sample->rotation[3]};
    const bool drawn = receivers
        ? AppendWorldNavigationReceivers(bank, &rotation, projection, viewport,
            sample->offset.x, sample->offset.y, sample->color)
        /* This loop only serves the complete compatibility surface group. */
        : AppendWorldNavigationCloudGrid(bank, kWorldNavigationCloudSurface_Ground,
              viewport, projection, &rotation, sample->offset.x, sample->offset.y, sample->color) &&
          AppendWorldNavigationCloudGrid(bank, kWorldNavigationCloudSurface_Ocean,
              viewport, projection, &rotation, sample->offset.x, sample->offset.y, sample->color);
    if (!drawn) return OmitWorldNavigationWeather(ArRenderDevice_LastError(&g_render_device));
  }

  if (slot->sim.view == kSimView_SkyPalace &&
      !PresentWorldNavSky_DrawClouds(&g_render_device, slot, viewport, projection, elapsed_ms, drift, opacity))
    return OmitWorldNavigationWeather(ArRenderDevice_LastError(&g_render_device));

  if (body_visibility > 0.001f) {
    if (DrawWorldNavigationGpuCloudBodies(projection,elapsed_ms,drift,opacity*body_visibility))
      return kPresentationOutcome_Complete;
    if (!DrawWorldNavigationSphereShell(
            viewport, projection, kWorldNavigationShell_Cloud))
      return OmitWorldNavigationWeather("cloud shell projection");
    for (unsigned layer_index = 0;
         layer_index < (size_t)kSimCloudLayerCount;
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      if (!DrawWorldNavigationCloudBody(
              viewport, projection, layer, elapsed_ms, drift,
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
    ArRenderTexture texture, ArRenderPointF offset, float scale) {
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
  if (scale != 1.0f) {
    /* Scale about the native travel focus, including the authored offset of
     * the cloud/platform, not about a potentially asymmetric raster crop. */
    destination.x = viewport.w * .5f + (top_left.x - viewport.w * .5f) * scale + offset.x;
    destination.y = viewport.h * .5f + (top_left.y - viewport.h * .5f) * scale + offset.y;
    destination.w *= scale;
    destination.h *= scale;
  }
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
    if (!WorldNavigationSurfaceNormal(projection,
            slot->sim.world_navigation.focus_x, slot->sim.world_navigation.focus_y, normal)) return false;
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
  return DrawWorldNavigationCompositionLayer(slot, viewport,
      &slot->sim.world_navigation_scene.composition.palace,
      s_world_composition.palace, offset, scale);
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

static bool EnsureWorldNavigationResourcesAtRadius(const FrameSlot *slot, float radius_tiles) {
  Sim3DPerformanceScope art_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Upload);
  const bool setup_timing = Sim3DPerformance_Enabled() &&
      ((!s_world_mountains.ready && WorldNavigationMountainsEnabled(slot)) ||
       !s_world_terrain.cliffs_ready ||
       !s_world_art.serial);
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
  if (s_world_art.unavailable) EnsureWorldNavigationCliffs(slot,radius_tiles);
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
      !PresentWorldNavSky_DrawMist(&g_render_device, viewport, PresentWorldNavSky_Horizon(viewport, projection)))
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

/* Connected SIM background. Keep the shared terrain/art/model GPU resource
 * owner here: making another renderer would duplicate both uploads and cache
 * invalidation policy. The separate private entry contract receives the SIM
 * matrix and never invokes the navigation camera, scene or UI compositor.
 * Live SIM retains all actor/priority/depth ownership above this background. */
void PresentSimGlobe_ClampCamera(Scene3DCamera *camera) {
  if (!camera) return;
  camera->distance = fminf(camera->distance,
      (float)kSim3DConnectedCameraDistanceMaximumX100 / kPercentScale);
  /* Keep the authored SIM facade's full low-angle range. The connected
   * background bounds travel/side rotation, not how far we can look across
   * the active town. Use the same pitch limit as the other SIM layers. */
  camera->tilt_x = fmaxf(camera->tilt_x,
      (float)kSim3DCameraPitchMinimumMrad / (float)kPermilleScale);
  const float maximum_yaw = (float)kSim3DConnectedCameraYawMaximumMrad / kPermilleScale;
  camera->tilt_y = fmaxf(-maximum_yaw, fminf(maximum_yaw, camera->tilt_y));
}

static bool SimGlobeNearby(const SimGlobeMapping *map, float x, float y) {
  return x >= map->origin_x-24 && x <= map->origin_x+56 &&
      y >= map->origin_y-24 && y <= map->origin_y+56;
}

static bool SimGlobeEmbedVertex(const SimGlobeMapping *map, float x, float y,
    float extra_scale, Sim3DDepthSurfaceVertex *v) {
  float shade[3];
  SimWorldNavigationGlobe_TransformNormal(&map->frame, v->shade_normal, shade);
  memcpy(v->shade_normal, shade, sizeof(shade));
  return SimGlobeMapping_Encode(map, x, y, v->elevation[0],
      v->elevation[1]*extra_scale, v->normal, v->elevation);
}

static bool SimGlobeInsideTown(const SimGlobeMapping *map, float x, float y) {
  return x > map->origin_x && x < map->origin_x+32 &&
      y > map->origin_y && y < map->origin_y+32;
}

static bool SimGlobeKeepCell(const SimGlobeMapping *map, int x, int y, bool detailed_town) {
  if (detailed_town && SimGlobeInsideTown(map,x+.5f,y+.5f)) return false;
  return !s_world_terrain.cliffs.replacement[y*kWorldNavigationTerrainCells+x];
}

static bool SimGlobeKeepFace(const SimGlobeMapping *map,
    const float x[4], const float y[4], bool mountain, bool detailed_town) {
  float cx = 0, cy = 0;
  for (int p = 0; p < 4; ++p) { cx += x[p]*.25f; cy += y[p]*.25f; }
  if (detailed_town && !mountain && SimGlobeInsideTown(map,cx,cy)) return false;
  return !mountain || SimGlobeNearby(map,cx,cy);
}

static bool SimGlobeBuildSurface(const FrameSlot *slot,
    const WorldNavigationProjection *projection, const SimGlobeMapping *map,
    bool detailed_town) {
  const uint32_t geography = SimWorldMap_GeographySerial();
  const bool water_ready = !detailed_town ||
      PresentSimGlobeWater_Matches(map,&slot->sim.world_navigation_towns.ground);
  if (s_sim_globe.ready && Sim3DMeshSet_Ready(&s_sim_globe.surface) &&
      !memcmp(map,&s_sim_globe.map,sizeof(*map)) &&
      s_sim_globe.geography == geography && s_sim_globe.detailed_town == detailed_town &&
      s_sim_globe.mountains == s_world_mountains.geometry_revision &&
      s_sim_globe.cliffs == s_world_terrain.cliff_serial &&
      s_sim_globe.height_percent == slot->sim.height_scale_x100 && water_ready) return true;
  if (!PrepareWorldNavigationGroundSamples(projection)) return false;
  const size_t cliffs = s_world_terrain.cliffs.face_count;
  const size_t mountains = s_world_mountains.active ? s_world_mountains.scene.face_count : 0;
  /* Count only published geometry. Active detailed surfaces replace their
   * overview counterparts; distant mountains consume no SIM storage. */
  size_t capacity = kWorldNavigationOceanQuads;
  for (int y = 0; y < kWorldNavigationTerrainCells; ++y)
    for (int x = 0; x < kWorldNavigationTerrainCells; ++x)
      capacity += SimGlobeKeepCell(map,x,y,detailed_town);
  for (size_t i = 0; i < cliffs; ++i) {
    const SimWorldNavigationCliffFace *f = &s_world_terrain.cliffs.faces[i];
    capacity += SimGlobeKeepFace(map,f->x,f->y,false,detailed_town);
  }
  for (size_t i = 0; i < mountains; ++i) {
    const SimWorldNavigationMountainFace *f = &s_world_mountains.scene.faces[i];
    capacity += !(detailed_town && f->town == map->town) &&
        SimGlobeKeepFace(map,f->x,f->y,true,detailed_town);
  }
  if (capacity > kWorldNavigationSurfaceMaximumQuads) return false;
  Sim3DDepthSurfaceVertex *vertices = malloc(capacity*4*sizeof(*vertices));
  Sim3DDepthSurfaceVertex *points = malloc(kWorldNavigationTerrainVertexCount*sizeof(*points));
  ArRenderPointF *mask = malloc(capacity*4*sizeof(*mask));
  if (!vertices || !points || !mask) { free(vertices); free(points); free(mask); return false; }
  bool ok = BuildWorldNavigationOceanSource(vertices,mask);
  /* Camera-local ocean is beneath the chart; only its remote part is visible.
   * Everything chart-owned gets explicit geographic mask UVs, independent of
   * native/mountain texture packing. No color rebuild when focus changes. */
  for (size_t i=0;i<kWorldNavigationOceanQuads*4;++i) mask[i]=(ArRenderPointF){-2,-2};
  const float ratio = map->landscape/map->chart_radius;
  WorldNavigationGridSourceWork work = {s_world_terrain.samples, points, ratio};
  HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainVertexCount, 2048,
      BuildWorldNavigationGridSourceRange, &work);
  for (int y = 0; ok && y < kWorldNavigationTerrainAxis; ++y)
    for (int x = 0; ok && x < kWorldNavigationTerrainAxis; ++x)
      ok = SimGlobeEmbedVertex(map,x,y,0,&points[WorldNavigationTerrainVertexIndex(x,y)]);
  size_t count = kWorldNavigationOceanQuads;
  for (int y = 0; ok && y < kWorldNavigationTerrainCells; ++y)
    for (int x = 0; x < kWorldNavigationTerrainCells; ++x) {
      /* Detailed active tops replace the overview cells without a second
       * backing canvas or flattened footprint. */
      if (!SimGlobeKeepCell(map,x,y,detailed_town)) continue;
      const int at = WorldNavigationTerrainVertexIndex(x,y);
      const int corners[4] = {at,at+1,at+kWorldNavigationTerrainAxis+1,at+kWorldNavigationTerrainAxis};
      for (int p = 0; p < 4; ++p) {
        vertices[count*4+p] = points[corners[p]];
        mask[count*4+p]=(ArRenderPointF){(x+(p==1 || p==2))/128.0f,(y+(p>=2))/128.0f};
      }
      ++count;
    }
  for (size_t i = 0; ok && i < cliffs; ++i) {
    const SimWorldNavigationCliffFace *f = &s_world_terrain.cliffs.faces[i];
    if (!SimGlobeKeepFace(map,f->x,f->y,false,detailed_town)) continue;
    for (int p = 0; p < 4; ++p) {
      Sim3DDepthSurfaceVertex *v = &vertices[count*4+p];
      *v = (Sim3DDepthSurfaceVertex){.uv = {f->u[p],f->v[p]},
          .color = {f->shade,f->shade,f->shade,1},.elevation = {f->height[p],0}};
      ok &= SimGlobeEmbedVertex(map,f->x[p],f->y[p],0,v);
      mask[count*4+p]=(ArRenderPointF){f->x[p]/128,f->y[p]/128};
    }
    ++count;
  }
  const size_t mountain_first = count;
  for (size_t i = 0; ok && i < mountains; ++i) {
    const SimWorldNavigationMountainFace *f = &s_world_mountains.scene.faces[i];
    if ((detailed_town && f->town == map->town) ||
        !SimGlobeKeepFace(map,f->x,f->y,true,detailed_town)) continue;
    for (int p = 0; p < 4; ++p) {
      Sim3DDepthSurfaceVertex *v = &vertices[count*4+p];
      const float shade = f->brightness[p]/255.0f;
      *v = (Sim3DDepthSurfaceVertex){.uv = {f->uv[p].x,f->uv[p].y},
          .color = {shade,shade,shade,1}};
      float normal[3], metric;
      ok &= SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius,f->x[p],f->y[p],normal,&metric);
      v->elevation[0] = WorldNavigationTerrainHeightAtPrepared(
          f->x[p]*kSimWorldMapTilePixels,f->y[p]*kSimWorldMapTilePixels,NULL,true);
      v->elevation[1] = f->z[p]*metric;
      ok &= SimGlobeEmbedVertex(map,f->x[p],f->y[p],
          slot->sim.height_scale_x100/(100.0f*map->metric),v);
      mask[count*4+p]=(ArRenderPointF){f->x[p]/128,f->y[p]/128};
    }
    ++count;
  }
  ok = ok && count == capacity && Sim3DMeshSet_UpdateSurface(&s_sim_globe.surface,vertices,mask,count);
  if (ok && detailed_town)
    ok = PresentSimGlobeWater_Prepare(map,&slot->sim.world_navigation_towns.ground,points);
  free(vertices); free(points); free(mask);
  if (!ok) {
    fprintf(stderr,"[sim-globe-underlay] surface publication rejected quads=%zu\n",count);
    return false;
  }
  s_sim_globe.map = *map; s_sim_globe.geography = geography;
  s_sim_globe.mountains = s_world_mountains.geometry_revision;
  s_sim_globe.cliffs = s_world_terrain.cliff_serial;
  s_sim_globe.height_percent = slot->sim.height_scale_x100;
  s_sim_globe.quads = count; s_sim_globe.mountain_first = mountain_first;
  s_sim_globe.ready = true; s_sim_globe.detailed_town = detailed_town;
  if (Sim3DPerformance_Enabled())
    fprintf(stderr,"[sim-globe-town] source town=%u quads=%zu mountains=%zu\n",
        slot->sim.town,count,count-mountain_first);
  return true;
}

#if AR_SIM_GLOBE_TESTING
bool PresentSimGlobe_TestSurfaceSource(const FrameSlot *slot,
    SimWorldNavigationMountainFace *faces, size_t count,
    size_t *published_mountains, size_t *chunks) {
  const SimWorldNavigationMountainScene saved = s_world_mountains.scene;
  const bool active = s_world_mountains.active;
  const uint32_t revision = s_world_mountains.geometry_revision;
  const SimGlobeMapping map = s_sim_globe.map;
  const WorldNavigationProjection projection = {.chart_radius_tiles = map.chart_radius,
    .height_world_per_unit = map.landscape > 0 ? 1 : 0};
  s_world_mountains.scene.faces = faces; s_world_mountains.scene.face_count = count;
  s_world_mountains.active = true; ++s_world_mountains.geometry_revision;
  s_sim_globe.ready = false;
  const bool ok = SimGlobeBuildSurface(slot,&projection,&map,false);
  *published_mountains = ok ? s_sim_globe.quads - s_sim_globe.mountain_first : 0;
  *chunks = s_sim_globe.surface.count;
  s_world_mountains.scene = saved; s_world_mountains.active = active;
  s_world_mountains.geometry_revision = revision;
  Sim3DMeshSet_Destroy(&s_sim_globe.surface); s_sim_globe.ready = false;
  return ok;
}
#endif

static PresentationOutcome DrawSimGlobeImage(
    ArRenderRectI viewport, ArRenderTexture composite) {
  const ArRenderRectF destination = {viewport.x,viewport.y,viewport.w,viewport.h};
  /* Focus is applied spatially to neighbours, never to the complete town. */
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend | kArRenderDrawState_Tint,
    .blend = kArRenderBlendMode_AlphaPremultiplied,
    .tint = {1,1,1,1},
  };
  return ArRenderDevice_DrawTextureWithState(&g_render_device,composite,NULL,&destination,&state)
      ? kPresentationOutcome_Complete : kPresentationOutcome_CoreFailure;
}

static float SimGlobeMountainGround(float x, float y) {
  return WorldNavigationTerrainHeightAtPrepared(
      x*kSimWorldMapTilePixels,y*kSimWorldMapTilePixels,NULL,true);
}

static PresentationOutcome DrawSimGlobeScene(const FrameSlot *slot, ArRenderRectI source,
    ArRenderRectI viewport, const Scene3DCamera *camera, const float matrix[16],
    float radius_scale, bool sim_facades, bool detailed_town,
    const PresentSimGlobeContent *content,
    PresentSimGlobeView *out_view) {
  if (!slot || !camera || !matrix || source.w <= 0 || source.h <= 0 ||
      viewport.w <= 0 || viewport.h <= 0 || !isfinite(radius_scale) ||
      radius_scale < 1 || radius_scale > 4 ||
      (unsigned)slot->sim.background_voxel_detail >= kSimBackgroundVoxelDetail_Count ||
      (sim_facades && (
       (unsigned)slot->sim.background_voxel_facing >= kSimBackgroundVoxelFacing_Count ||
       (unsigned)slot->sim.background_voxel_shading >= kSimBackgroundVoxelShading_Count)))
    return kPresentationOutcome_CoreFailure;
  const float chart_radius = WorldNavigationChartRadius(slot)*radius_scale;
  if (!EnsureWorldNavigationResourcesAtRadius(slot,chart_radius))
    return kPresentationOutcome_CoreFailure;
  /* SIM owns its active landscape and model shading. Navigation toggles may
   * reduce the neighbouring overview, but cannot flatten or unlight the town
   * the player is editing. Do not copy/mutate a large captured FrameSlot to
   * manufacture settings for this consumer. */
  const bool relief = AR_SIM3D_TERRAIN_ELEVATION;
  if (relief && slot->sim.landscape_height_pct) PrepareWorldNavigationTerrain();
  /* The private GPU model cache now holds embedded sources. A later return
   * to navigation must not repeat its old projection key over these meshes. */
  s_world_models.gpu_current_ready = false;
  s_world_models.gpu_current_rejected = false;
  s_world_models.projection_key_ready = false;
  const float ox = slot->sim.underlay_origin_tile_x, oy = slot->sim.underlay_origin_tile_y;
  float reference = 0;
  if (relief && !SimWorldNavigationTerrain_RegisterTownFloor(
          slot->sim.town,16,16,SimTownTerrain_HeightUnitsAt(slot->sim.town,256,256),&reference))
    return kPresentationOutcome_CoreFailure;
  SimGlobeMapping map;
  if (!SimGlobeMapping_Build(slot->sim.town, ox, oy, chart_radius,reference,
          relief ? slot->sim.landscape_height_pct/100.0f : 0, &map))
    return kPresentationOutcome_CoreFailure;
  /* Globe relief is independently optional. Its shoreline still meets the
   * active town's actual elevation, including builds with flat SIM terrain. */
#if AR_SIM3D_TERRAIN_ELEVATION
  map.town_landscape = slot->sim.landscape_height_pct / (float)kPercentScale;
#else
  map.town_landscape = 0;
#endif
  /* Keep the centre's audited SIM elevation at the same camera-space datum.
   * Curvature is the variable, not a hidden vertical reframe of the town. */
  if (map.landscape > 0)
    map.reference_height -= SimTownTerrain_HeightUnitsAt(map.town,256,256) *
        map.town_landscape * map.metric / map.landscape;
  WorldNavigationProjection projection = {.chart_radius_tiles = map.chart_radius,
      .globe_radius_world = map.radius,.height_world_per_unit = map.landscape > 0 ? 1 : 0,.clip_frustum = true,
      .tile_world = 1/map.metric,
      .globe_frame = {.right = {1,0,0},.up = {0,1,0},.outward = {0,0,1}}};
  const float aspect = (float)viewport.w/viewport.h;
  const float scale[3] = {16*aspect/source.w,16.0f/source.h,16.0f/source.h};
  const float offset[3] = {
      ((slot->sim.underlay_screen_x0-slot->sim.camera_x+256.0f-source.x)/source.w-.5f)*aspect,
      .5f-(256.0f-slot->sim.camera_y-source.y)/source.h,0};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col) projection.matrix[col*4+row] = matrix[col*4+row]*scale[col];
    projection.matrix[12+row] = matrix[12+row];
    for (int col = 0; col < 3; ++col) projection.matrix[12+row] += matrix[col*4+row]*offset[col];
  }
  for (int i = 0; i < 3; ++i)
    projection.camera_world[i] = (-camera->distance*matrix[i*4+3]-offset[i])/scale[i];
  PresentSimGlobeView view = {.map = map};
  memcpy(view.matrix,projection.matrix,sizeof(view.matrix));
  memcpy(view.camera,projection.camera_world,sizeof(view.camera));
  if (out_view) *out_view = view;
  if (detailed_town) {
    SimBackgroundVoxelRenderParams params = SimVoxelRenderParams(slot,source,viewport,matrix);
    SimBackgroundVoxelProject_Prepare(&params);
    if (!PresentSimGlobeMountains_Prepare(&params,&map,SimGlobeMountainGround,
            SimWorldMap_GeographySerial())) {
      fprintf(stderr,"[sim-globe-town] native mountain preparation rejected\n");
      goto failed;
    }
    if (!PresentSimGlobeTerrain_Prepare(&map,SimWorldMap_GeographySerial())) {
      fprintf(stderr,"[sim-globe-town] native terrain preparation rejected\n");
      goto failed;
    }
  }
  PresentSimGlobeGroundShadow town_shadow = {0};
  if (content && content->prepare &&
      !content->prepare(content->userdata,&view,&town_shadow)) {
    fprintf(stderr,"[sim-globe-town] dynamic content preparation rejected\n");
    goto failed;
  }
  if (!Sim3DDepthPass_Begin(&g_render_device,viewport.w,viewport.h,kArRenderFilter_Linear)) goto failed;
  if (!SimGlobeBuildSurface(slot,&projection,&map,detailed_town)) goto failed;
  bool ok = s_world_art.displayed_version < 0 || Sim3DDepthPass_SelectAtlasVersion(
      s_world_art.atlas_cache,(unsigned)s_world_art.displayed_version);
  Sim3DDepthSurfaceTransform t = {.radial = {.sphere_radius = map.radius,.height_scale = 1},
      .ambient = .76f,
      .diffuse = .24f};
  t.focus=PresentSimGlobeFocus_Resolve(&map,
      (slot->sim.effective_features & kSimFeature_CullHaze)!=0,
      slot->sim.cull_dim_pct,slot->sim.underlay_haze_pct,slot->sim.cull_haze_lead_px);
  memcpy(t.radial.matrix,projection.matrix,sizeof(t.radial.matrix));
  for (int i = 0; i < 3; ++i) t.radial.basis[i][i] = 1;
  const float azimuth = slot->sim.light_azimuth_deg*kPi/180, elevation = slot->sim.light_elevation_deg*kPi/180;
  t.light[0] = -cosf(azimuth)*cosf(elevation); t.light[1] = -sinf(azimuth)*cosf(elevation); t.light[2] = sinf(elevation);
  Sim3DDepthSurfaceBatch batches[3] = {
    {.layer = kSim3DDepthPass_Ground,.range = {0,kWorldNavigationOceanQuads}},
    {.layer = kSim3DDepthPass_Ground,.range = {kWorldNavigationOceanQuads,s_sim_globe.mountain_first-kWorldNavigationOceanQuads},.transform = t},
    {.layer = kSim3DDepthPass_WorldMountain,.range = {s_sim_globe.mountain_first,s_sim_globe.quads-s_sim_globe.mountain_first},.transform = t}};
  batches[2].transform.ambient = .90f; batches[2].transform.diffuse = 0;
  WorldNavigationProjection ocean = projection;
  ocean.reference_height_units = map.reference_height*map.landscape/map.metric;
  ok = ok && WorldNavigationOceanTransform(&ocean,&batches[0].transform);
  batches[0].transform.focus=t.focus;
  ok = ok && Sim3DMeshSet_AppendSurface(&s_sim_globe.surface,batches,3);
  if (!ok) fprintf(stderr,"[sim-globe-underlay] surface batch rejected\n");
  if (ok && detailed_town) {
    /* Geographic focus marks the selected town; this separate ground cue
     * follows its smaller live sprite window. Both are draw-time materials,
     * never reasons to rebuild resident terrain/model sources on a pan. */
    const Sim3DDepthSurfaceFocus visibility =
        PresentSimGlobeFocus_ResolveVisibility(&map,&slot->sim,source);
    ok = PresentSimGlobeTerrain_Append(projection.matrix,map.radius,
            SimBackgroundVoxelRenderer_GroundTexture(slot->sim.background_voxel_serial),
            town_shadow.texture,town_shadow.opacity,&visibility) &&
        PresentSimGlobeMountains_Append(projection.matrix,map.radius) &&
        PresentSimGlobeWater_Append(projection.matrix,map.radius,
            SimBackgroundVoxelRenderer_GroundTexture(slot->sim.background_voxel_serial),&visibility);
    if (ok && content)
      ok=PresentSimGlobeMountains_AppendEffects(projection.matrix,viewport,
          slot->sim.game_frame,slot->sim.background_voxel_detail,slot->sim.background_voxel_style,
          SimGlobeMountainGround);
    if (!ok) fprintf(stderr,"[sim-globe-town] native surface/shadow append rejected\n");
  }
  if (ok) {
    const SimWorldNavigationTowns *towns = &slot->sim.world_navigation_towns;
    /* The detailed town consumes the already published active SIM
     * identities, not navigation's approximation of currently resident art.
     * Borrowed only during this call; retained sources own their values. */
    const SimBackgroundVoxelObject *active_objects = NULL;
    size_t active_count = 0;
    if (detailed_town) {
      const SimBackgroundVoxelScene *active = SimBackgroundVoxels_Scene();
      if (active->overflow || active->town != map.town || active->object_count > kSimBackgroundMaxObjects) ok = false;
      else { active_objects = active->objects; active_count = active->object_count; }
    }
    if (towns->overflow || towns->object_count > kSimWorldNavigationTownObjectCapacity) ok = false;
    const size_t candidates = towns->object_count + active_count;
    if (ok && candidates > s_world_models.gpu_source_capacity) {
      void *buffer = realloc(s_world_models.gpu_sources,candidates*sizeof(WorldNavigationModelSource));
      if (!buffer) ok = false;
      else { s_world_models.gpu_sources = buffer; s_world_models.gpu_source_capacity = candidates; }
    }
    WorldNavigationModelSource *sources = s_world_models.gpu_sources;
    size_t count = 0, radial_count = 0;
    /* Stable partition: neighbours and geometric bridges first, active SIM
     * facades second. Each source is compiled by exactly one representation. */
    for (unsigned group = 0; group < (sim_facades ? 2u : 1u); ++group) {
      for (size_t i = 0; ok && i < candidates; ++i) {
        const SimWorldNavigationTownObject *object = i < towns->object_count
            ? &towns->objects[i] : &active_objects[i-towns->object_count];
        if (active_objects && i < towns->object_count && object->town == map.town) continue;
        if (object->town != map.town && !slot->sim.world_navigation_models) continue;
        const bool facing = sim_facades && object->town == map.town &&
            object->kind != kSimBackgroundVoxel_Bridge;
        if (facing != (group == 1)) continue;
        if (object->kind >= kSimBackgroundVoxelKindCount) continue;
        const SimBackgroundBridgeBounds bounds = WorldNavigationObjectBounds(object);
        int tx, ty;
        if (!SimWorldMap_OriginForTown(object->town,&tx,&ty)) { ok = false; break; }
        const float x = tx+(bounds.origin_x+bounds.width*.5f)/16;
        const float y = ty+(bounds.origin_y+bounds.depth*.5f)/16;
        if (!SimGlobeNearby(&map,x,y)) continue;
        if (count >= kSimWorldNavigationTownObjectCapacity) { ok = false; break; }
        WorldNavigationModelSource *s = &sources[count++];
        memset(s,0,sizeof(*s)); s->object = *object;
        if (object->kind == kSimBackgroundVoxel_Windmill && !(active_objects && facing))
          s->object.animation_phase = 0;
        s->detail = object->town == slot->sim.town
            ? (SimBackgroundVoxelDetail)slot->sim.background_voxel_detail
            : kSimBackgroundVoxelDetail_Low;
        s->object_index = active_objects ? count-1 : i;
        s->source_x = tx*kSimWorldMapTilePixels+bounds.origin_x*.5f;
        s->source_y = ty*kSimWorldMapTilePixels+bounds.origin_y*.5f;
        s->centre_x = bounds.width*.5f; s->centre_y = bounds.depth*.5f;
        if (detailed_town && object->town == map.town) {
          if (!SimWorldNavigationTerrain_RegisterTownFloor(map.town,x-tx,y-ty,
                SimTownTerrain_HeightUnitsAt(map.town,(x-tx)*16,(y-ty)*16),&s->anchor_height)) {
            ok = false; break;
          }
        } else s->anchor_height = map.landscape > 0
            ? WorldNavigationTerrainHeightAt(x*kSimWorldMapTilePixels,y*kSimWorldMapTilePixels,NULL) : 0;
      }
      if (!group) radial_count = count;
    }
    WorldNavigationModelSourceStyle style;
    memset(&style,0,sizeof(style));
    style.embedding = map; style.surface_revision = SimWorldMap_GeographySerial();
    style.chart_radius_tiles = map.chart_radius; style.tile_world = 1/map.metric;
    style.height_percent = slot->sim.height_scale_x100;
    style.light_azimuth = slot->sim.light_azimuth_deg; style.light_elevation = slot->sim.light_elevation_deg;
    style.style = slot->sim.background_voxel_style; style.lighting = true;
    style.focus=t.focus;
    t.radial.variant = (unsigned)((slot->sim.game_frame / 12) % 3) + 1;
    if (ok && count) ok = WorldNavigationModelMesh_Enabled();
    if (ok && radial_count)
      ok = WorldNavigationModelMesh_Draw(sources,radial_count,&style,&t.radial);
    if (ok && sim_facades) {
      /* Active facades are wholly inside the clear town. Do not invalidate
       * their resident sources when a neighbour-only focus setting changes. */
      style.focus=(Sim3DDepthSurfaceFocus){0};
      style.captured_poses = active_objects != NULL;
      const SimBackgroundVoxelRenderParams params = {
        .source = source, .viewport = viewport, .matrix = matrix,
        .facing = slot->sim.background_voxel_facing,
      };
      SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
      SimBackgroundVoxelProject_ResolveAxes(&params,axes);
      ok = WorldNavigationModelMesh_DrawFacingTown(
          sources ? sources+radial_count : NULL,count-radial_count,&style,
          slot->sim.background_voxel_shading,axes,projection.matrix,(unsigned)t.radial.variant-1);
    }
    if (!ok) fprintf(stderr,"[sim-globe-underlay] models rejected count=%zu\n",count);
  }
  if (ok && content) ok = content->append && content->append(content->userdata,&view);
  ArRenderTexture composite = Sim3DDepthPass_Submit(&g_render_device,ArRenderTexture_Invalid());
  if (!ArRenderTexture_IsValid(composite)) fprintf(stderr,"[sim-globe-underlay] composite rejected\n");
  if (!ok || !ArRenderTexture_IsValid(composite)) goto failed;
  return DrawSimGlobeImage(viewport,composite);
failed:
  if (Sim3DDepthPass_IsCollecting())
    (void)Sim3DDepthPass_Submit(&g_render_device,ArRenderTexture_Invalid());
  fprintf(stderr,"[sim-globe-underlay] selected connected world could not be rendered\n");
  return kPresentationOutcome_CoreFailure;
}

PresentationOutcome PresentSimGlobeTown(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const Scene3DCamera *camera,
    const float matrix[16], const PresentSimGlobeContent *content,
    PresentSimGlobeView *out_view) {
  return DrawSimGlobeScene(slot,source,viewport,camera,matrix,
      3,true,true,content,out_view);
}

#if AR_SIM_GLOBE_TESTING
PresentationOutcome PresentSimGlobe_TestTownScene(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const Scene3DCamera *camera,
    const float matrix[16], float radius_scale, PresentSimGlobeView *out_view) {
  return DrawSimGlobeScene(slot,source,viewport,camera,matrix,
      radius_scale,false,false,NULL,out_view);
}

PresentationOutcome PresentSimGlobe_TestFacingTownScene(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const Scene3DCamera *camera,
    const float matrix[16], float radius_scale, PresentSimGlobeView *out_view) {
  return DrawSimGlobeScene(slot,source,viewport,camera,matrix,
      radius_scale,true,false,NULL,out_view);
}

PresentationOutcome PresentSimGlobe_TestDetailedTownScene(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const Scene3DCamera *camera,
    const float matrix[16], float radius_scale, PresentSimGlobeView *out_view) {
  return DrawSimGlobeScene(slot,source,viewport,camera,matrix,
      radius_scale,true,true,NULL,out_view);
}
#endif

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
       !ArRenderTexture_IsValid(s_world_composition.plaque) ||
       (composition->label.visible &&
        !ArRenderTexture_IsValid(s_world_composition.label))))
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
      const ArRenderPointF top_left = WorldNavigationAuthenticToOutput(
          slot, viewport, record->x, record->y);
      const ArRenderPointF bottom_right = WorldNavigationAuthenticToOutput(
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
       !DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->plaque,
           s_world_composition.plaque, (ArRenderPointF){0}, 1.0f) ||
       (!localized_label_ready && composition->label.visible &&
        !DrawWorldNavigationCompositionLayer(
            slot, viewport, &composition->label,
            s_world_composition.label, (ArRenderPointF){0}, 1.0f)) ||
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

/* The world-map half of the presentation-resource reset. PresentSim3D_ResetResources
 * keeps the town half and calls this; see the comment on
 * PresentRendererResources_Reset in present.c for why any of it exists. */
void PresentWorldNav_ResetResources(void) {
  PresentSimGlobeMountains_Reset();
  PresentSimGlobeTerrain_Reset();
  PresentSimGlobeWater_Reset();
  Sim3DMeshSet_Destroy(&s_sim_globe.surface);
  memset(&s_sim_globe, 0, sizeof(s_sim_globe));
  WorldNavigationModelMesh_Reset();
  free(s_world_models.gpu_sources);
  s_world_models.gpu_sources = NULL;
  s_world_models.gpu_source_capacity = 0;
  s_world_models.gpu_sources_unavailable = false;
  s_world_models.gpu_current_ready = false;
  s_world_models.gpu_current_rejected = false;
  Sim3DDepthPass_DestroyMesh(s_world_surfaces.mesh);
  memset(&s_world_surfaces, 0, sizeof(s_world_surfaces));
  Sim3DMeshSet_Destroy(&s_world_gpu_grid.meshes);
  memset(&s_world_gpu_grid, 0, sizeof(s_world_gpu_grid));
  free(s_world_shells.atmosphere_draw.vertices);
  free(s_world_shells.atmosphere_draw.indices);
  s_world_shells.atmosphere_draw = (WorldNavigationAtmosphereDrawCache){0};
  s_world_shells.ocean.ready = s_world_shells.cloud.ready = s_world_shells.atmosphere.ready = false;
  Sim3DDepthPass_DestroyMesh(s_world_weather.receiver_mesh);
  Sim3DDepthPass_DestroyMesh(s_world_weather.spherical_mesh);
  Sim3DDepthPass_DestroyMesh(s_world_weather.body_mesh);
  free(s_world_weather.body_vertices); s_world_weather.body_vertices = NULL;
  s_world_weather.body_mesh = NULL; s_world_weather.body_unavailable = false;
  s_world_weather.body_radius = s_world_weather.body_distance = 0;
  free(s_world_weather.spherical_quads);
  s_world_weather.spherical_mesh = NULL; s_world_weather.spherical_quads = NULL;
  s_world_weather.spherical_capacity = 0;
  s_world_weather.spherical_ready = s_world_weather.spherical_unavailable = false;
  s_world_weather.receiver_mesh = NULL;
  free(s_world_weather.receiver_positions); free(s_world_weather.receiver_uv);
  s_world_weather.receiver_positions = NULL; s_world_weather.receiver_uv = NULL;
  s_world_weather.receiver_mesh_capacity = 0;
  s_world_weather.receiver_mesh_ready = s_world_weather.receiver_mesh_unavailable = false;
  HostParallelWork_Destroy(s_world_workers);
  free(s_world_art.animation);
  s_world_art.animation = NULL;
  s_world_art.animation_unavailable = false;
  free(s_world_model_batch);
  s_world_model_batch = NULL;
  s_world_model_batch_unavailable = false;
  s_world_workers = NULL;
  s_world_workers_attempted = false;
  s_world_terrain.samples_ready = false;
  free(s_world_weather.receivers);
  s_world_weather.receivers = NULL;
  s_world_weather.receiver_count = s_world_weather.receiver_capacity = 0;
  s_world_weather.receivers_ready = s_world_weather.receivers_unavailable = false;
  free(s_world_models.projected);
  Sim3DDepthPass_DestroyMesh(s_world_models.solid_mesh);
  s_world_models.solid_mesh = NULL;
  s_world_models.solid_mesh_published = s_world_models.solid_mesh_unavailable = false;
  s_world_models.solid_mesh_attempted = false;
  s_world_models.solid_mesh_opt_out = false;
  s_world_models.projected = NULL;
  s_world_models.projected_count = s_world_models.projected_capacity = 0;
  s_world_models.projected_valid = s_world_models.capturing = false;
  s_world_models.capture_static = false;
  s_world_models.animated_count = 0;
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
  s_world_art.atlas_cache_unavailable = false;
  free(s_world_art.pixels);
  s_world_art.pixels = NULL;
  free(s_world_art.baseline);
  s_world_art.baseline = NULL;
  s_world_weather.ready = false;
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_composition.palace);
  s_world_composition.palace = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_composition.label);
  s_world_composition.label = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_composition.plaque);
  s_world_composition.plaque = ArRenderTexture_Invalid();
  s_world_composition.uploaded = false;
  s_world_weather.unavailable = false;
  PresentWorldNavSky_Reset();
  s_world_weather.failure_reported = false;
}
