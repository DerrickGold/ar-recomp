#include "sim_background_voxel_renderer.h"
#include "sim3d_performance.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "scene3d_math.h"
#include "sim3d_depth_pass.h"
#include "sim_background_bridge.h"
#include "sim_background_mountain_render.h"
#include "sim_background_voxel_biome.h"
#include "sim_background_voxel_lod.h"
#include "sim_background_voxel_model_cache.h"
#include "sim_background_voxel_models.h"
#include "sim_background_voxel_palette.h"
#include "sim_background_voxel_project.h"
#include "sim_background_voxel_proportions.h"
#include "sim_background_voxel_region.h"
#include "sim_background_voxel_terrain_depth.h"
#include "sim_background_voxels.h"
#include "sim_town_terrain.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

enum {
  /* Smooth 2x is intended for roughly 1080p-class output. At 1440p or 4K its
   * RGBA+D32 target would consume about 118/265 MiB and quadruple fill cost
   * before the final downsample, so large viewports remain native. */
  kMaxSupersampledPixels = 10 * 1024 * 1024,
};

/* Contact shading is a ground decal. It is lifted by a fraction of a source
 * pixel so it wins the depth test against the ground plane it sits on without
 * being separable from it at any supported zoom. */
static const float kContactLiftPixels = 0.06f;
static const float kObjectCullMarginPixels = 24.0f;

typedef struct SimBackgroundSolidProjectionKey {
  uint32_t scene_serial;
  uint8_t detail;
  uint8_t lod;
  uint8_t shading;
  uint8_t style;
  uint8_t facing;
  uint8_t render_scale;
  uint8_t town;
  uint16_t landscape_height_pct;
  uint16_t camera_x, camera_y;
  uint16_t town_screen_x0;
  uint16_t light_azimuth_deg;
  uint8_t light_elevation_deg;
  ArRenderRectI source;
  ArRenderRectI viewport;
  float matrix[16];
  float texture_to_clip[16];
  bool texture_to_clip_valid;
} SimBackgroundSolidProjectionKey;

typedef struct SimBackgroundCachedSolidFace {
  /* Depth-pass vertices are backend-neutral. Retaining them here avoids
   * leaking SDL GPU layout or resource ownership into the portable renderer;
   * the project-private depth-pass seam still performs the backend copy. */
  Sim3DDepthVertex vertices[4];
} SimBackgroundCachedSolidFace;

_Static_assert(sizeof(SimBackgroundCachedSolidFace) ==
                   sizeof(Sim3DDepthVertex) * 4,
               "cached solid faces must be contiguous depth vertices");

typedef struct SimBackgroundSolidProjectionBuilder {
  bool failed;
} SimBackgroundSolidProjectionBuilder;

static struct {
  ArRenderTexture ground;
  uint32_t uploaded_serial;
  uint32_t uploaded_ground_serial;
  uint32_t uploaded_atlas_serial;
  uint32_t uploaded_scene_serial;
  bool mountain_atlas_ready;
  bool allocation_failed;
  uint32_t cache_stamp;
  SimBackgroundVoxelBiome biome;
  SimBackgroundVoxelPalette palettes[kSimBackgroundMaxObjects];
  SimBackgroundCachedSolidFace *projected_solids;
  size_t projected_solid_count;
  size_t projected_solid_capacity;
  SimBackgroundSolidProjectionKey projected_solid_key;
  bool projected_solids_valid;
  SimBackgroundGeometryBatch batch;
} g_renderer_state;

static bool SameRenderRect(ArRenderRectI a, ArRenderRectI b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static bool SolidProjectionCacheMatches(
    const SimBackgroundVoxelRenderParams *params, uint32_t scene_serial) {
  const SimBackgroundSolidProjectionKey *key =
      &g_renderer_state.projected_solid_key;
  return g_renderer_state.projected_solids_valid &&
      key->scene_serial == scene_serial &&
      key->detail == params->detail && key->lod == params->lod &&
      key->shading == params->shading && key->style == params->style &&
      key->facing == params->facing &&
      key->render_scale == params->render_scale &&
      key->town == params->town &&
      key->landscape_height_pct == params->landscape_height_pct &&
      key->camera_x == params->camera_x && key->camera_y == params->camera_y &&
      key->town_screen_x0 == params->town_screen_x0 &&
      key->light_azimuth_deg == params->light_azimuth_deg &&
      key->light_elevation_deg == params->light_elevation_deg &&
      SameRenderRect(key->source, params->source) &&
      SameRenderRect(key->viewport, params->viewport) &&
      key->texture_to_clip_valid == params->texture_to_clip_valid &&
      memcmp(key->matrix, params->matrix, sizeof(key->matrix)) == 0 &&
      memcmp(key->texture_to_clip, params->texture_to_clip,
             sizeof(key->texture_to_clip)) == 0;
}

static void SaveSolidProjectionCacheKey(
    const SimBackgroundVoxelRenderParams *params, uint32_t scene_serial) {
  SimBackgroundSolidProjectionKey *key =
      &g_renderer_state.projected_solid_key;
  *key = (SimBackgroundSolidProjectionKey){
    .scene_serial = scene_serial,
    .detail = params->detail,
    .lod = params->lod,
    .shading = params->shading,
    .style = params->style,
    .facing = params->facing,
    .render_scale = params->render_scale,
    .town = params->town,
    .landscape_height_pct = params->landscape_height_pct,
    .camera_x = params->camera_x,
    .camera_y = params->camera_y,
    .town_screen_x0 = params->town_screen_x0,
    .light_azimuth_deg = params->light_azimuth_deg,
    .light_elevation_deg = params->light_elevation_deg,
    .source = params->source,
    .viewport = params->viewport,
    .texture_to_clip_valid = params->texture_to_clip_valid,
  };
  memcpy(key->matrix, params->matrix, sizeof(key->matrix));
  memcpy(key->texture_to_clip, params->texture_to_clip,
         sizeof(key->texture_to_clip));
}

static bool RetainProjectedSolidFace(
    const Sim3DDepthVertex vertices[4]) {
  if (g_renderer_state.projected_solid_count ==
      g_renderer_state.projected_solid_capacity) {
    size_t capacity = g_renderer_state.projected_solid_capacity
        ? g_renderer_state.projected_solid_capacity * 2u : 2048u;
    if (capacity < g_renderer_state.projected_solid_capacity ||
        capacity > SIZE_MAX / sizeof(*g_renderer_state.projected_solids))
      return false;
    SimBackgroundCachedSolidFace *faces = realloc(
        g_renderer_state.projected_solids,
        capacity * sizeof(*g_renderer_state.projected_solids));
    if (!faces) return false;
    g_renderer_state.projected_solids = faces;
    g_renderer_state.projected_solid_capacity = capacity;
  }
  g_renderer_state.projected_solids[g_renderer_state.projected_solid_count++] =
      (SimBackgroundCachedSolidFace){0};
  memcpy(g_renderer_state.projected_solids[
             g_renderer_state.projected_solid_count - 1].vertices,
         vertices, sizeof(g_renderer_state.projected_solids[0].vertices));
  return true;
}

static void AppendSolidFace(
    const SimBackgroundProjectedFace *face,
    uint16_t palette_index,
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundSolidProjectionBuilder *builder) {
  if (palette_index >= kSimBackgroundMaxObjects) return;
  Sim3DDepthVertex vertices[4];
  if (!SimBackgroundVoxelProject_ResolveFace(
          face, &g_renderer_state.palettes[palette_index],
          (SimBackgroundVoxelShading)params->shading, vertices))
    return;
  Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, vertices);
  if (builder && !builder->failed &&
      !RetainProjectedSolidFace(vertices))
    builder->failed = true;
}

static ArRenderTexture CreateGroundTexture(ArRenderDevice *device) {
  const ArRenderTextureDesc desc = {
    .width = kSimTownCanvasPixels,
    .height = kSimTownCanvasPixels,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture texture = ArRenderTexture_Invalid();
  (void)ArRenderDevice_CreateTexture(device, &desc, &texture);
  return texture;
}

void SimBackgroundVoxelRenderer_Upload(ArRenderDevice *device) {
  uint32_t serial = SimBackgroundVoxels_Serial();
  if (!ArRenderDevice_IsReady(device) || !serial ||
      serial == g_renderer_state.uploaded_serial ||
      g_renderer_state.allocation_failed)
    return;
  bool created_ground = false;
  if (!ArRenderTexture_IsValid(g_renderer_state.ground)) {
    g_renderer_state.ground = CreateGroundTexture(device);
    created_ground = ArRenderTexture_IsValid(g_renderer_state.ground);
  }
  if (!ArRenderTexture_IsValid(g_renderer_state.ground)) {
    g_renderer_state.allocation_failed = true;
    fprintf(stderr, "[sim-bg-voxels] texture allocation failed: %s\n",
            ArRenderDevice_LastError(device));
    return;
  }
  int pitch = kSimTownCanvasPixels * (int)sizeof(uint32_t);
  uint32_t ground_serial = SimBackgroundVoxels_GroundSerial();
  if (created_ground ||
      ground_serial != g_renderer_state.uploaded_ground_serial) {
    const uint32_t *ground = SimBackgroundVoxels_GroundPixels();
    bool upload_ok = true;
    bool uploaded_region = false;
    if (!created_ground && g_renderer_state.uploaded_ground_serial) {
      int x, y, width, height;
      while (SimBackgroundVoxels_TakeGroundDirtyRect(
                 &x, &y, &width, &height)) {
        uploaded_region = true;
        ArRenderRectI dirty = {x, y, width, height};
        if (!ArRenderDevice_UpdateTexture(
                device, g_renderer_state.ground, &dirty,
                ground + (size_t)y * kSimTownCanvasPixels + x, pitch)) {
          upload_ok = false;
          break;
        }
        Sim3DPerformance_AddUpload(
            (uint64_t)width * (uint64_t)height * sizeof(uint32_t));
      }
    }
    /* A new texture is undefined everywhere. A serial mismatch with no dirty
     * region means a previous partial attempt failed after consuming its
     * cursor; recover with one complete publication. */
    if (created_ground || !g_renderer_state.uploaded_ground_serial ||
        !uploaded_region) {
      upload_ok = ArRenderDevice_UpdateTexture(
          device, g_renderer_state.ground, NULL, ground, pitch);
      if (upload_ok) {
        Sim3DPerformance_AddUpload(
            (uint64_t)kSimTownCanvasPixels * kSimTownCanvasPixels *
            sizeof(uint32_t));
        int x, y, width, height;
        while (SimBackgroundVoxels_TakeGroundDirtyRect(
                   &x, &y, &width, &height)) {}
      }
    }
    if (!upload_ok) {
      g_renderer_state.uploaded_ground_serial = 0;
      fprintf(stderr, "[sim-bg-voxels] texture upload failed: %s\n",
              ArRenderDevice_LastError(device));
      return;
    }
    g_renderer_state.uploaded_ground_serial = ground_serial;
  }

  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  bool mountains_required = scene->mountains.cell_count != 0;
  uint32_t atlas_serial = SimBackgroundVoxels_AtlasSerial();
  if (mountains_required &&
      (!g_renderer_state.mountain_atlas_ready ||
       atlas_serial != g_renderer_state.uploaded_atlas_serial)) {
    ArRenderRectI regions[kSimTownCanvasPixels];
    int region_count = 0;
    uint64_t uploaded_bytes = 0;
    if (!g_renderer_state.mountain_atlas_ready) {
      regions[region_count++] = (ArRenderRectI){
        0, 0, kSimTownCanvasPixels, kSimTownCanvasPixels,
      };
      int x, y, width, height;
      while (SimBackgroundVoxels_TakeAtlasDirtyRect(
                 &x, &y, &width, &height)) {}
    } else {
      int x, y, width, height;
      while (region_count < kSimTownCanvasPixels &&
             SimBackgroundVoxels_TakeAtlasDirtyRect(
                 &x, &y, &width, &height)) {
        regions[region_count++] = (ArRenderRectI){x, y, width, height};
      }
      /* A serial mismatch without retained regions means an earlier consumer
       * lost its cursor. Recover the complete pass-owned resource rather than
       * accepting a mixed atlas generation. */
      if (!region_count)
        regions[region_count++] = (ArRenderRectI){
          0, 0, kSimTownCanvasPixels, kSimTownCanvasPixels,
        };
    }
    for (int region = 0; region < region_count; region++)
      uploaded_bytes += (uint64_t)regions[region].w *
          (uint64_t)regions[region].h * sizeof(uint32_t);
    if (!Sim3DDepthPass_UploadMountainAtlasRegions(
            device, SimBackgroundVoxels_AtlasPixels(),
            kSimTownCanvasPixels, kSimTownCanvasPixels, pitch,
            regions, region_count)) {
      fprintf(stderr, "[sim-bg-voxels] GPU mountain atlas upload failed: %s\n",
              Sim3DDepthPass_LastError());
      g_renderer_state.allocation_failed = true;
      return;
    }
    /* This shader-owned texture does not pass through UpdateTexture, so
     * account for it here while the presentation upload scope is active.
     * Otherwise the performance report hides one complete 512x512 transfer
     * whenever the mountain atlas changes. */
    Sim3DPerformance_AddUpload(uploaded_bytes);
    g_renderer_state.uploaded_atlas_serial = atlas_serial;
    g_renderer_state.mountain_atlas_ready = true;
  }

  uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  if (scene_serial != g_renderer_state.uploaded_scene_serial) {
    g_renderer_state.biome = SimBackgroundVoxelBiome_ForTown(scene->town);
    for (uint16_t i = 0; i < scene->object_count; i++)
      SimBackgroundVoxelPalette_Build(
          &scene->objects[i], g_renderer_state.biome,
          &g_renderer_state.palettes[i]);
    g_renderer_state.uploaded_scene_serial = scene_serial;
  }
  g_renderer_state.uploaded_serial = serial;
}

bool SimBackgroundVoxelRenderer_Ready(uint32_t serial) {
  return serial && serial == g_renderer_state.uploaded_serial &&
      ArRenderTexture_IsValid(g_renderer_state.ground);
}

ArRenderTexture SimBackgroundVoxelRenderer_GroundTexture(uint32_t serial) {
  return SimBackgroundVoxelRenderer_Ready(serial) ? g_renderer_state.ground
                                                   : ArRenderTexture_Invalid();
}

static float ObjectOriginX(const SimBackgroundVoxelObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge) {
    return SimBackgroundBridge_ResolveBounds(object).origin_x;
  }
  return object->cell_x * kSimBackgroundCellPixels;
}

static float ObjectOriginY(const SimBackgroundVoxelObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge) {
    return SimBackgroundBridge_ResolveBounds(object).origin_y;
  }
  return (object->cell_y + object->source_cells_h -
          object->footprint_cells_d) * kSimBackgroundCellPixels;
}

static float ObjectFootprintWidth(const SimBackgroundVoxelObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge) {
    return SimBackgroundBridge_ResolveBounds(object).width;
  }
  return object->footprint_cells_w * (float)kSimBackgroundCellPixels;
}

static float ObjectFootprintDepth(const SimBackgroundVoxelObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge) {
    return SimBackgroundBridge_ResolveBounds(object).depth;
  }
  return object->footprint_cells_d * (float)kSimBackgroundCellPixels;
}

static float BridgeApproachLiftPixels(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params) {
#if AR_SIM3D_TERRAIN_ELEVATION
  float height_units;
  int resolved = 0;
  if (object->bridge_axis == kSimBackgroundBridgeAxis_EastWest) {
    resolved = SimTownTerrain_LevelPairUnits(
        params->town,
        object->bridge_bank_a_x, object->bridge_bank_a_y, 1.0f, 0.5f,
        object->bridge_bank_b_x, object->bridge_bank_b_y, 0.0f, 0.5f,
        &height_units);
  } else if (object->bridge_axis ==
             kSimBackgroundBridgeAxis_NorthSouth) {
    resolved = SimTownTerrain_LevelPairUnits(
        params->town,
        object->bridge_bank_a_x, object->bridge_bank_a_y, 0.5f, 1.0f,
        object->bridge_bank_b_x, object->bridge_bank_b_y, 0.5f, 0.0f,
        &height_units);
  }
  if (!resolved) return 0.0f;
  return SimBackgroundVoxelProject_TerrainUnitsToPixels(params, height_units);
#else
  (void)object;
  (void)params;
  return 0.0f;
#endif
}

static float BridgeDepthLiftPixels(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params) {
#if AR_SIM3D_TERRAIN_ELEVATION
  /* Keep projection at the path-centre datum, but depth-test the rigid model
   * against the highest terrain point touched by its footprint. Marahna's
   * transverse grade makes those differ by about 2.35 pixels at the captured
   * 40% landscape setting. Using the maximum for projection left the bridge
   * visibly perched above the path; using the approach for depth let the far
   * terrain corner erase its paving. Two datums give the bridge a buried bank
   * joint without reopening the water/terrain punch-through. */
  const SimBackgroundBridgeBounds bounds =
      SimBackgroundBridge_ResolveBounds(object);
  float height_units;
  if (!SimTownTerrain_MaximumUnitsInRect(
          params->town, bounds.origin_x, bounds.origin_y,
          bounds.origin_x + bounds.width,
          bounds.origin_y + bounds.depth, &height_units))
    return 0.0f;
  return SimBackgroundVoxelProject_TerrainUnitsToPixels(params, height_units);
#else
  (void)object;
  (void)params;
  return 0.0f;
#endif
}

static float ObjectTerrainLiftPixels(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    float local_x, float local_y) {
  if (object->kind == kSimBackgroundVoxel_Bridge) {
    (void)local_x;
    (void)local_y;
    return BridgeApproachLiftPixels(object, params);
  }
  return SimBackgroundVoxelProject_TerrainLiftPixels(
      params,
      ObjectOriginX(object) + local_x,
      ObjectOriginY(object) + local_y);
}

static float ObjectModelLiftPixels(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    float local_x, float local_y) {
  if (object->kind == kSimBackgroundVoxel_Bridge)
    return ObjectTerrainLiftPixels(object, params, local_x, local_y);
  /* A rigid model belongs at the natural height of its plot anchor. Sampling
   * the highest footprint corner lifted whole houses into conspicuous towers;
   * the buried foundation below closes only the downhill gap instead. */
  return ObjectTerrainLiftPixels(
      object, params,
      ObjectFootprintWidth(object) * 0.5f,
      ObjectFootprintDepth(object) * 0.5f);
}

typedef struct SimBackgroundContactBounds {
  float x0, y0, x1, y1;
} SimBackgroundContactBounds;

enum { kSimBackgroundMaxContactQuads = 3 };

#if AR_SIM3D_TERRAIN_ELEVATION
enum {
  kFoundationGapSamplesPerEdge = 5,
  kFoundationMaximumSegmentsPerEdge = 12,
  kFoundationTopBrightness = 214,
  kFoundationBrightnessVariation = 5,
};

static const float kFoundationPrepassVisibleGapPixels = 0.14f;
static const float kFoundationApronPixels = 0.40f;
static const float kFoundationTopInsetPixels = 0.05f;
static const float kFoundationSegmentPixels = 3.5f;
static const float kFoundationFaceVisibleGapPixels = 0.12f;
static const uint8_t kFoundationEdgeBrightness[4] = {178, 194, 210, 186};
#endif

static int ContactBounds(const SimBackgroundVoxelObject *object,
                         SimBackgroundContactBounds *out) {
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      out[0] = (SimBackgroundContactBounds){1.8f, 2.5f, 14.2f, 15.2f};
      return 1;
    case kSimBackgroundVoxel_Cathedral:
      out[0] = (SimBackgroundContactBounds){0.8f, 8.8f, 31.2f, 31.4f};
      return 1;
    case kSimBackgroundVoxel_Windmill:
      out[0] = (SimBackgroundContactBounds){6.2f, 2.3f, 25.8f, 15.2f};
      return 1;
    case kSimBackgroundVoxel_Factory:
      out[0] = (SimBackgroundContactBounds){0.8f, 0.8f, 21.8f, 10.8f};
      out[1] = (SimBackgroundContactBounds){0.8f, 22.2f, 21.8f, 31.2f};
      out[2] = (SimBackgroundContactBounds){21.2f, 0.8f, 31.2f, 31.2f};
      return 3;
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_BroadTree:
      /* Contact belongs to the trunk, not the foliage footprint. This avoids
       * turning dense forests into one continuous dark carpet. */
      out[0] = (SimBackgroundContactBounds){5.4f, 5.4f, 10.6f, 10.6f};
      return 1;
    case kSimBackgroundVoxel_Shrub:
      out[0] = (SimBackgroundContactBounds){5.8f, 5.8f, 10.2f, 10.2f};
      return 1;
    case kSimBackgroundVoxel_StoryTree:
      out[0] = (SimBackgroundContactBounds){10.0f, 11.0f, 22.0f, 23.0f};
      return 1;
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
      out[0] = (SimBackgroundContactBounds){1.0f, 3.0f, 31.0f, 31.5f};
      return 1;
    case kSimBackgroundVoxel_Pyramid:
      out[0] = (SimBackgroundContactBounds){0.5f, 1.5f, 31.5f, 31.5f};
      return 1;
    case kSimBackgroundVoxel_Bridge:
      return 0;
  }
  return 0;
}

static SimBackgroundContactBounds ScaledContactBounds(
    SimBackgroundContactBounds bounds,
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelProportions *proportions) {
  float center_x = ObjectFootprintWidth(object) * 0.5f;
  float center_y = ObjectFootprintDepth(object) * 0.5f;
  return (SimBackgroundContactBounds){
    center_x + (bounds.x0 - center_x) * proportions->footprint_scale,
    center_y + (bounds.y0 - center_y) * proportions->footprint_scale,
    center_x + (bounds.x1 - center_x) * proportions->footprint_scale,
    center_y + (bounds.y1 - center_y) * proportions->footprint_scale,
  };
}

static void AppendGroundContact(
    const SimBackgroundVoxelObject *object,
    uint16_t palette_index,
    const SimBackgroundVoxelRenderParams *params,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions,
    SimBackgroundSolidProjectionBuilder *builder) {
  if (params->shading < kSimBackgroundVoxelShading_AmbientOcclusion)
    return;
  SimBackgroundContactBounds bounds[kSimBackgroundMaxContactQuads];
  int count = ContactBounds(object, bounds);
  for (int at = 0; at < count; at++) {
    SimBackgroundContactBounds scaled = ScaledContactBounds(
        bounds[at], object, proportions);
    float x0 = scaled.x0, x1 = scaled.x1;
    float y0 = scaled.y0, y1 = scaled.y1;
    const float local_x[4] = {x0, x1, x1, x0};
    const float local_y[4] = {y0, y0, y1, y1};
    SimBackgroundProjectedFace face = {
      .material = kSimVoxelMaterial_Contact,
      .brightness = {255, 255, 255, 255},
    };
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      float terrain_lift = ObjectTerrainLiftPixels(
          object, params, local_x[point], local_y[point]);
      if (!SimBackgroundVoxelProject_GroundedVertex(
              params, &kSimBackgroundUprightProjectionAxis,
              origin_x + local_x[point], origin_y + local_y[point],
              kContactLiftPixels, terrain_lift,
              &face.points[point], &face.gpu_depth[point])) {
        valid = false;
        break;
      }
    }
    if (valid && !SimBackgroundVoxelProject_IsDegenerate(face.points))
      AppendSolidFace(&face, palette_index, params, builder);
  }
}

#if AR_SIM3D_TERRAIN_ELEVATION
static bool ObjectUsesBuriedFoundation(
    const SimBackgroundVoxelObject *object) {
  if (!object) return false;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
    case kSimBackgroundVoxel_Cathedral:
    case kSimBackgroundVoxel_Windmill:
    case kSimBackgroundVoxel_Factory:
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
    case kSimBackgroundVoxel_Pyramid:
      return true;
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_Shrub:
    case kSimBackgroundVoxel_StoryTree:
    case kSimBackgroundVoxel_Bridge:
      return false;
  }
  return false;
}

static void AppendFoundationFace(
    const SimBackgroundVoxelRenderParams *params,
    uint16_t palette_index,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float absolute_z[4], SimBackgroundVoxelMaterial material,
    uint8_t brightness,
    SimBackgroundSolidProjectionBuilder *builder) {
  SimBackgroundProjectedFace face = {
    .material = (uint8_t)material,
    .brightness = {brightness, brightness, brightness, brightness},
  };
  for (int point = 0; point < 4; point++)
    if (!SimBackgroundVoxelProject_GroundedVertex(
            params, &kSimBackgroundUprightProjectionAxis,
            origin_x + local_x[point], origin_y + local_y[point],
            0.0f, absolute_z[point],
            &face.points[point], &face.gpu_depth[point]))
      return;
  if (!SimBackgroundVoxelProject_IsDegenerate(face.points))
    AppendSolidFace(&face, palette_index, params, builder);
}

static bool FoundationHasExposedGap(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundContactBounds bounds, float foundation_top) {
  for (int edge = 0; edge < 4; edge++)
    for (int sample = 0; sample < kFoundationGapSamplesPerEdge; sample++) {
      float t = sample / (float)(kFoundationGapSamplesPerEdge - 1);
      float x, y;
      if (edge == 0 || edge == 2) {
        x = bounds.x0 + (bounds.x1 - bounds.x0) * t;
        y = edge == 0 ? bounds.y0 : bounds.y1;
      } else {
        x = edge == 1 ? bounds.x1 : bounds.x0;
        y = bounds.y0 + (bounds.y1 - bounds.y0) * t;
      }
      if (foundation_top - ObjectTerrainLiftPixels(
              object, params, x, y) >
          kFoundationPrepassVisibleGapPixels)
        return true;
    }
  return false;
}

static void AppendBuildingFoundation(
    const SimBackgroundVoxelObject *object,
    uint16_t palette_index,
    const SimBackgroundVoxelRenderParams *params,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions,
    float anchor_lift,
    SimBackgroundSolidProjectionBuilder *builder) {
  if (!ObjectUsesBuriedFoundation(object)) return;

  /* The top sits fractionally inside the model base. The depth-tested terrain
   * buries its uphill portion; only exposed downhill infill survives. */
  const float foundation_top = anchor_lift - kFoundationTopInsetPixels;
  SimBackgroundContactBounds authored[kSimBackgroundMaxContactQuads];
  int count = ContactBounds(object, authored);
  for (int part = 0; part < count; part++) {
    SimBackgroundContactBounds bounds = ScaledContactBounds(
        authored[part], object, proportions);
    float apron = kFoundationApronPixels * proportions->footprint_scale;
    bounds.x0 = fmaxf(0.0f, bounds.x0 - apron);
    bounds.y0 = fmaxf(0.0f, bounds.y0 - apron);
    bounds.x1 = fminf(ObjectFootprintWidth(object), bounds.x1 + apron);
    bounds.y1 = fminf(ObjectFootprintDepth(object), bounds.y1 + apron);
    if (!FoundationHasExposedGap(
            object, params, bounds, foundation_top))
      continue;

    const float top_x[4] = {bounds.x0, bounds.x1, bounds.x1, bounds.x0};
    const float top_y[4] = {bounds.y0, bounds.y0, bounds.y1, bounds.y1};
    const float top_z[4] = {
      foundation_top, foundation_top, foundation_top, foundation_top,
    };
    AppendFoundationFace(
        params, palette_index, origin_x, origin_y,
        top_x, top_y, top_z, kSimVoxelMaterial_Foundation,
        kFoundationTopBrightness, builder);

    for (int edge = 0; edge < 4; edge++) {
      float edge_length = edge == 0 || edge == 2
          ? bounds.x1 - bounds.x0 : bounds.y1 - bounds.y0;
      int segment_count = (int)ceilf(edge_length /
          kFoundationSegmentPixels);
      if (segment_count < 1) segment_count = 1;
      if (segment_count > kFoundationMaximumSegmentsPerEdge)
        segment_count = kFoundationMaximumSegmentsPerEdge;
      for (int segment = 0; segment < segment_count; segment++) {
        float a = segment / (float)segment_count;
        float b = (segment + 1) / (float)segment_count;
        float x0, y0, x1, y1;
        switch (edge) {
          case 0:
            x0 = bounds.x0 + (bounds.x1 - bounds.x0) * a;
            x1 = bounds.x0 + (bounds.x1 - bounds.x0) * b;
            y0 = y1 = bounds.y0;
            break;
          case 1:
            x0 = x1 = bounds.x1;
            y0 = bounds.y0 + (bounds.y1 - bounds.y0) * a;
            y1 = bounds.y0 + (bounds.y1 - bounds.y0) * b;
            break;
          case 2:
            x0 = bounds.x1 - (bounds.x1 - bounds.x0) * a;
            x1 = bounds.x1 - (bounds.x1 - bounds.x0) * b;
            y0 = y1 = bounds.y1;
            break;
          default:
            x0 = x1 = bounds.x0;
            y0 = bounds.y1 - (bounds.y1 - bounds.y0) * a;
            y1 = bounds.y1 - (bounds.y1 - bounds.y0) * b;
            break;
        }
        float ground0 = ObjectTerrainLiftPixels(object, params, x0, y0);
        float ground1 = ObjectTerrainLiftPixels(object, params, x1, y1);
        /* An uphill endpoint is embedded in the terrain, never extruded upward
         * as an inverted wall. A single exposed endpoint naturally becomes a
         * triangular wedge along the hillside. */
        if (ground0 > foundation_top) ground0 = foundation_top;
        if (ground1 > foundation_top) ground1 = foundation_top;
        if (foundation_top - ground0 < kFoundationFaceVisibleGapPixels &&
            foundation_top - ground1 < kFoundationFaceVisibleGapPixels)
          continue;
        const float side_x[4] = {x0, x1, x1, x0};
        const float side_y[4] = {y0, y1, y1, y0};
        const float side_z[4] = {
          foundation_top, foundation_top, ground1, ground0,
        };
        int variation = ((segment + part + object->record_slot) & 1)
            ? kFoundationBrightnessVariation
            : -kFoundationBrightnessVariation;
        uint8_t brightness = (uint8_t)(
            kFoundationEdgeBrightness[edge] + variation);
        AppendFoundationFace(
            params, palette_index, origin_x, origin_y,
            side_x, side_y, side_z, kSimVoxelMaterial_Foundation,
            brightness, builder);
      }
    }
  }
}
#endif

static float ModelAuthoredHeight(const SimBackgroundVoxelObject *object) {
  return SimBackgroundVoxelRegion_AuthoredHeight(object);
}

static bool ObjectMayBeVisible(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float model_lift) {
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      ObjectOriginX(object);
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  float center_x = ObjectFootprintWidth(object) * 0.5f;
  float center_y = ObjectFootprintDepth(object) * 0.5f;
  float half_x = center_x * proportions->footprint_scale;
  float half_y = center_y * proportions->footprint_scale;
  float x[2] = {origin_x + center_x - half_x,
                origin_x + center_x + half_x};
  float y[2] = {origin_y + center_y - half_y,
                origin_y + center_y + half_y};
  float z[2] = {0.0f,
                ModelAuthoredHeight(object) * proportions->height_scale};
  float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
  bool any = false;
  for (int zi = 0; zi < 2; zi++)
    for (int yi = 0; yi < 2; yi++)
      for (int xi = 0; xi < 2; xi++) {
        Scene3DPoint point;
        if (!SimBackgroundVoxelProject_GroundedPoint(
                params, axis, x[xi], y[yi], z[zi], model_lift,
                &point, NULL))
          continue;
        if (!any) {
          min_x = max_x = point.x;
          min_y = max_y = point.y;
          any = true;
        } else {
          if (point.x < min_x) min_x = point.x;
          if (point.x > max_x) max_x = point.x;
          if (point.y < min_y) min_y = point.y;
          if (point.y > max_y) max_y = point.y;
        }
      }
  if (!any) return false;
  /* Output-space padding covers projecting edges whose corners straddle the
   * near plane, while remaining independent of source zoom and aspect. */
  const float margin = kObjectCullMarginPixels *
      (params->render_scale == kSimBackgroundVoxelRenderScale_2x
          ? 2.0f : 1.0f);
  return max_x >= params->viewport.x - margin &&
      min_x <= params->viewport.x + params->viewport.w + margin &&
      max_y >= params->viewport.y - margin &&
      min_y <= params->viewport.y + params->viewport.h + margin;
}

static SimBackgroundVoxelDetail EffectiveDetail(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions,
    float center_x, float center_y, float model_lift) {
  SimBackgroundVoxelDetail requested =
      (SimBackgroundVoxelDetail)params->detail;
  if (params->lod != kSimBackgroundVoxelLod_Adaptive) return requested;
  Scene3DPoint bottom, top;
  float height = ModelAuthoredHeight(object) * proportions->height_scale;
  if (!SimBackgroundVoxelProject_GroundedPoint(
          params, axis, origin_x + center_x, origin_y + center_y,
          0.0f, model_lift, &bottom, NULL) ||
      !SimBackgroundVoxelProject_GroundedPoint(
          params, axis, origin_x + center_x, origin_y + center_y,
          height, model_lift, &top, NULL))
    return requested;
  float dx = top.x - bottom.x;
  float dy = top.y - bottom.y;
  float projected_height = sqrtf(dx * dx + dy * dy);
  if (params->render_scale == kSimBackgroundVoxelRenderScale_2x)
    projected_height *= 0.5f;
  return SimBackgroundVoxelLod_Resolve(
      requested, kSimBackgroundVoxelLod_Adaptive,
      projected_height);
}

static void DrawModel(
    const SimBackgroundVoxelObject *object,
    uint16_t palette_index,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float anchor_lift, float depth_lift,
    SimBackgroundSolidProjectionBuilder *builder) {
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      ObjectOriginX(object);
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  float center_x = ObjectFootprintWidth(object) * 0.5f;
  float center_y = ObjectFootprintDepth(object) * 0.5f;
  SimBackgroundVoxelDetail detail = EffectiveDetail(
      object, params, axis, origin_x, origin_y, proportions,
      center_x, center_y, anchor_lift);
  /* Shading rides along with the geometry: none of its inputs move with the
   * camera, so the cache resolves it once per model per lighting state. */
  const SimBackgroundVoxelModelShadingKey shading_key = {
    .light_azimuth_deg = params->light_azimuth_deg,
    .light_elevation_deg = params->light_elevation_deg,
    .shading = params->shading,
    .biome = (uint8_t)g_renderer_state.biome,
  };
  const SimBackgroundVoxelModelShading *shading = NULL;
  const SimBackgroundVoxelModel *model = SimBackgroundVoxelModelCache_Get(
      object, detail, (SimBackgroundVoxelStyle)params->style,
      g_renderer_state.cache_stamp, &shading_key, &shading);
  if (!model || !model->face_count || model->overflow || !shading) return;
#if AR_SIM3D_TERRAIN_ELEVATION
  AppendBuildingFoundation(
      object, palette_index, params, origin_x, origin_y,
      proportions, anchor_lift, builder);
#endif
  AppendGroundContact(object, palette_index, params, origin_x, origin_y,
                      proportions, builder);
  /* Faces are submitted as they are projected on a cache miss and retained by
   * the renderer as portable projected faces. A settled camera can then replay
   * them through the same palette/projection submission boundary without
   * repeating model-space projection on every presentation. */
  for (uint16_t face_index = 0; face_index < model->face_count; face_index++) {
    const SimBackgroundVoxelModelFace *source = &model->faces[face_index];
    SimBackgroundProjectedFace face = {
      .material = shading->material[face_index],
    };
    memcpy(face.brightness, shading->brightness[face_index],
           sizeof(face.brightness));
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      float local_x = center_x +
          (source->points[point].x - center_x) *
              proportions->footprint_scale;
      float local_y = center_y +
          (source->points[point].y - center_y) *
              proportions->footprint_scale;
      float local_z = source->points[point].z *
          proportions->height_scale;
      bool projected;
      if (object->kind == kSimBackgroundVoxel_Bridge) {
        projected = SimBackgroundVoxelProject_GroundedVertexWithDepthGround(
            params, axis,
            origin_x + local_x, origin_y + local_y,
            local_z, anchor_lift,
            depth_lift,
            &face.points[point], &face.gpu_depth[point]);
      } else {
        projected = SimBackgroundVoxelProject_GroundedVertex(
            params, axis,
            origin_x + local_x, origin_y + local_y,
            local_z, anchor_lift,
            &face.points[point], &face.gpu_depth[point]);
      }
      if (!projected) {
        valid = false;
        break;
      }
    }
    if (!valid || SimBackgroundVoxelProject_IsDegenerate(face.points)) continue;
    AppendSolidFace(&face, palette_index, params, builder);
  }
}

typedef struct SimBackgroundVisibleModel {
  uint16_t index;
  SimBackgroundProjectionAxis axis;
  float anchor_lift;
  float depth_lift;
} SimBackgroundVisibleModel;

typedef struct SimBackgroundVisibleModelList {
  uint16_t count;
  SimBackgroundVisibleModel entries[kSimBackgroundMaxObjects];
} SimBackgroundVisibleModelList;

static bool RenderParamsValid(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params) {
  if (!ArRenderDevice_IsReady(device) || !params || !params->matrix ||
      params->source.w <= 0 ||
      params->source.h <= 0 || params->viewport.w <= 0 ||
      params->viewport.h <= 0 ||
      !SimBackgroundVoxelRenderer_Ready(params->serial))
    return false;
  return true;
}

static void BuildVisibleModelList(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVisibleModelList *list) {
  list->count = 0;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  SimBackgroundVoxelProject_ResolveAxes(params, axes);
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (object->kind >= kSimBackgroundVoxelKindCount) continue;
    SimBackgroundVisibleModel entry = {
      .index = i,
      .axis = axes[object->kind],
    };
    const float center_x = ObjectFootprintWidth(object) * 0.5f;
    const float center_y = ObjectFootprintDepth(object) * 0.5f;
    entry.anchor_lift = ObjectModelLiftPixels(
        object, params, center_x, center_y);
    entry.depth_lift = object->kind == kSimBackgroundVoxel_Bridge
        ? BridgeDepthLiftPixels(object, params) : entry.anchor_lift;
    if (!ObjectMayBeVisible(
            object, params, &entry.axis, entry.anchor_lift))
      continue;
    list->entries[list->count++] = entry;
  }
}

static void CollectDepthGeometry(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundVisibleModelList *list,
    int mountain_relief_count,
    uint32_t scene_serial,
    bool solid_projection_cached) {
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  /* Submission order is intentionally immaterial. One opaque mountain draw
   * and one solid-model draw share the same D32 attachment; the GPU resolves
   * visibility per pixel instead of relying on CPU object/face ordering. */
#if AR_SIM3D_TERRAIN_ELEVATION
  SimBackgroundVoxelTerrainDepth_Append(params);
#endif
  SimBackgroundMountainRender_SubmitFaces(mountain_relief_count);

  if (solid_projection_cached) {
    if (g_renderer_state.projected_solid_count)
      Sim3DDepthPass_AppendQuads(
          kSim3DDepthPass_Solid,
          g_renderer_state.projected_solids[0].vertices,
          g_renderer_state.projected_solid_count);
    return;
  }

  g_renderer_state.projected_solid_count = 0;
  g_renderer_state.projected_solids_valid = false;
  SimBackgroundSolidProjectionBuilder builder = {0};
  for (uint16_t at = 0; at < list->count; at++) {
    const SimBackgroundVisibleModel *entry = &list->entries[at];
    uint16_t index = entry->index;
    const SimBackgroundVoxelObject *object = &scene->objects[index];
    DrawModel(object, index, params, &entry->axis,
              entry->anchor_lift, entry->depth_lift, &builder);
  }
  if (!builder.failed) {
    SaveSolidProjectionCacheKey(params, scene_serial);
    g_renderer_state.projected_solids_valid = true;
  } else {
    g_renderer_state.projected_solid_count = 0;
  }
}

static void GroundDepthRange(
    const SimBackgroundVoxelRenderParams *params,
    float *minimum, float *maximum) {
#if AR_SIM3D_TERRAIN_ELEVATION
  SimBackgroundVoxelTerrainDepth_GroundDepthRange(params, minimum, maximum);
#else
  *minimum = FLT_MAX;
  *maximum = -FLT_MAX;
  const float x[2] = {(float)params->source.x,
                      (float)(params->source.x + params->source.w)};
  const float y[2] = {(float)params->source.y,
                      (float)(params->source.y + params->source.h)};
  for (int yi = 0; yi < 2; yi++)
    for (int xi = 0; xi < 2; xi++) {
      float world_x, world_y, world_z;
      SimBackgroundVoxelProject_TexturePointToWorld(params, x[xi], y[yi], 0.0f,
                          &world_x, &world_y, &world_z);
      float depth = Scene3D_ClipDepth(
          params->matrix, world_x, world_y, world_z);
      if (depth < *minimum) *minimum = depth;
      if (depth > *maximum) *maximum = depth;
    }
#endif
}

static bool BeginDepthTarget(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelRenderParams *draw_params) {
  if (!RenderParamsValid(device, params) || !draw_params) return false;
  *draw_params = *params;
  int output_scale = params->render_scale ==
          kSimBackgroundVoxelRenderScale_2x &&
      params->detail >= kSimBackgroundVoxelDetail_High ? 2 : 1;
  const uint64_t supersampled_pixels =
      (uint64_t)params->viewport.w * (uint64_t)params->viewport.h * 4u;
  if (output_scale == 2 &&
      supersampled_pixels > kMaxSupersampledPixels)
    output_scale = 1;
  if (params->viewport.w > INT_MAX / output_scale ||
      params->viewport.h > INT_MAX / output_scale)
    return false;
  draw_params->viewport = (ArRenderRectI){
    0, 0,
    params->viewport.w * output_scale,
    params->viewport.h * output_scale,
  };
  SimBackgroundVoxelProject_Prepare(draw_params);
  ArRenderFilter output_filter =
      params->render_scale == kSimBackgroundVoxelRenderScale_PixelClean
          ? kArRenderFilter_Nearest : kArRenderFilter_Linear;
  return Sim3DDepthPass_Begin(
      device, draw_params->viewport.w, draw_params->viewport.h,
      output_filter);
}

static void CompositeDepthTarget(
    ArRenderDevice *device, ArRenderTexture texture,
    const SimBackgroundVoxelRenderParams *params) {
  if (!ArRenderTexture_IsValid(texture)) return;
  ArRenderRectF destination = {
    (float)params->viewport.x, (float)params->viewport.y,
    (float)params->viewport.w, (float)params->viewport.h,
  };
  if (ArRenderDevice_DrawTexture(device, texture, NULL, &destination))
    Sim3DPerformance_AddDraw(0, 0);
}

static void DrawDepthLayers(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelDepthLayerCallback callback, void *userdata,
    bool interleaved) {
  SimBackgroundVoxelRenderParams draw_params;
  if (!BeginDepthTarget(device, params, &draw_params)) return;

  g_renderer_state.cache_stamp++;
  if (!g_renderer_state.cache_stamp) g_renderer_state.cache_stamp = 1;
  const uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  const bool solid_projection_cached =
      SolidProjectionCacheMatches(&draw_params, scene_serial);
  SimBackgroundVisibleModelList list = {0};
  Sim3DPerformanceScope cull_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthCull);
  if (!solid_projection_cached)
    BuildVisibleModelList(&draw_params, &list);
  Sim3DPerformance_End(cull_performance);
  Sim3DPerformanceScope mountain_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthMountain);
  int mountain_relief_count =
      SimBackgroundMountainRender_BuildFaces(&draw_params);
  Sim3DPerformance_End(mountain_performance);
  float visible_minimum = 0.0f, visible_maximum = 0.0f;
  if (callback && interleaved)
    GroundDepthRange(&draw_params, &visible_minimum, &visible_maximum);
  Sim3DPerformanceScope project_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  CollectDepthGeometry(&draw_params, &list, mountain_relief_count,
                       scene_serial, solid_projection_cached);
  Sim3DPerformance_End(project_performance);
  Sim3DPerformanceScope submit_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_DepthSubmit);
  ArRenderTexture depth_composite = Sim3DDepthPass_Submit(
      device, ArRenderTexture_Invalid());
  Sim3DPerformance_End(submit_performance);
  /* Actors standing on ordinary ground go UNDER the composite, so the town's
   * own geometry hides them: a villager behind a peak or behind a house is
   * covered by it. The overhead camera means anything merely beside a building
   * still shows, because a model only occludes what is behind it on screen.
   *
   * Actors standing on a mountain cell go over it instead, lifted onto the
   * surface -- that is the town event that sends a villager up a peak, and the
   * authentic 2D map already says which case an actor is in. */
  if (callback && interleaved)
    callback(userdata, params, visible_minimum, visible_maximum,
             kSimBackgroundVoxelActorBand_Ground);
  CompositeDepthTarget(device, depth_composite, params);
  if (callback && interleaved) {
    callback(userdata, params, visible_minimum, visible_maximum,
             kSimBackgroundVoxelActorBand_Mountain);
    callback(userdata, params, 0.0f, 0.0f,
             kSimBackgroundVoxelActorBand_Overhead);
  }
}

void SimBackgroundVoxelRenderer_DrawTerrainShadow(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params) {
#if AR_SIM3D_TERRAIN_ELEVATION
  if (!params || !ArRenderTexture_IsValid(params->shadow_mask) ||
      !params->shadow_opacity_pct)
    return;
  SimBackgroundVoxelRenderParams draw_params;
  if (!BeginDepthTarget(device, params, &draw_params)) return;
  /* This pass deliberately contains no model geometry. It runs at BG1Low so
   * the clipped mask darkens only the ground; later actor/model ranks retain
   * the authentic painter relationship and cover it normally. */
  SimBackgroundVoxelTerrainDepth_Append(&draw_params);
  ArRenderTexture shadow_composite = Sim3DDepthPass_Submit(
      device, draw_params.shadow_mask);
  CompositeDepthTarget(device, shadow_composite, params);
#else
  (void)device;
  (void)params;
#endif
}

void SimBackgroundVoxelRenderer_Draw(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params) {
  DrawDepthLayers(device, params, NULL, NULL, false);
}

void SimBackgroundVoxelRenderer_DrawInterleaved(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelDepthLayerCallback callback, void *userdata) {
  DrawDepthLayers(device, params, callback, userdata, true);
}

static void AppendSolidQuad(SimBackgroundGeometryBatch *batch,
                            const Scene3DPoint points[4]) {
  if (batch->vertex_count + 4 > kSimBackgroundBatchMaxVertices ||
      batch->index_count + 6 > kSimBackgroundBatchMaxIndices)
    return;
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  int base = batch->vertex_count;
  for (int i = 0; i < 4; i++)
    batch->vertices[batch->vertex_count++] =
        (ArRenderVertex2D){{points[i].x, points[i].y}, black, {0, 0}};
  static const int indices[] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++)
    batch->indices[batch->index_count++] = base + indices[i];
}

typedef struct SimBackgroundShadowBounds {
  float x0, y0, x1, y1, height;
} SimBackgroundShadowBounds;

enum {
  kSimBackgroundMaxShadowVolumes = 3,
  /* A volume emits its base and offset caps plus one bridge per edge. */
  kShadowQuadsPerVolume = 6,
  kMaxShadowQuadsPerObject =
      kSimBackgroundMaxShadowVolumes * kShadowQuadsPerVolume,
};

_Static_assert(kSimBackgroundBatchMaxQuads >= kMaxShadowQuadsPerObject,
               "the shadow batch must hold one caster's volumes outright");

static int ShadowBounds(const SimBackgroundVoxelObject *object,
                        SimBackgroundShadowBounds *out) {
  bool construction =
      (object->flags & kSimBackgroundVoxel_UnderConstruction) != 0;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      out[0] = (SimBackgroundShadowBounds){
        1.0f, 2.0f, 15.0f, 15.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_Cathedral:
      out[0] = (SimBackgroundShadowBounds){
        0.0f, 8.5f, 32.0f, 32.0f, 24.0f};
      return 1;
    case kSimBackgroundVoxel_Windmill:
      out[0] = (SimBackgroundShadowBounds){
        5.0f, 2.0f, 27.0f, 16.0f, construction ? 24.0f : 32.0f};
      return 1;
    case kSimBackgroundVoxel_Factory:
      if (construction) {
        out[0] = (SimBackgroundShadowBounds){
          0.5f, 0.5f, 31.5f, 32.0f, 10.0f};
        return 1;
      }
      /* Match the authored sideways-U mass instead of darkening its grass
       * courtyard with the old full-footprint approximation. */
      out[0] = (SimBackgroundShadowBounds){
        0.5f, 0.5f, 21.5f, 11.5f, 17.0f};
      out[1] = (SimBackgroundShadowBounds){
        0.5f, 20.5f, 21.5f, 31.5f, 17.0f};
      out[2] = (SimBackgroundShadowBounds){
        21.5f, 0.5f, 31.5f, 31.5f, 17.0f};
      return 3;
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Shrub:
      out[0] = (SimBackgroundShadowBounds){
        0.0f, 0.0f, 16.0f, 16.0f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_StoryTree:
      out[0] = (SimBackgroundShadowBounds){
        4.5f, 4.5f, 27.5f, 27.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
      out[0] = (SimBackgroundShadowBounds){
        1.0f, 3.0f, 31.0f, 31.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_Pyramid:
      out[0] = (SimBackgroundShadowBounds){
        0.5f, 1.5f, 31.5f, 31.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_Bridge:
      /* Its shallow dark underside is self-shading; a large directional mask
       * would paint an implausible opaque stripe across the river. */
      return 0;
  }
  return 0;
}

static void AppendShadowVolume(
    SimBackgroundGeometryBatch *batch,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundVoxelObject *object,
    SimBackgroundShadowBounds bounds, float axis_z_scale,
    float light_x, float light_y) {
  if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0 ||
      bounds.height <= 0.0f)
    return;
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  float center_x = ObjectFootprintWidth(object) * 0.5f;
  float center_y = ObjectFootprintDepth(object) * 0.5f;
  float local_x[2] = {
    center_x + (bounds.x0 - center_x) *
        proportions->footprint_scale,
    center_x + (bounds.x1 - center_x) *
        proportions->footprint_scale,
  };
  float local_y[2] = {
    center_y + (bounds.y0 - center_y) *
        proportions->footprint_scale,
    center_y + (bounds.y1 - center_y) *
        proportions->footprint_scale,
  };
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      ObjectOriginX(object);
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  float height_world = bounds.height * proportions->height_scale *
      axis_z_scale / params->source.h;
  Scene3DPoint base[4], offset[4];
  for (int corner = 0; corner < 4; corner++) {
    const float corner_x = local_x[corner == 1 || corner == 2];
    const float corner_y = local_y[corner >= 2];
    const float terrain_lift = ObjectTerrainLiftPixels(
        object, params, corner_x, corner_y);
    float world_x, world_y, world_z;
    SimBackgroundVoxelProject_TexturePointToWorld(
        params, origin_x + corner_x,
        origin_y + corner_y, terrain_lift,
        &world_x, &world_y, &world_z);
    if (!Scene3D_ProjectWorldPoint(
            params->matrix, world_x, world_y, world_z,
            params->viewport.w, params->viewport.h, &base[corner]) ||
        !Scene3D_ProjectWorldPoint(
            params->matrix,
            world_x + height_world * light_x,
            world_y + height_world * light_y, world_z,
            params->viewport.w, params->viewport.h, &offset[corner]))
      return;
    base[corner].x += params->viewport.x;
    base[corner].y += params->viewport.y;
    offset[corner].x += params->viewport.x;
    offset[corner].y += params->viewport.y;
  }
  AppendSolidQuad(batch, base);
  AppendSolidQuad(batch, offset);
  for (int edge = 0; edge < 4; edge++) {
    int next = (edge + 1) & 3;
    Scene3DPoint bridge[4] = {
      base[edge], base[next], offset[next], offset[edge],
    };
    if (!SimBackgroundVoxelProject_IsDegenerate(bridge))
      AppendSolidQuad(batch, bridge);
  }
}

void SimBackgroundVoxelRenderer_DrawShadowMask(
    ArRenderDevice *device, const SimBackgroundVoxelRenderParams *params,
    float light_x, float light_y) {
  if (!RenderParamsValid(device, params)) return;
  SimBackgroundVoxelRenderParams prepared_params = *params;
  SimBackgroundVoxelProject_Prepare(&prepared_params);
  params = &prepared_params;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  SimBackgroundGeometryBatch *batch = &g_renderer_state.batch;
  batch->vertex_count = 0;
  batch->index_count = 0;
  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  SimBackgroundVoxelProject_ResolveAxes(params, axes);
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (object->kind >= kSimBackgroundVoxelKindCount) continue;
    const SimBackgroundProjectionAxis *axis = &axes[object->kind];
    const float center_x = ObjectFootprintWidth(object) * 0.5f;
    const float center_y = ObjectFootprintDepth(object) * 0.5f;
    const float model_lift = ObjectModelLiftPixels(
        object, params, center_x, center_y);
    if (!ObjectMayBeVisible(object, params, axis, model_lift)) continue;
    SimBackgroundShadowBounds bounds[kSimBackgroundMaxShadowVolumes];
    int volume_count = ShadowBounds(object, bounds);
    /* Flush ahead of the caster that would not fit. AppendSolidQuad drops
     * silently when full, and a developed town can ask for more than this
     * batch holds - 1160 objects at three volumes is 20880 quads - so
     * without this the shadows nearest the end of the scene simply stop
     * appearing. Splitting the mask across draws is free: it is opaque black
     * into an offscreen target, so overlap is idempotent and order does not
     * matter, and the OBJ caster pass already issues one draw apiece. */
    if (batch->vertex_count + kMaxShadowQuadsPerObject * 4 >
            kSimBackgroundBatchMaxVertices ||
        batch->index_count + kMaxShadowQuadsPerObject * 6 >
            kSimBackgroundBatchMaxIndices)
      SimBackgroundVoxelProject_FlushBatch(device, batch);
    for (int volume = 0; volume < volume_count; volume++)
      AppendShadowVolume(batch, params, object, bounds[volume],
                         axis->height_scale,
                         light_x, light_y);
  }
  SimBackgroundVoxelProject_FlushBatch(device, batch);
}

void SimBackgroundVoxelRenderer_Reset(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, g_renderer_state.ground);
  Sim3DDepthPass_Reset(device);
  g_renderer_state.ground = ArRenderTexture_Invalid();
  g_renderer_state.uploaded_serial = 0;
  g_renderer_state.uploaded_ground_serial = 0;
  g_renderer_state.uploaded_atlas_serial = 0;
  g_renderer_state.uploaded_scene_serial = 0;
  g_renderer_state.mountain_atlas_ready = false;
  g_renderer_state.allocation_failed = false;
  g_renderer_state.cache_stamp = 0;
  g_renderer_state.biome = kSimBackgroundVoxelBiome_Temperate;
  free(g_renderer_state.projected_solids);
  g_renderer_state.projected_solids = NULL;
  g_renderer_state.projected_solid_count = 0;
  g_renderer_state.projected_solid_capacity = 0;
  g_renderer_state.projected_solids_valid = false;
  SimBackgroundMountainRender_Reset();
  SimBackgroundVoxelModelCache_Reset();
  g_renderer_state.batch.vertex_count = 0;
  g_renderer_state.batch.index_count = 0;
#if AR_SIM3D_TERRAIN_ELEVATION
  SimBackgroundVoxelTerrainDepth_Reset();
#endif
}
