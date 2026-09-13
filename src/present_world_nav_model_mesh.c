#include "present_world_nav_model_mesh.h"

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

static bool AppendSource(const WorldNavigationModelSource *source,
    const WorldNavigationModelSourceStyle *style, unsigned pose, bool animated) {
  SimBackgroundVoxelObject object = source->object;
  if (animated) object.animation_phase = (uint8_t)pose;
  const SimBackgroundVoxelBiome biome = SimBackgroundVoxelBiome_ForTown(object.town);
  const SimBackgroundVoxelModelShadingKey light = {
    .light_azimuth_deg = style->light_azimuth,
    .light_elevation_deg = style->light_elevation,
    .shading = kSimBackgroundVoxelShading_AmbientOcclusion, .biome = (uint8_t)biome,
  };
  const SimBackgroundVoxelModelShading *shading = NULL;
  const Sim3DPerformanceScope compile = Sim3DPerformance_Begin(kSim3DPerformance_DepthVoxel);
  const SimBackgroundVoxelModelView *model = SimBackgroundVoxelModelCache_Get(
      &object, source->detail, style->style, style->lighting ? &light : NULL, &shading);
  Sim3DPerformance_End(compile);
  if (!model || model->overflow || !model->face_count ||
      model->face_count > kSimBackgroundVoxelModelMaxFaces || (style->lighting && !shading) ||
      !ReserveSource((size_t)model->face_count * 4)) return false;
  /* Consume this borrowed compiler view completely before the next Get. */
  SimBackgroundVoxelPalette palette;
  SimBackgroundVoxelPalette_Build(&object, biome, &palette);
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object.kind);
  const float source_scale = (float)kSimWorldMapTilePixels / kSimTownCellPixels;
  const float anchor_x = source->source_x + source->centre_x * source_scale;
  const float anchor_y = source->source_y + source->centre_y * source_scale;
  float normal[3], local_scale;
  if (!SimWorldNavigationGlobe_SampleAtRadius(style->chart_radius_tiles,
          anchor_x / kSimWorldMapTilePixels, anchor_y / kSimWorldMapTilePixels,
          normal, &local_scale)) return false;
  const float height_scale = local_scale * style->tile_world / kSimTownCellPixels *
      proportions->height_scale * style->height_percent / (float)kPercentScale;
  for (uint16_t face = 0; face < model->face_count; ++face) {
    const SimBackgroundVoxelModelFace *authored = &model->faces[face];
    const SimBackgroundVoxelMaterial material = style->lighting
        ? (SimBackgroundVoxelMaterial)shading->material[face]
        : SimBackgroundVoxelBiome_SurfaceMaterial(biome, source->detail,
            (SimBackgroundVoxelMaterial)authored->material, authored);
    const uint32_t argb = SimBackgroundVoxelPalette_Base(&palette, material);
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
      const float shade = style->lighting ? .74f + .18f * shading->brightness[face][p] / 255.0f : .88f;
      v->color = (ArRenderColorF){
        ((argb >> 16) & 255) / 255.0f * shade, ((argb >> 8) & 255) / 255.0f * shade,
        (argb & 255) / 255.0f * shade, (argb >> 24) / 255.0f,
      };
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

void WorldNavigationModelMesh_Reset(void) {
  Sim3DDepthPass_DestroyMesh(s_models.mesh);
  free(s_models.vertices);
  free(s_models.resident);
  free(s_models.pending_ranges);
  free(s_models.rejected_sources);
  memset(&s_models, 0, sizeof(s_models));
}
