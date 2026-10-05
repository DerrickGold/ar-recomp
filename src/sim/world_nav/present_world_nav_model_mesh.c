#include "sim/world_nav/present_world_nav_model_mesh.h"
#include "sim/world_nav/present_sim_globe_focus.h"

#include <stdlib.h>
#include <string.h>
#include "constants.h"
#include "host/host_clock.h"
#include "sim/sim3d/sim3d_performance.h"
#include "sim/voxels/sim_background_voxel_biome.h"
#include "sim/voxels/sim_background_voxel_model_cache.h"
#include "sim/voxels/sim_background_voxel_palette.h"
#include "sim/voxels/sim_background_voxel_proportions.h"
#include "sim/world_nav/sim_world_navigation_globe.h"
#include "sim/world_nav/sim_world_navigation_towns.h"
#include "sim/sim3d/sim3d_mesh_set.h"

enum { kMaximumSourceVertices = (16 * 1024 * 1024 / sizeof(Sim3DDepthRadialVertex)) & ~3u };
_Static_assert(kMaximumSourceVertices / 4 <= kSim3DDepthMaximumRadialSourceQuads,
    "world model residency must fit the portable radial publication contract");
typedef struct WorldNavigationResidentModel {
  WorldNavigationModelSource source;
  Sim3DDepthMeshRange range;
} WorldNavigationResidentModel;
enum { kDetailCount = kSimBackgroundVoxelDetail_Ultra + 1 };
typedef enum ModelBuildResult {
  kModelBuild_Ready,
  kModelBuild_Rejected,
  kModelBuild_Limit,
  kModelBuild_ResourceFailure,
} ModelBuildResult;
/* Retry scarce resources at most once per second, even while a view changes.
 * Deterministic content/size rejection remains local to its source selection. */
enum { kModelResourceRetryMs = kMillisecondsPerSecond };
static struct {
  Sim3DDepthMesh *mesh;
  Sim3DDepthRadialVertex *vertices;
  size_t vertex_count, capacity, range_count;
  Sim3DDepthMeshRange *pending_ranges;
  size_t pending_capacity;
  WorldNavigationModelSource *rejected_sources;
  size_t rejected_count, rejected_capacity;
  WorldNavigationResidentModel (*resident)[kDetailCount];
  Sim3DDepthMeshRange ranges[kSimWorldNavigationTownObjectCapacity];
  WorldNavigationModelSourceStyle style;
  uint64_t retry_after_ms;
  ModelBuildResult failure;
  bool checked, enabled, key_ready, source_ready, selection_ready;
} s_models;

enum { kTownModelPoseCount = 4 }; /* static source, then three windmill poses */
static struct {
  Sim3DMeshSet meshes[kTownModelPoseCount];
  WorldNavigationModelSource *sources;
  size_t count, capacity;
  WorldNavigationModelSourceStyle style;
  SimBackgroundVoxelShading shading;
  uint64_t retry_after_ms;
  bool observed, ready, rejected;
} s_town_models;

typedef struct TownSourceBuilder {
  Sim3DDepthLinearVertex *vertices;
  size_t count, capacity;
} TownSourceBuilder;

/* Retain only the current static town's embedded vertices. A construction
 * change must not recompute every unchanged model's curved ground positions.
 * Two buffers alternate so updates also avoid repeatedly allocating/touching
 * several MiB of fresh pages. Each has a 16 MiB retention bound. Larger scenes
 * keep the ordinary full rebuild instead of growing this cache. */
enum { kTownStaticCacheBytes = 16 * 1024 * 1024 };
typedef struct TownCachedSource {
  WorldNavigationModelSource source;
  size_t first, count;
} TownCachedSource;
static struct {
  TownCachedSource *sources;
  Sim3DDepthLinearVertex *vertices;
  size_t count, capacity;
  TownSourceBuilder scratch;
  bool valid;
} s_town_static;

static bool SourceEqual(const WorldNavigationModelSource *a, const WorldNavigationModelSource *b) {
  const SimBackgroundVoxelObject *x = &a->object, *y = &b->object;
  return x->group == y->group && x->kind == y->kind && x->flags == y->flags && x->town == y->town &&
         x->development_level == y->development_level && x->cell_x == y->cell_x &&
         x->cell_y == y->cell_y && x->source_cells_w == y->source_cells_w &&
         x->source_cells_h == y->source_cells_h && x->footprint_cells_w == y->footprint_cells_w &&
         x->footprint_cells_d == y->footprint_cells_d && x->tree_edges == y->tree_edges &&
         x->record_slot == y->record_slot && x->bridge_axis == y->bridge_axis &&
         x->bridge_bank_a_x == y->bridge_bank_a_x && x->bridge_bank_a_y == y->bridge_bank_a_y &&
         x->bridge_bank_b_x == y->bridge_bank_b_x && x->bridge_bank_b_y == y->bridge_bank_b_y &&
         x->animation_phase == y->animation_phase && x->visual_state == y->visual_state &&
         x->visual_metatile == y->visual_metatile && a->detail == b->detail &&
         a->object_index == b->object_index && a->source_x == b->source_x &&
         a->source_y == b->source_y && a->centre_x == b->centre_x && a->centre_y == b->centre_y &&
         a->anchor_height == b->anchor_height && a->depth_height == b->depth_height;
}

static bool SourcesEqual(const WorldNavigationModelSource *a, const WorldNavigationModelSource *b,
                         size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (!SourceEqual(&a[i], &b[i])) return false;
  return true;
}

static bool StyleEqual(const WorldNavigationModelSourceStyle *a,
                       const WorldNavigationModelSourceStyle *b) {
  const Sim3DDepthSurfaceFocus *x = &a->focus, *y = &b->focus;
  return SimGlobeMapping_Equal(&a->embedding, &b->embedding) &&
         a->model_revision == b->model_revision && a->surface_revision == b->surface_revision &&
         a->chart_radius_tiles == b->chart_radius_tiles && a->tile_world == b->tile_world &&
         a->height_percent == b->height_percent && a->light_azimuth == b->light_azimuth &&
         a->light_elevation == b->light_elevation && a->style == b->style &&
         a->lighting == b->lighting && a->captured_poses == b->captured_poses &&
         x->clear_rect.x == y->clear_rect.x && x->clear_rect.y == y->clear_rect.y &&
         x->clear_rect.w == y->clear_rect.w && x->clear_rect.h == y->clear_rect.h &&
         x->feather == y->feather && x->dim == y->dim && x->corner_radius == y->corner_radius &&
         x->inset == y->inset && x->haze.r == y->haze.r && x->haze.g == y->haze.g &&
         x->haze.b == y->haze.b && x->haze.a == y->haze.a;
}

static void ClearTownStatic(void) {
  free(s_town_static.sources);
  free(s_town_static.vertices);
  free(s_town_static.scratch.vertices);
  memset(&s_town_static, 0, sizeof(s_town_static));
}

static void ResetTownModels(void) {
  ClearTownStatic();
  for (unsigned i = 0; i < kTownModelPoseCount; ++i)
    Sim3DMeshSet_Destroy(&s_town_models.meshes[i]);
  free(s_town_models.sources);
  memset(&s_town_models,0,sizeof(s_town_models));
}

typedef struct WorldPreparedModel {
  const SimBackgroundVoxelModelView *model;
  const SimBackgroundVoxelModelShading *shading;
  const SimBackgroundVoxelProportions *proportions;
  SimBackgroundVoxelPalette palette;
  SimBackgroundVoxelBiome biome;
  float height_scale;
} WorldPreparedModel;

/* Borrowed model/shading views must be consumed before the next preparation.
 * Both GPU representations share the compiler, palette and proportions. */
static ModelBuildResult PrepareSourceModel(const WorldNavigationModelSource *source,
                                           const WorldNavigationModelSourceStyle *style,
                                           unsigned pose, SimBackgroundVoxelShading shading,
                                           WorldPreparedModel *out) {
  SimBackgroundVoxelObject object = source->object;
  if (object.kind == kSimBackgroundVoxel_Windmill && !style->captured_poses)
    object.animation_phase = (uint8_t)pose;
  out->biome = SimBackgroundVoxelBiome_ForTown(object.town);
  const SimBackgroundVoxelModelShadingKey light = {
    .light_azimuth_deg = style->light_azimuth, .light_elevation_deg = style->light_elevation,
    .shading = shading, .biome = (uint8_t)out->biome,
  };
  const Sim3DPerformanceScope scope = Sim3DPerformance_Begin(kSim3DPerformance_DepthVoxel);
  out->shading = NULL;
  out->model = SimBackgroundVoxelModelCache_Get(
      &object,source->detail,style->style,style->lighting ? &light : NULL,&out->shading);
  Sim3DPerformance_End(scope);
  if (!out->model) return kModelBuild_ResourceFailure;
  if (out->model->overflow || !out->model->face_count ||
      out->model->face_count > kSimBackgroundVoxelModelMaxFaces)
    return kModelBuild_Rejected;
  if (style->lighting && !out->shading) return kModelBuild_ResourceFailure;
  SimBackgroundVoxelPalette_Build(&object,out->biome,&out->palette);
  out->proportions = SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object.kind);
  const float source_scale = (float)kSimWorldMapTilePixels / kSimTownCellPixels;
  float normal[3], local_scale;
  if (!SimWorldNavigationGlobe_SampleAtRadius(
          style->chart_radius_tiles,
          (source->source_x + source->centre_x * source_scale) / kSimWorldMapTilePixels,
          (source->source_y + source->centre_y * source_scale) / kSimWorldMapTilePixels, normal,
          &local_scale))
    return kModelBuild_Rejected;
  out->height_scale = local_scale * style->tile_world / kSimTownCellPixels *
      out->proportions->height_scale * style->height_percent / (float)kPercentScale;
  return kModelBuild_Ready;
}

static void ClearSources(void) {
  s_models.source_ready = s_models.selection_ready = false;
  s_models.failure = kModelBuild_Ready;
  s_models.vertex_count = 0;
  if (s_models.resident) memset(s_models.resident, 0,
      kSimWorldNavigationTownObjectCapacity * sizeof(*s_models.resident));
}

static void RememberRejectedSources(const WorldNavigationModelSource *sources, size_t count) {
  if (count > s_models.rejected_capacity) {
    void *copy = realloc(s_models.rejected_sources, count * sizeof(*sources));
    if (!copy) {
      s_models.failure = kModelBuild_ResourceFailure;
      return;
    }
    s_models.rejected_sources = copy;
    s_models.rejected_capacity = count;
  }
  memcpy(s_models.rejected_sources, sources, count * sizeof(*sources));
  s_models.rejected_count = count;
}

bool WorldNavigationModelMesh_Enabled(void) {
  if (!s_models.checked) {
    const char *value = getenv("AR_SIM3D_WORLD_GPU_MODELS");
    /* Default game path; explicit opt-out retains the CPU/multicore renderer. */
    s_models.enabled = !value || strcmp(value, "0") != 0;
    s_models.checked = true;
  }
  return s_models.enabled &&
         (!s_models.retry_after_ms || HostClock_Milliseconds() >= s_models.retry_after_ms);
}

bool WorldNavigationModelMesh_Rejected(void) {
  return s_models.failure == kModelBuild_Rejected || s_models.failure == kModelBuild_Limit;
}

static ModelBuildResult ReserveSource(size_t added) {
  if (added > kMaximumSourceVertices - s_models.vertex_count) {
    return kModelBuild_Limit;
  }
  const size_t needed = s_models.vertex_count + added;
  if (needed <= s_models.capacity) return kModelBuild_Ready;
  size_t capacity = s_models.capacity ? s_models.capacity * 2 : 4096;
  if (capacity < needed) capacity = needed;
  if (capacity > kMaximumSourceVertices) capacity = kMaximumSourceVertices;
  void *vertices = realloc(s_models.vertices, capacity * sizeof(*s_models.vertices));
  if (!vertices) return kModelBuild_ResourceFailure;
  s_models.vertices = vertices;
  s_models.capacity = capacity;
  return kModelBuild_Ready;
}

static ArRenderColorF RadialModelColor(uint32_t argb, bool lighting, uint8_t brightness) {
  const float shade = lighting ? .74f + .18f * brightness / 255.0f : .88f;
  return (ArRenderColorF){
    ((argb >> 16) & 255) / 255.0f * shade, ((argb >> 8) & 255) / 255.0f * shade,
    (argb & 255) / 255.0f * shade, (argb >> 24) / 255.0f,
  };
}

static ModelBuildResult AppendSource(const WorldNavigationModelSource *source,
                                     const WorldNavigationModelSourceStyle *style, unsigned pose,
                                     bool animated) {
  WorldPreparedModel prepared;
  ModelBuildResult result = PrepareSourceModel(
      source, style, pose, kSimBackgroundVoxelShading_AmbientOcclusion, &prepared);
  if (result != kModelBuild_Ready) return result;
  result = ReserveSource((size_t)prepared.model->face_count * 4);
  if (result != kModelBuild_Ready) return result;
  const SimBackgroundVoxelModelView *model = prepared.model;
  const SimBackgroundVoxelModelShading *shading = prepared.shading;
  const SimBackgroundVoxelProportions *proportions = prepared.proportions;
  const SimBackgroundVoxelBiome biome = prepared.biome;
  const float source_scale = (float)kSimWorldMapTilePixels / kSimTownCellPixels;
  const float height_scale = prepared.height_scale;
  const float focus = PresentSimGlobeFocus_Weight(&style->focus,
      (source->source_x+source->centre_x*source_scale)/kSimWorldMapPixels,
      (source->source_y+source->centre_y*source_scale)/kSimWorldMapPixels);
  for (uint16_t face = 0; face < model->face_count; ++face) {
    const SimBackgroundVoxelModelFace *authored = &model->faces[face];
    const SimBackgroundVoxelMaterial material = style->lighting
        ? (SimBackgroundVoxelMaterial)shading->material[face]
        : SimBackgroundVoxelBiome_SurfaceMaterial(biome, source->detail,
            (SimBackgroundVoxelMaterial)authored->material, authored);
    const uint32_t argb = SimBackgroundVoxelPalette_Base(&prepared.palette, material);
    for (unsigned p = 0; p < 4; ++p) {
      const SimBackgroundVoxelModelPoint *point = &authored->points[p];
      const float x =
          source->centre_x + (point->x - source->centre_x) * proportions->footprint_scale;
      const float y =
          source->centre_y + (point->y - source->centre_y) * proportions->footprint_scale;
      Sim3DDepthRadialVertex *v = &s_models.vertices[s_models.vertex_count++];
      if (!SimWorldNavigationGlobe_SampleAtRadius(
              style->chart_radius_tiles,
              (source->source_x + x * source_scale) / kSimWorldMapTilePixels,
              (source->source_y + y * source_scale) / kSimWorldMapTilePixels, v->normal, NULL))
        return kModelBuild_Rejected;
      v->elevation[0] = source->anchor_height;
      v->elevation[1] = point->z * height_scale;
      if (style->embedding.town &&
          !SimGlobeMapping_Encode(&style->embedding,
                                  (source->source_x + x * source_scale) / kSimWorldMapTilePixels,
                                  (source->source_y + y * source_scale) / kSimWorldMapTilePixels,
                                  v->elevation[0], v->elevation[1], v->normal, v->elevation))
        return kModelBuild_Rejected;
      v->color = RadialModelColor(argb,style->lighting,
          shading ? shading->brightness[face][p] : 255);
      if (focus>0) v->color=PresentSimGlobeFocus_Color(&style->focus,focus,v->color);
      v->variant = animated ? (float)(pose + 1) : 0;
    }
  }
  return kModelBuild_Ready;
}

bool WorldNavigationModelMesh_Draw(const WorldNavigationModelSource *sources,
    size_t count, const WorldNavigationModelSourceStyle *style,
    const Sim3DDepthRadialTransform *transform) {
  if (!sources || !style || !transform || !count || count > kSimWorldNavigationTownObjectCapacity)
    return false;
  if (s_models.retry_after_ms) {
    if (HostClock_Milliseconds() < s_models.retry_after_ms) return false;
    ClearSources();
    s_models.retry_after_ms = 0;
  }
  const Sim3DPerformanceScope scope = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  const bool same = s_models.key_ready && StyleEqual(style, &s_models.style);
  const bool changed_rejection = WorldNavigationModelMesh_Rejected() && s_models.rejected_sources &&
                                 (count != s_models.rejected_count ||
                                  !SourcesEqual(sources, s_models.rejected_sources, count));
  if (!same || changed_rejection) {
    s_models.style = *style;
    s_models.key_ready = true;
    ClearSources();
  }
  const bool was_rejected = WorldNavigationModelMesh_Rejected();
  /* Fail before compiling any source when the GPU cannot allocate storage. */
  if (!s_models.mesh && s_models.failure == kModelBuild_Ready) {
    s_models.mesh = Sim3DDepthPass_CreateRadialMesh();
    if (!s_models.mesh) s_models.failure = kModelBuild_ResourceFailure;
  }
  if (!s_models.resident && s_models.failure == kModelBuild_Ready) {
    s_models.resident = calloc(kSimWorldNavigationTownObjectCapacity, sizeof(*s_models.resident));
    if (!s_models.resident) s_models.failure = kModelBuild_ResourceFailure;
  }
  if (count > s_models.pending_capacity && s_models.failure == kModelBuild_Ready) {
    void *pending = realloc(s_models.pending_ranges, count * sizeof(*s_models.pending_ranges));
    if (!pending)
      s_models.failure = kModelBuild_ResourceFailure;
    else { s_models.pending_ranges = pending; s_models.pending_capacity = count; }
  }
  Sim3DDepthMeshRange *ranges = s_models.pending_ranges;
  const bool had_history = s_models.vertex_count != 0;
  for (unsigned attempt = 0; attempt < 2 && !was_rejected; ++attempt) {
    for (size_t i = 0; i < count && s_models.failure == kModelBuild_Ready; ++i) {
      if (sources[i].object_index >= kSimWorldNavigationTownObjectCapacity ||
          (unsigned)sources[i].detail >= kDetailCount ||
          sources[i].object.kind >= kSimBackgroundVoxelKindCount) {
        s_models.failure = kModelBuild_Rejected;
        break;
      }
      WorldNavigationResidentModel *entry =
          &s_models.resident[sources[i].object_index][sources[i].detail];
      if (!entry->range.quad_count || !SourceEqual(&entry->source, &sources[i])) {
        const size_t first = s_models.vertex_count;
        const bool animated = sources[i].object.kind == kSimBackgroundVoxel_Windmill;
        for (unsigned pose = 0; pose < (animated ? 3u : 1u); ++pose) {
          s_models.failure = AppendSource(&sources[i], style, pose, animated);
          if (s_models.failure != kModelBuild_Ready) break;
        }
        if (s_models.failure != kModelBuild_Ready) break;
        entry->source = sources[i];
        entry->range = (Sim3DDepthMeshRange){first / 4, (s_models.vertex_count - first) / 4};
        s_models.source_ready = s_models.selection_ready = false;
      }
      ranges[i] = entry->range;
    }
    if (s_models.failure != kModelBuild_Limit || !had_history || attempt) break;
    /* Evict historical LODs once before rejecting an oversized current view. */
    ClearSources();
  }
  bool selection_changed = count != s_models.range_count;
  for (size_t i = 0; s_models.failure == kModelBuild_Ready && !selection_changed && i < count; ++i)
    selection_changed = ranges[i].first_quad != s_models.ranges[i].first_quad ||
                        ranges[i].quad_count != s_models.ranges[i].quad_count;
  if (s_models.failure == kModelBuild_Ready && selection_changed) {
    memcpy(s_models.ranges, ranges, count * sizeof(*ranges));
    s_models.range_count = count;
    s_models.selection_ready = false;
  }
  bool ready = false;
  if (s_models.failure == kModelBuild_Ready) {
    if (!s_models.source_ready || !Sim3DDepthPass_MeshReady(s_models.mesh)) {
      s_models.source_ready = Sim3DDepthPass_UpdateRadialMesh(
          s_models.mesh, s_models.vertices, s_models.vertex_count / 4);
      if (!s_models.source_ready) s_models.failure = kModelBuild_ResourceFailure;
      s_models.selection_ready = false;
      if (s_models.source_ready) Sim3DPerformance_AddPath(kSim3DPath_Publish);
    }
    if (s_models.failure == kModelBuild_Ready && !s_models.selection_ready) {
      s_models.selection_ready = Sim3DDepthPass_SelectRadialMesh(
          s_models.mesh, s_models.ranges, s_models.range_count);
      if (!s_models.selection_ready) s_models.failure = kModelBuild_ResourceFailure;
    }
    if (s_models.failure == kModelBuild_Ready)
      ready = Sim3DDepthPass_AppendRadialMesh(s_models.mesh, transform);
  }
  if (s_models.failure != kModelBuild_Ready) {
    if (WorldNavigationModelMesh_Rejected() && !was_rejected)
      RememberRejectedSources(sources, count);
    if (s_models.failure == kModelBuild_ResourceFailure)
      s_models.retry_after_ms = HostClock_Milliseconds() + kModelResourceRetryMs;
    Sim3DDepthPass_DestroyMesh(s_models.mesh);
    s_models.mesh = NULL;
    s_models.source_ready = s_models.selection_ready = false;
  }
  Sim3DPerformance_AddPath(ready                                   ? kSim3DPath_GpuReuse
                           : s_models.failure == kModelBuild_Limit ? kSim3DPath_Limit
                                                                   : kSim3DPath_Rejected);
  Sim3DPerformance_End(scope);
  return ready;
}

bool WorldNavigationModelMesh_Repeat(const Sim3DDepthRadialTransform *transform) {
  if (!s_models.source_ready || !s_models.selection_ready ||
      s_models.failure != kModelBuild_Ready || !Sim3DDepthPass_MeshReady(s_models.mesh))
    return false;
  const Sim3DPerformanceScope scope = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  const bool ready = Sim3DDepthPass_AppendRadialMesh(s_models.mesh, transform);
  Sim3DPerformance_AddPath(ready ? kSim3DPath_GpuReuse : kSim3DPath_Rejected);
  Sim3DPerformance_End(scope);
  return ready;
}

static ModelBuildResult ReserveTownVertices(TownSourceBuilder *builder, size_t added) {
  if (added > (size_t)kSim3DMeshSetMaximumQuads * 4 - builder->count) return kModelBuild_Limit;
  const size_t needed = builder->count+added;
  if (needed > builder->capacity) {
    size_t capacity = builder->capacity ? builder->capacity*2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > (size_t)kSim3DMeshSetMaximumQuads * 4)
      capacity = (size_t)kSim3DMeshSetMaximumQuads * 4;
    void *vertices = realloc(builder->vertices,capacity*sizeof(*builder->vertices));
    if (!vertices) return kModelBuild_ResourceFailure;
    builder->vertices = vertices;
    builder->capacity = capacity;
  }
  return kModelBuild_Ready;
}

static ModelBuildResult AppendFacingSource(const WorldNavigationModelSource *source,
                                           const WorldNavigationModelSourceStyle *style,
                                           SimBackgroundVoxelShading shading, unsigned pose,
                                           TownSourceBuilder *builder) {
  WorldPreparedModel prepared;
  const bool bridge = source->object.kind == kSimBackgroundVoxel_Bridge;
  ModelBuildResult result =
      PrepareSourceModel(source, style, pose,
                         bridge ? kSimBackgroundVoxelShading_AmbientOcclusion : shading, &prepared);
  if (result != kModelBuild_Ready) return result;
  result = ReserveTownVertices(builder, (size_t)prepared.model->face_count * 4);
  if (result != kModelBuild_Ready) return result;
  const float source_scale = (float)kSimWorldMapTilePixels/kSimTownCellPixels;
  for (unsigned face = 0; face < prepared.model->face_count; ++face) {
    const SimBackgroundVoxelModelFace *authored = &prepared.model->faces[face];
    uint8_t brightness[4] = {255,255,255,255};
    uint8_t material = (uint8_t)SimBackgroundVoxelBiome_SurfaceMaterial(prepared.biome,
        source->detail,(SimBackgroundVoxelMaterial)authored->material,authored);
    if (prepared.shading) {
      material = prepared.shading->material[face];
      memcpy(brightness,prepared.shading->brightness[face],sizeof(brightness));
    }
    ArRenderColorF colors[4];
    SimBackgroundVoxelProject_FaceColors(material,brightness,&prepared.palette,
        style->lighting ? shading : kSimBackgroundVoxelShading_Basic,colors);
    if (bridge) {
      const uint32_t argb = SimBackgroundVoxelPalette_Base(&prepared.palette,material);
      for (unsigned p = 0; p < 4; ++p)
        colors[p] = RadialModelColor(argb,style->lighting,brightness[p]);
    }
    for (unsigned p = 0; p < 4; ++p) {
      const SimBackgroundVoxelModelPoint *point = &authored->points[p];
      const float x =
          source->centre_x + (point->x - source->centre_x) * prepared.proportions->footprint_scale;
      const float y =
          source->centre_y + (point->y - source->centre_y) * prepared.proportions->footprint_scale;
      Sim3DDepthLinearVertex *v = &builder->vertices[builder->count++];
      *v = (Sim3DDepthLinearVertex){.color = colors[p],
          .displacement = point->z*prepared.height_scale,.axis = source->object.kind};
      if (bridge) {
        /* Bake geometric height along the globe normal. The linear stream is
         * used only for its independent depth probe: bridges never billboard.
         * Keep the approach silhouette while protecting the paving from the
         * higher terrain under the rigid footprint. */
        const float chart_x = (source->source_x+x*source_scale)/kSimWorldMapTilePixels;
        const float chart_y = (source->source_y+y*source_scale)/kSimWorldMapTilePixels;
        float safety[3];
        if (!SimGlobeMapping_Point(&style->embedding, chart_x, chart_y, source->anchor_height,
                                   v->displacement, v->position) ||
            !SimGlobeMapping_Point(&style->embedding, chart_x, chart_y, source->depth_height,
                                   v->displacement, safety))
          return kModelBuild_Rejected;
        v->depth_offset = safety[2] - v->position[2];
        v->displacement = 0;
        continue;
      }
      /* The footprint follows the shared globe. Camera-facing height is a
       * separate GPU displacement, never applied to terrain altitude. */
      if (!SimGlobeMapping_Point(&style->embedding,
                                 (source->source_x + x * source_scale) / kSimWorldMapTilePixels,
                                 (source->source_y + y * source_scale) / kSimWorldMapTilePixels,
                                 source->anchor_height, 0, v->position))
        return kModelBuild_Rejected;
    }
  }
  return kModelBuild_Ready;
}

static WorldNavigationModelSource StaticSourceKey(WorldNavigationModelSource source) {
  /* This ordinal addresses the capture, not the geometry. Inserting a model
   * may renumber unchanged neighbours; their semantic identities still match. */
  source.object_index = 0;
  return source;
}

typedef struct TownStaticUpdate {
  TownCachedSource *sources;
  size_t count, dirty_first, dirty_end;
  TownSourceBuilder builder;
} TownStaticUpdate;

typedef struct TownStaticSplice {
  size_t first_quad, removed_quads, inserted_quads;
  bool shifted_tail;
} TownStaticSplice;

static bool TownStaticMatches(const WorldNavigationModelSource *sources, size_t count) {
  size_t at = 0;
  for (size_t i = 0; i < count; ++i) {
    if (sources[i].object.kind == kSimBackgroundVoxel_Windmill) continue;
    const WorldNavigationModelSource key = StaticSourceKey(sources[i]);
    if (at == s_town_static.count || !SourceEqual(&key, &s_town_static.sources[at++].source))
      return false;
  }
  return at == s_town_static.count;
}

/* Plan against the previous immutable CPU image. The update owns the detached
 * working buffer until publication either commits it or discards it. */
static ModelBuildResult BuildTownStaticUpdate(const WorldNavigationModelSource *sources,
                                              size_t count,
                                              const WorldNavigationModelSourceStyle *style,
                                              SimBackgroundVoxelShading shading, bool reuse,
                                              TownStaticUpdate *update) {
  *update = (TownStaticUpdate){.builder = s_town_static.scratch, .dirty_first = SIZE_MAX};
  s_town_static.scratch = (TownSourceBuilder){0};
  update->builder.count = 0;
  for (size_t i = 0; i < count; ++i)
    update->count += sources[i].object.kind != kSimBackgroundVoxel_Windmill;
  update->sources = update->count ? calloc(update->count, sizeof(*update->sources)) : NULL;
  if (update->count && !update->sources) return kModelBuild_ResourceFailure;
  size_t at = 0;
  TownSourceBuilder *builder = &update->builder;
  for (size_t i = 0; i < count; ++i) {
    if (sources[i].object.kind == kSimBackgroundVoxel_Windmill) continue;
    TownCachedSource *entry = &update->sources[at++];
    entry->source = StaticSourceKey(sources[i]);
    entry->first = builder->count;
    const TownCachedSource *cached = NULL;
    for (size_t j = 0; reuse && j < s_town_static.count; ++j) {
      if (SourceEqual(&entry->source, &s_town_static.sources[j].source)) {
        cached = &s_town_static.sources[j];
        break;
      }
    }
    ModelBuildResult result;
    if (cached) {
      result = ReserveTownVertices(builder, cached->count);
      if (result == kModelBuild_Ready && cached->count) {
        memcpy(builder->vertices + builder->count, s_town_static.vertices + cached->first,
               cached->count * sizeof(*builder->vertices));
        builder->count += cached->count;
      }
    } else {
      result = AppendFacingSource(&sources[i], style, shading, 0, builder);
    }
    if (result != kModelBuild_Ready) return result;
    entry->count = builder->count - entry->first;
    /* Equal-size edits dirty only their span; shifts dirty the packed tail. */
    if (entry->count && (!cached || cached->first != entry->first)) {
      if (entry->first < update->dirty_first) update->dirty_first = entry->first;
      update->dirty_end = builder->count;
    }
  }
  if (update->dirty_first == SIZE_MAX) update->dirty_first = update->dirty_end = builder->count;
  return kModelBuild_Ready;
}

static TownStaticSplice PlanTownStaticSplice(const TownStaticUpdate *update) {
  size_t prefix = 0, prefix_vertices = 0, suffix = 0, suffix_vertices = 0;
  while (prefix < update->count && prefix < s_town_static.count &&
         update->sources[prefix].count == s_town_static.sources[prefix].count &&
         SourceEqual(&update->sources[prefix].source, &s_town_static.sources[prefix].source)) {
    prefix_vertices += update->sources[prefix].count;
    ++prefix;
  }
  while (suffix < update->count - prefix && suffix < s_town_static.count - prefix) {
    const TownCachedSource *a = &update->sources[update->count - suffix - 1];
    const TownCachedSource *b = &s_town_static.sources[s_town_static.count - suffix - 1];
    if (a->count != b->count || !SourceEqual(&a->source, &b->source)) break;
    suffix_vertices += a->count;
    ++suffix;
  }
  const size_t old_vertices = s_town_static.count
                                  ? s_town_static.sources[s_town_static.count - 1].first +
                                        s_town_static.sources[s_town_static.count - 1].count
                                  : 0;
  return (TownStaticSplice){
      .first_quad = prefix_vertices / 4,
      .removed_quads = (old_vertices - prefix_vertices - suffix_vertices) / 4,
      .inserted_quads = (update->builder.count - prefix_vertices - suffix_vertices) / 4,
      .shifted_tail = suffix_vertices && old_vertices != update->builder.count};
}

static bool PublishTownStaticUpdate(const TownStaticUpdate *update, bool reuse) {
  Sim3DMeshSet *mesh = &s_town_models.meshes[0];
  const Sim3DDepthLinearVertex *vertices = update->builder.vertices;
  const char *splice_option = getenv("AR_SIM_MODEL_SPLICE");
  if (reuse && (!splice_option || strcmp(splice_option, "0"))) {
    const TownStaticSplice splice = PlanTownStaticSplice(update);
    if (splice.shifted_tail &&
        Sim3DMeshSet_SpliceLinear(mesh,
                                  splice.inserted_quads ? vertices + splice.first_quad * 4 : NULL,
                                  splice.first_quad, splice.removed_quads, splice.inserted_quads))
      return true;
  }
  if (reuse &&
      Sim3DMeshSet_UpdateLinearRange(
          mesh, update->dirty_end > update->dirty_first ? vertices + update->dirty_first : NULL,
          update->dirty_first / 4, (update->dirty_end - update->dirty_first) / 4,
          update->builder.count / 4))
    return true;
  return Sim3DMeshSet_UpdateLinear(mesh, vertices, update->builder.count / 4);
}

static void RetainTownStaticUpdate(TownStaticUpdate *update, bool published) {
  TownSourceBuilder previous = {
      .vertices = s_town_static.vertices, .capacity = s_town_static.capacity};
  s_town_static.vertices = NULL;
  ClearTownStatic();
  if (published &&
      update->builder.capacity <= kTownStaticCacheBytes / sizeof(*update->builder.vertices)) {
    s_town_static.sources = update->sources;
    s_town_static.vertices = update->builder.vertices;
    s_town_static.count = update->count;
    s_town_static.capacity = update->builder.capacity;
    s_town_static.scratch = previous;
    s_town_static.valid = true;
  } else {
    free(update->sources);
    free(update->builder.vertices);
    free(previous.vertices);
  }
}

static ModelBuildResult PublishTownStatic(const WorldNavigationModelSource *sources, size_t count,
                                          const WorldNavigationModelSourceStyle *style,
                                          SimBackgroundVoxelShading shading, bool reuse) {
  reuse = reuse && s_town_static.valid;
  if (reuse && TownStaticMatches(sources, count)) return kModelBuild_Ready;
  TownStaticUpdate update;
  ModelBuildResult result = BuildTownStaticUpdate(sources, count, style, shading, reuse, &update);
  if (result == kModelBuild_Ready && !PublishTownStaticUpdate(&update, reuse))
    result = kModelBuild_ResourceFailure;
  RetainTownStaticUpdate(&update, result == kModelBuild_Ready);
  return result;
}

bool WorldNavigationModelMesh_DrawFacingTown(
    const WorldNavigationModelSource *sources, size_t count,
    const WorldNavigationModelSourceStyle *style, SimBackgroundVoxelShading shading,
    const SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount],
    const float matrix[16], unsigned wind_pose) {
  _Static_assert(kSimBackgroundVoxelKindCount <= kSim3DDepthLinearAxisCount,
      "town facing axes must fit the generic linear displacement contract");
  if (!style || !axes || !matrix || (count && !sources) || wind_pose >= 3 ||
      count > kSimBackgroundMaxObjects ||
      !style->embedding.town ||
      (unsigned)shading >= kSimBackgroundVoxelShading_Count) return false;
  for (size_t i = 0; i < count; ++i)
    if (sources[i].object.town != style->embedding.town ||
        sources[i].object.kind >= kSimBackgroundVoxelKindCount ||
        (unsigned)sources[i].detail >= kDetailCount) return false;
  if (!count) { ResetTownModels(); return true; }
  if (s_town_models.retry_after_ms && HostClock_Milliseconds() < s_town_models.retry_after_ms)
    return false;
  s_town_models.retry_after_ms = 0;
  bool ready = s_town_models.ready;
  for (unsigned pose = 0; pose < kTownModelPoseCount && ready; ++pose)
    ready = Sim3DMeshSet_Ready(&s_town_models.meshes[pose]);
  const bool same_render_style = s_town_models.observed && shading == s_town_models.shading &&
                                 StyleEqual(style, &s_town_models.style);
  const bool same_style = same_render_style && count == s_town_models.count;
  const bool matching = same_style && SourcesEqual(sources, s_town_models.sources, count);
  bool only_captured_motion = same_style && style->captured_poses && !matching;
  for (size_t i = 0; i < count && only_captured_motion; ++i) {
    WorldNavigationModelSource stable = sources[i];
    if (stable.object.kind == kSimBackgroundVoxel_Windmill) {
      stable.object.animation_phase = s_town_models.sources[i].object.animation_phase;
      /* Native spin frames change both phase and metatile ($24/$26/$16).
       * The windmill compiler uses phase/state, not the tile identity. Keep
       * this change in the animated stream instead of rebuilding the static
       * town. State, construction flags and every other source still match. */
      stable.object.visual_metatile = s_town_models.sources[i].object.visual_metatile;
    }
    only_captured_motion = SourceEqual(&stable, &s_town_models.sources[i]);
  }
  if (matching && s_town_models.rejected) return false;
  if (!matching || !ready) {
    const bool motion_update = only_captured_motion && ready;
    s_town_models.ready = false;
    if (count > s_town_models.capacity) {
      void *copy = realloc(s_town_models.sources,count*sizeof(*sources));
      if (!copy) {
        ResetTownModels();
        s_town_models.retry_after_ms = HostClock_Milliseconds() + kModelResourceRetryMs;
        return false;
      }
      s_town_models.sources = copy;
      s_town_models.capacity = count;
    }
    memcpy(s_town_models.sources,sources,count*sizeof(*sources));
    s_town_models.count = count;
    s_town_models.style = *style;
    s_town_models.shading = shading;
    s_town_models.observed = true;
    s_town_models.rejected = false;
    TownSourceBuilder builder = {0};
    ModelBuildResult result = kModelBuild_Ready;
    /* Static buildings never republish for a captured windmill tick. Preserve
     * individual stopped/construction phases in a small independent stream;
     * navigation still prepublishes its three clock-selected pose streams. */
    for (unsigned pose = motion_update ? 1 : 0;
         pose < (motion_update ? 2 : kTownModelPoseCount) && result == kModelBuild_Ready; ++pose) {
      if (!pose) {
        result = PublishTownStatic(sources, count, style, shading, same_render_style && ready);
        continue;
      }
      builder.count = 0;
      for (size_t i = 0; i < count && result == kModelBuild_Ready; ++i) {
        if (style->captured_poses && pose > 1) break;
        const bool windmill = sources[i].object.kind == kSimBackgroundVoxel_Windmill;
        if (windmill != (pose != 0)) continue;
        result = AppendFacingSource(&sources[i], style, shading, pose ? pose - 1 : 0, &builder);
      }
      if (result == kModelBuild_Ready &&
          !Sim3DMeshSet_UpdateLinear(&s_town_models.meshes[pose], builder.vertices,
                                     builder.count / 4))
        result = kModelBuild_ResourceFailure;
    }
    free(builder.vertices);
    if (result != kModelBuild_Ready) {
      s_town_models.rejected = result != kModelBuild_ResourceFailure;
      if (!s_town_models.rejected)
        s_town_models.retry_after_ms = HostClock_Milliseconds() + kModelResourceRetryMs;
      ClearTownStatic();
      for (unsigned pose = 0; pose < kTownModelPoseCount; ++pose)
        Sim3DMeshSet_Destroy(&s_town_models.meshes[pose]);
      return false;
    }
    s_town_models.ready = true;
    s_town_models.rejected = false;
    Sim3DPerformance_AddPath(kSim3DPath_Publish);
  }
  Sim3DDepthLinearTransform transform = {0};
  memcpy(transform.matrix,matrix,sizeof(transform.matrix));
  for (unsigned kind = 0; kind < kSimBackgroundVoxelKindCount; ++kind) {
    transform.axes[kind][0] = axes[kind].x_per_height;
    transform.axes[kind][1] = -axes[kind].y_per_height;
    transform.axes[kind][2] = axes[kind].height_scale;
  }
  Sim3DDepthMesh *meshes[2*kSim3DMeshSetMaximumChunks];
  size_t mesh_count = 0;
  const unsigned selected[2] = {0,style->captured_poses ? 1 : wind_pose+1};
  for (unsigned i = 0; i < 2; ++i) {
    const Sim3DMeshSet *set = &s_town_models.meshes[selected[i]];
    for (size_t m = 0; m < set->count; ++m) meshes[mesh_count++] = set->meshes[m];
  }
  /* Atomic across static + animated sources, including chunked large towns. */
  const bool ok = !mesh_count || Sim3DDepthPass_AppendLinearMeshes(meshes,mesh_count,&transform);
  Sim3DPerformance_AddPath(ok ? kSim3DPath_GpuReuse : kSim3DPath_Rejected);
  return ok;
}

void WorldNavigationModelMesh_Reset(void) {
  ResetTownModels();
  Sim3DDepthPass_DestroyMesh(s_models.mesh);
  free(s_models.vertices);
  free(s_models.resident);
  free(s_models.pending_ranges);
  free(s_models.rejected_sources);
  memset(&s_models, 0, sizeof(s_models));
}
