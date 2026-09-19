#include "present_world_nav_model_mesh.h"
#include "present_sim_globe_focus.h"

#include <stdlib.h>
#include <string.h>
#include "constants.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_background_voxel_biome.h"
#include "sim/sim_background_voxel_model_cache.h"
#include "sim/sim_background_voxel_palette.h"
#include "sim/sim_background_voxel_proportions.h"
#include "sim/sim_world_navigation_globe.h"
#include "sim/sim_world_navigation_towns.h"
#include "sim/sim3d_mesh_set.h"

enum { kMaximumSourceVertices = (16 * 1024 * 1024 / sizeof(Sim3DDepthRadialVertex)) & ~3u };
_Static_assert(kMaximumSourceVertices / 4 <= kSim3DDepthMaximumRadialSourceQuads,
    "world model residency must fit the portable radial publication contract");
typedef struct WorldNavigationResidentModel {
  WorldNavigationModelSource source;
  Sim3DDepthMeshRange range;
} WorldNavigationResidentModel;
enum { kDetailCount = kSimBackgroundVoxelDetail_Ultra + 1 };
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
  bool checked, enabled, unavailable, key_ready, source_ready, selection_ready, rejected, limited;
} s_models;

enum { kTownModelPoseCount = 4 }; /* static source, then three windmill poses */
static struct {
  Sim3DMeshSet meshes[kTownModelPoseCount];
  WorldNavigationModelSource *sources;
  size_t count, capacity;
  WorldNavigationModelSourceStyle style;
  SimBackgroundVoxelShading shading;
  bool observed, ready, rejected;
} s_town_models;

static void ResetTownModels(void) {
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
static bool PrepareSourceModel(const WorldNavigationModelSource *source,
    const WorldNavigationModelSourceStyle *style, unsigned pose,
    SimBackgroundVoxelShading shading, WorldPreparedModel *out) {
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
  if (!out->model || out->model->overflow || !out->model->face_count ||
      out->model->face_count > kSimBackgroundVoxelModelMaxFaces ||
      (style->lighting && !out->shading)) return false;
  SimBackgroundVoxelPalette_Build(&object,out->biome,&out->palette);
  out->proportions = SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object.kind);
  const float source_scale = (float)kSimWorldMapTilePixels / kSimTownCellPixels;
  float normal[3], local_scale;
  if (!SimWorldNavigationGlobe_SampleAtRadius(style->chart_radius_tiles,
          (source->source_x+source->centre_x*source_scale)/kSimWorldMapTilePixels,
          (source->source_y+source->centre_y*source_scale)/kSimWorldMapTilePixels,
          normal,&local_scale)) return false;
  out->height_scale = local_scale * style->tile_world / kSimTownCellPixels *
      out->proportions->height_scale * style->height_percent / (float)kPercentScale;
  return true;
}

static void ClearSources(void) {
  s_models.source_ready = s_models.selection_ready = false;
  s_models.rejected = s_models.limited = false;
  s_models.vertex_count = 0;
  if (s_models.resident) memset(s_models.resident, 0,
      kSimWorldNavigationTownObjectCapacity * sizeof(*s_models.resident));
}

static void RememberRejectedSources(const WorldNavigationModelSource *sources, size_t count) {
  if (count > s_models.rejected_capacity) {
    void *copy = realloc(s_models.rejected_sources, count * sizeof(*sources));
    if (!copy) { s_models.unavailable = true; return; }
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
  return s_models.enabled && !s_models.unavailable;
}

static bool ReserveSource(size_t added) {
  if (added > kMaximumSourceVertices - s_models.vertex_count) {
    s_models.limited = true;
    return false;
  }
  const size_t needed = s_models.vertex_count + added;
  if (needed <= s_models.capacity) return true;
  size_t capacity = s_models.capacity ? s_models.capacity * 2 : 4096;
  if (capacity < needed) capacity = needed;
  if (capacity > kMaximumSourceVertices) capacity = kMaximumSourceVertices;
  void *vertices = realloc(s_models.vertices, capacity * sizeof(*s_models.vertices));
  if (!vertices) return false;
  s_models.vertices = vertices;
  s_models.capacity = capacity;
  return true;
}

static ArRenderColorF RadialModelColor(uint32_t argb, bool lighting, uint8_t brightness) {
  const float shade = lighting ? .74f + .18f * brightness / 255.0f : .88f;
  return (ArRenderColorF){
    ((argb >> 16) & 255) / 255.0f * shade, ((argb >> 8) & 255) / 255.0f * shade,
    (argb & 255) / 255.0f * shade, (argb >> 24) / 255.0f,
  };
}

static bool AppendSource(const WorldNavigationModelSource *source,
    const WorldNavigationModelSourceStyle *style, unsigned pose, bool animated) {
  WorldPreparedModel prepared;
  if (!PrepareSourceModel(source,style,pose,kSimBackgroundVoxelShading_AmbientOcclusion,&prepared) ||
      !ReserveSource((size_t)prepared.model->face_count * 4)) return false;
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
      const float x = source->centre_x + (point->x - source->centre_x) * proportions->footprint_scale;
      const float y = source->centre_y + (point->y - source->centre_y) * proportions->footprint_scale;
      Sim3DDepthRadialVertex *v = &s_models.vertices[s_models.vertex_count++];
      if (!SimWorldNavigationGlobe_SampleAtRadius(style->chart_radius_tiles,
              (source->source_x + x * source_scale) / kSimWorldMapTilePixels,
              (source->source_y + y * source_scale) / kSimWorldMapTilePixels, v->normal, NULL)) return false;
      v->elevation[0] = source->anchor_height;
      v->elevation[1] = point->z * height_scale;
      if (style->embedding.town && !SimGlobeMapping_Encode(&style->embedding,
              (source->source_x+x*source_scale)/kSimWorldMapTilePixels,
              (source->source_y+y*source_scale)/kSimWorldMapTilePixels,
              v->elevation[0], v->elevation[1], v->normal, v->elevation)) return false;
      v->color = RadialModelColor(argb,style->lighting,
          shading ? shading->brightness[face][p] : 255);
      if (focus>0) v->color=PresentSimGlobeFocus_Color(&style->focus,focus,v->color);
      v->variant = animated ? (float)(pose + 1) : 0;
    }
  }
  return true;
}

bool WorldNavigationModelMesh_Draw(const WorldNavigationModelSource *sources,
    size_t count, const WorldNavigationModelSourceStyle *style,
    const Sim3DDepthRadialTransform *transform) {
  if (!sources || !style || !transform || !count || count > kSimWorldNavigationTownObjectCapacity)
    return false;
  const Sim3DPerformanceScope scope = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  const bool same = s_models.key_ready && !memcmp(style, &s_models.style, sizeof(*style));
  const bool changed_rejection = s_models.rejected && s_models.rejected_sources &&
      (count != s_models.rejected_count ||
       memcmp(sources, s_models.rejected_sources, count * sizeof(*sources)));
  if (!same || changed_rejection) {
    s_models.style = *style;
    s_models.key_ready = true;
    ClearSources();
  }
  /* Reject unavailable GPU resources before compiling/copying any source.
   * The normal CPU renderer must not pay for an unused duplicate build. */
  if (!s_models.mesh && !s_models.rejected) {
    s_models.mesh = Sim3DDepthPass_CreateRadialMesh();
    s_models.rejected = !s_models.mesh;
    s_models.unavailable = !s_models.mesh; /* Restore normal held/multicore fallback until reset. */
  }
  if (!s_models.resident && !s_models.rejected) {
    s_models.resident = calloc(kSimWorldNavigationTownObjectCapacity, sizeof(*s_models.resident));
    s_models.rejected = !s_models.resident;
  }
  if (count > s_models.pending_capacity && !s_models.rejected) {
    void *pending = realloc(s_models.pending_ranges, count * sizeof(*s_models.pending_ranges));
    if (!pending) s_models.rejected = true;
    else { s_models.pending_ranges = pending; s_models.pending_capacity = count; }
  }
  Sim3DDepthMeshRange *ranges = s_models.pending_ranges;
  const bool was_rejected = s_models.rejected;
  const bool had_history = s_models.vertex_count != 0;
  for (unsigned attempt = 0; attempt < 2 && !was_rejected; ++attempt) {
    for (size_t i = 0; i < count && !s_models.rejected; ++i) {
      if (sources[i].object_index >= kSimWorldNavigationTownObjectCapacity ||
          (unsigned)sources[i].detail >= kDetailCount) { s_models.rejected = true; break; }
      WorldNavigationResidentModel *entry = &s_models.resident[sources[i].object_index][sources[i].detail];
      if (!entry->range.quad_count || memcmp(&entry->source, &sources[i], sizeof(sources[i]))) {
        const size_t first = s_models.vertex_count;
        const bool animated = sources[i].object.kind == kSimBackgroundVoxel_Windmill;
        for (unsigned pose = 0; pose < (animated ? 3u : 1u); ++pose)
          if (!AppendSource(&sources[i], style, pose, animated)) { s_models.rejected = true; break; }
        if (s_models.rejected) break;
        entry->source = sources[i];
        entry->range = (Sim3DDepthMeshRange){first / 4, (s_models.vertex_count - first) / 4};
        s_models.source_ready = s_models.selection_ready = false;
      }
      ranges[i] = entry->range;
    }
    if (!s_models.limited || !had_history || attempt) break;
    /* The budget bounds residency, not lifetime travel. Discard historical
     * LODs and rebuild only this selection before publishing/queuing anything.
     * Never reduce the requested detail or omit an object to make it fit. */
    ClearSources();
  }
  if (!s_models.rejected && (count != s_models.range_count ||
          memcmp(ranges, s_models.ranges, count * sizeof(*ranges)))) {
    memcpy(s_models.ranges, ranges, count * sizeof(*ranges));
    s_models.range_count = count;
    s_models.selection_ready = false;
  }
  bool ready = false;
  if (!s_models.rejected) {
    if (!s_models.source_ready || !Sim3DDepthPass_MeshReady(s_models.mesh)) {
      s_models.source_ready = Sim3DDepthPass_UpdateRadialMesh(
          s_models.mesh, s_models.vertices, s_models.vertex_count / 4);
      s_models.rejected = !s_models.source_ready;
      s_models.selection_ready = false;
      if (s_models.source_ready) Sim3DPerformance_AddPath(kSim3DPath_Publish);
    }
    if (!s_models.rejected && !s_models.selection_ready) {
      s_models.selection_ready = Sim3DDepthPass_SelectRadialMesh(
          s_models.mesh, s_models.ranges, s_models.range_count);
      s_models.rejected = !s_models.selection_ready;
    }
    if (!s_models.rejected) ready = Sim3DDepthPass_AppendRadialMesh(s_models.mesh, transform);
  }
  Sim3DPerformance_AddPath(ready ? kSim3DPath_GpuReuse :
      s_models.limited ? kSim3DPath_Limit : kSim3DPath_Rejected);
  if (s_models.rejected) {
    /* No sample was queued. Suppress identical failed requests, but keep a
     * content/capacity rejection local to this selection. In particular it
     * must not disable the required SIM embedding after navigation zooms. */
    if (!was_rejected) RememberRejectedSources(sources, count);
    Sim3DDepthPass_DestroyMesh(s_models.mesh);
    s_models.mesh = NULL;
  }
  Sim3DPerformance_End(scope);
  return ready;
}

bool WorldNavigationModelMesh_Repeat(const Sim3DDepthRadialTransform *transform) {
  if (!s_models.source_ready || !s_models.selection_ready || s_models.rejected ||
      !Sim3DDepthPass_MeshReady(s_models.mesh)) return false;
  const Sim3DPerformanceScope scope = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
  const bool ready = Sim3DDepthPass_AppendRadialMesh(s_models.mesh, transform);
  Sim3DPerformance_AddPath(ready ? kSim3DPath_GpuReuse : kSim3DPath_Rejected);
  Sim3DPerformance_End(scope);
  return ready;
}

typedef struct TownSourceBuilder {
  Sim3DDepthLinearVertex *vertices;
  size_t count, capacity;
} TownSourceBuilder;

static bool AppendFacingSource(const WorldNavigationModelSource *source,
    const WorldNavigationModelSourceStyle *style, SimBackgroundVoxelShading shading,
    unsigned pose, TownSourceBuilder *builder) {
  WorldPreparedModel prepared;
  const bool bridge = source->object.kind == kSimBackgroundVoxel_Bridge;
  if (!PrepareSourceModel(source,style,pose,
          bridge ? kSimBackgroundVoxelShading_AmbientOcclusion : shading,&prepared)) return false;
  const size_t added = (size_t)prepared.model->face_count*4;
  if (added > (size_t)kSim3DMeshSetMaximumQuads*4-builder->count) return false;
  const size_t needed = builder->count+added;
  if (needed > builder->capacity) {
    size_t capacity = builder->capacity ? builder->capacity*2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > (size_t)kSim3DMeshSetMaximumQuads*4) capacity = (size_t)kSim3DMeshSetMaximumQuads*4;
    void *vertices = realloc(builder->vertices,capacity*sizeof(*builder->vertices));
    if (!vertices) return false;
    builder->vertices = vertices; builder->capacity = capacity;
  }
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
      const float x = source->centre_x+(point->x-source->centre_x)*prepared.proportions->footprint_scale;
      const float y = source->centre_y+(point->y-source->centre_y)*prepared.proportions->footprint_scale;
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
        if (!SimGlobeMapping_Point(&style->embedding, chart_x, chart_y,
                source->anchor_height, v->displacement, v->position) ||
            !SimGlobeMapping_Point(&style->embedding, chart_x, chart_y,
                source->depth_height, v->displacement, safety)) return false;
        v->depth_offset = safety[2] - v->position[2];
        v->displacement = 0;
        continue;
      }
      /* The footprint follows the shared globe. Camera-facing height is a
       * separate GPU displacement, never applied to terrain altitude. */
      if (!SimGlobeMapping_Point(&style->embedding,
              (source->source_x+x*source_scale)/kSimWorldMapTilePixels,
              (source->source_y+y*source_scale)/kSimWorldMapTilePixels,
              source->anchor_height,0,v->position)) return false;
    }
  }
  return true;
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
  bool ready = s_town_models.ready;
  for (unsigned pose = 0; pose < kTownModelPoseCount && ready; ++pose)
    ready = Sim3DMeshSet_Ready(&s_town_models.meshes[pose]);
  const bool same_style = s_town_models.observed && count == s_town_models.count &&
      shading == s_town_models.shading && !memcmp(style,&s_town_models.style,sizeof(*style));
  const bool matching = same_style && !memcmp(sources,s_town_models.sources,count*sizeof(*sources));
  bool only_captured_motion = same_style && style->captured_poses && !matching;
  for (size_t i = 0; i < count && only_captured_motion; ++i) {
    WorldNavigationModelSource stable = sources[i];
    if (stable.object.kind == kSimBackgroundVoxel_Windmill)
      stable.object.animation_phase = s_town_models.sources[i].object.animation_phase;
    only_captured_motion = !memcmp(&stable,&s_town_models.sources[i],sizeof(stable));
  }
  if (matching && s_town_models.rejected) return false;
  if (!matching || !ready) {
    const bool motion_update = only_captured_motion && ready;
    s_town_models.ready = false;
    if (count > s_town_models.capacity) {
      void *copy = realloc(s_town_models.sources,count*sizeof(*sources));
      if (!copy) { ResetTownModels(); return false; }
      s_town_models.sources = copy; s_town_models.capacity = count;
    }
    memcpy(s_town_models.sources,sources,count*sizeof(*sources));
    s_town_models.count = count; s_town_models.style = *style;
    s_town_models.shading = shading; s_town_models.observed = true;
    s_town_models.rejected = true;
    TownSourceBuilder builder = {0};
    bool ok = true;
    /* Static buildings never republish for a captured windmill tick. Preserve
     * individual stopped/construction phases in a small independent stream;
     * navigation still prepublishes its three clock-selected pose streams. */
    for (unsigned pose = motion_update ? 1 : 0;
         pose < (motion_update ? 2 : kTownModelPoseCount) && ok; ++pose) {
      builder.count = 0;
      for (size_t i = 0; i < count && ok; ++i) {
        if (style->captured_poses && pose > 1) break;
        const bool windmill = sources[i].object.kind == kSimBackgroundVoxel_Windmill;
        if (windmill != (pose != 0)) continue;
        ok = AppendFacingSource(&sources[i],style,shading,pose ? pose-1 : 0,&builder);
      }
      if (ok) ok = Sim3DMeshSet_UpdateLinear(&s_town_models.meshes[pose],builder.vertices,builder.count/4);
    }
    free(builder.vertices);
    if (!ok) {
      for (unsigned pose = 0; pose < kTownModelPoseCount; ++pose)
        Sim3DMeshSet_Destroy(&s_town_models.meshes[pose]);
      return false;
    }
    s_town_models.ready = true; s_town_models.rejected = false;
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
