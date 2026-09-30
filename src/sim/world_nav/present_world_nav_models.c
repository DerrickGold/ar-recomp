/* PresentWorldNav models: the towns' authored models and the mountains in the
 * world view: projection jobs, model quads, viewport culling, and the town and
 * mountain draws.
 * Phase: present (FrameSlot only).
 * Tests: tests/present_world_nav_test.c */
#include "sim/world_nav/present_world_nav_internal.h"
typedef struct WorldNavigationVisibleTownObject {
  const SimWorldNavigationTownObject *object;
  SimBackgroundVoxelDetail detail;
} WorldNavigationVisibleTownObject;

/* Towns use the same authored model compiler, proportions and material
 * palettes as the full-town renderer. Only their projection changes. The
 * common depth pass also hides rear facades and objects behind the terrain. */
SimBackgroundBridgeBounds WorldNavigationObjectBounds(
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
  if (!g_world_nav_models.capturing || !g_world_nav_models.capture_static)
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
  const size_t needed = g_world_nav_models.projected_count + quads * 4;
  size_t maximum_vertices = (8 * 1024 * 1024 / sizeof(Sim3DDepthVertex)) & ~(size_t)3;
#if AR_WORLD_NAV_CACHE_TESTING
  if (g_world_nav_model_test_bytes / sizeof(Sim3DDepthVertex) < maximum_vertices)
    maximum_vertices = (g_world_nav_model_test_bytes / sizeof(Sim3DDepthVertex)) & ~(size_t)3;
#endif
  if (needed > maximum_vertices) {
    g_world_nav_models.capturing = false;
    g_world_nav_models.projection_unavailable =
        true; /* Do not recopy an oversized view every frame. */
    return true;
  }
  if (needed > g_world_nav_models.projected_capacity) {
    size_t capacity =
        g_world_nav_models.projected_capacity ? g_world_nav_models.projected_capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > maximum_vertices) capacity = maximum_vertices;
    void *points =
        realloc(g_world_nav_models.projected, capacity * sizeof(*g_world_nav_models.projected));
    if (!points) {
      g_world_nav_models.capturing = false;
      g_world_nav_models.projection_unavailable = true; /* Retry when the view changes. */
      return true;
    }
    g_world_nav_models.projected = points;
    g_world_nav_models.projected_capacity = capacity;
  }
  memcpy(g_world_nav_models.projected + g_world_nav_models.projected_count, vertices,
      quads * 4 * sizeof(*vertices));
  g_world_nav_models.projected_count = needed;
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
  float focus_gain;
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
          column->stamp = stamp;
          column->x_bits = x_bits;
          column->y_bits = y_bits;
        }
        float world[3];
        WorldNavigationRadialPoint(projection, column->normal, job->base + p->z * job->height_scale,
                                   world);
        Scene3DPoint projected;
        Sim3DDepthVertex *vertex = &out->vertices[point];
        if (!WorldNavigationProjectPoint(projection, work->viewport, world,
                &projected, &vertex->depth, &out->clip[point])) {
          out->valid = false;
          break;
        }
        vertex->x = projected.x;
        vertex->y = projected.y;
        vertex->uv = (ArRenderPointF){-1, -1};
        const float shade = (work->lighting
            ? 0.74f + 0.18f * job->shading.brightness[face][point] / 255.0f : 0.88f) *
            job->focus_gain;
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
  g_world_nav_models.capture_static = !job->animated;
  if (g_world_nav_models.capturing && job->animated) {
    g_world_nav_models.animated[g_world_nav_models.animated_count++] =
        (WorldNavigationAnimatedModel){
          .static_end = (uint32_t)g_world_nav_models.projected_count,
          .object = job->object_index,
          .detail = (uint8_t)job->detail,
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
    if (!SubmitWorldNavigationModelJob(&batch->jobs[i], batch->output, projection, viewport))
      valid = false;
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
  SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, anchor_normal,
                                          transformed_anchor);
  const float camera[3] = {projection->camera_world[0], projection->camera_world[1],
      projection->camera_world[2] + projection->globe_radius_world +
          projection->reference_height_units * projection->height_world_per_unit};
  const float occluder_radius = projection->globe_radius_world * 0.9975f *
      cosf(kPi / kWorldNavigationOceanRings + 2 * kPi / kWorldNavigationOceanSectors);
  const bool occluded = SimWorldNavigationGlobe_CapOccluded(camera, transformed_anchor,
      angular_radius, maximum_radius, occluder_radius);
  if (occluded && object.kind != kSimBackgroundVoxel_Windmill) return true;
  const Sim3DDepthSurfaceFocus focus = PresentWorldNavigationFocus_Resolve(&slot->sim);
  const float focus_weight = PresentSimGlobeFocus_Weight(&focus,
      (source_x + centre_x * pixel_to_world_source) / kSimWorldMapPixels,
      (source_y + centre_y * pixel_to_world_source) / kSimWorldMapPixels);
  WorldNavigationModelJob job = {
    .model = *model, .shading = shading ? *shading : (SimBackgroundVoxelModelShading){0},
    .palette = palette, .biome = biome, .detail = visible->detail,
    .source_x = source_x, .source_y = source_y, .centre_x = centre_x, .centre_y = centre_y,
    .footprint_scale = proportions->footprint_scale, .base = base, .height_scale = height_scale,
    .focus_gain = 1 - focus.dim * focus_weight,
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
      if (!FlushWorldNavigationModels(batch, projection, viewport,
                                      slot->sim.world_navigation_lighting))
        return false;
    job.first = batch->face_count;
    memcpy(batch->faces + job.first, model->faces, job.model.face_count * sizeof(*model->faces));
    job.model.faces = batch->faces + job.first;
    if (shading) {
      memcpy(batch->material + job.first, shading->material, job.model.face_count);
      memcpy(batch->brightness + job.first, shading->brightness,
             job.model.face_count * sizeof(*shading->brightness));
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
  if (g_world_nav_mountains.samples_ready) return true;
  if (g_world_nav_mountains.projection_unavailable) return false;
  const size_t count = g_world_nav_mountains.scene.face_count;
  if (g_world_nav_mountains.projection_capacity < count) {
    void *points = realloc(g_world_nav_mountains.projection,
        count * sizeof(*g_world_nav_mountains.projection));
    if (!points) {
      g_world_nav_mountains.projection_unavailable = true;
      return false; /* Same geometry, uncached path; no per-frame retry. */
    }
    g_world_nav_mountains.projection = points;
    g_world_nav_mountains.projection_capacity = count;
  }
  for (size_t i = 0; i < count; i++) {
    if (!SampleWorldNavigationMountainFace(&g_world_nav_mountains.scene.faces[i],
            projection, &g_world_nav_mountains.projection[i])) return false;
  }
  g_world_nav_mountains.samples_ready = true;
  g_world_nav_mountains.projection_ready = false;
  return true;
}

static bool ProjectWorldNavigationMountainFace(
    const SimWorldNavigationMountainFace *face, const WorldNavigationMountainProjection *sample,
    bool lighting, const Sim3DDepthSurfaceFocus *focus, ArRenderRectI viewport,
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
    vertices[p].x = screen.x;
    vertices[p].y = screen.y;
    vertices[p].uv = (ArRenderPointF){face->uv[p].x, face->uv[p].y};
    vertices[p].color = PresentSimGlobeFocus_Color(focus,
        PresentSimGlobeFocus_Weight(focus, face->x[p] / kSimWorldMapTiles,
            face->y[p] / kSimWorldMapTiles), (ArRenderColorF){shade, shade, shade, 1});
  }
  return true;
}

typedef struct WorldNavigationMountainWork {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  bool lighting;
  Sim3DDepthSurfaceFocus focus;
  const SimWorldNavigationMountainFace *faces;
  WorldNavigationMountainProjection *samples;
} WorldNavigationMountainWork;

static void ProjectWorldNavigationMountainRange(void *context, size_t first, size_t end) {
  WorldNavigationMountainWork *work = context;
  for (size_t i = first; i < end; ++i) {
    WorldNavigationMountainProjection *sample = &work->samples[i];
    sample->visible = ProjectWorldNavigationMountainFace(&work->faces[i], sample,
        work->lighting, &work->focus, work->viewport, &work->projection, sample->points,
        sample->clip);
  }
}

bool DrawWorldNavigationMountains(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!g_world_nav_mountains.active) return true;
  const bool cached = PrepareWorldNavigationMountainSamples(projection);
  WorldNavigationMountainProjectionKey key;
  memset(&key, 0, sizeof(key));
  key.projection = *projection;
  key.width = viewport.w;
  key.height = viewport.h;
  key.lighting = slot->sim.world_navigation_lighting;
  key.focus = PresentWorldNavigationFocus_Resolve(&slot->sim);
  const bool project = !cached || !g_world_nav_mountains.projection_ready ||
      memcmp(&key, &g_world_nav_mountains.projection_key, sizeof(key));
  if (cached && project) {
    WorldNavigationMountainWork work = {.projection = *projection, .viewport = viewport,
      .lighting = slot->sim.world_navigation_lighting, .focus = key.focus,
      .faces = g_world_nav_mountains.scene.faces,
      .samples = g_world_nav_mountains.projection};
    HostParallelWork_Run(WorldNavigationWorkers(), g_world_nav_mountains.scene.face_count, 512,
        ProjectWorldNavigationMountainRange, &work);
  }
  Sim3DDepthVertex batch[64 * 4];
  Scene3DClipPoint clip[64 * 4];
  size_t count = 0;
  for (size_t at = 0; at < g_world_nav_mountains.scene.face_count; at++) {
    const SimWorldNavigationMountainFace *face = &g_world_nav_mountains.scene.faces[at];
    Sim3DDepthVertex *vertices = batch + count * 4;
    bool valid;
    if (cached) {
      WorldNavigationMountainProjection *sample = &g_world_nav_mountains.projection[at];
      valid = sample->visible;
      if (valid) memcpy(vertices, sample->points, sizeof(sample->points));
      if (valid && projection->clip_frustum)
        memcpy(clip + count * 4, sample->clip, sizeof(sample->clip));
    } else {
      WorldNavigationMountainProjection sample;
      valid = SampleWorldNavigationMountainFace(face, projection, &sample) &&
          ProjectWorldNavigationMountainFace(face, &sample, slot->sim.world_navigation_lighting,
              &key.focus, viewport, projection, vertices, clip + count * 4);
    }
    if (valid && ++count == 64) {
      if (!WorldNavigationAppendProjectedQuads(kSim3DDepthPass_WorldMountain, batch,
              projection->clip_frustum ? clip : NULL, count, viewport))
        return false;
      count = 0;
    }
  }
  if (cached) {
    g_world_nav_mountains.projection_key = key;
    g_world_nav_mountains.projection_ready = true;
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
  const WorldNavigationModelBounds *bound = &g_world_nav_models.bounds[object_index];
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
  g_world_nav_models.gpu_current_ready = false;
  if (count > g_world_nav_models.gpu_source_capacity) {
    void *sources =
        realloc(g_world_nav_models.gpu_sources, count * sizeof(*g_world_nav_models.gpu_sources));
    if (!sources) { g_world_nav_models.gpu_sources_unavailable = true; return false; }
    g_world_nav_models.gpu_sources = sources;
    g_world_nav_models.gpu_source_capacity = count;
  }
  WorldNavigationModelSource *sources = g_world_nav_models.gpu_sources;
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
                                                anchor_x / kSimWorldMapTilePixels,
                                                anchor_y / kSimWorldMapTilePixels, normal, &metric))
      return false;
    SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, normal, normal);
    const WorldNavigationModelBounds *bound = &g_world_nav_models.bounds[
        object - slot->sim.world_navigation_towns.objects];
    const float maximum_radius = projection->globe_radius_world +
        source.anchor_height * projection->height_world_per_unit +
        bound->maximum_rise * metric * projection->tile_world * slot->sim.height_scale_x100 /
            kPercentScale;
    /* These cached bounds include every authored pose/LOD. Keep this cheap
     * whole-object test on the CPU; uncertain/partially visible faces go GPU. */
    if (bound->angular_radius > 0 &&
        SimWorldNavigationGlobe_CapOccluded(camera, normal, bound->angular_radius, maximum_radius,
                                            occluder))
      continue;
    sources[source_count++] = source;
  }
  if (!source_count) return true;
  WorldNavigationModelSourceStyle style;
  memset(&style, 0, sizeof(style));
  style.model_revision = g_world_nav_models.revision;
  style.surface_revision = g_world_nav_terrain.cliff_serial;
  style.chart_radius_tiles = projection->chart_radius_tiles;
  style.tile_world = projection->tile_world;
  style.height_percent = slot->sim.height_scale_x100;
  style.light_azimuth = slot->sim.light_azimuth_deg;
  style.light_elevation = slot->sim.light_elevation_deg;
  style.style = (SimBackgroundVoxelStyle)slot->sim.background_voxel_style;
  style.lighting = slot->sim.world_navigation_lighting;
  style.focus = PresentWorldNavigationFocus_Resolve(&slot->sim);
  const Sim3DDepthRadialTransform transform = WorldNavigationRadialTransform(slot, projection);
  g_world_nav_models.gpu_current_ready =
      WorldNavigationModelMesh_Draw(sources, source_count, &style, &transform);
  return g_world_nav_models.gpu_current_ready;
}

bool DrawWorldNavigationTowns(
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
  key.model_revision = g_world_nav_models.revision;
  key.surface_revision = g_world_nav_terrain.cliff_serial;
  key.height_scale = slot->sim.height_scale_x100;
  key.light_azimuth = slot->sim.light_azimuth_deg;
  key.light_elevation = slot->sim.light_elevation_deg;
  key.lighting = slot->sim.world_navigation_lighting;
  key.focus = PresentWorldNavigationFocus_Resolve(&slot->sim);
  const bool same_projection = g_world_nav_models.projection_key_ready &&
      !memcmp(&key, &g_world_nav_models.projection_key, sizeof(key));
  if (!same_projection) {
    g_world_nav_models.projection_unavailable = false;
    g_world_nav_models.solid_mesh_unavailable = false;
  }
  /* A declined selection still gets the held CPU/multicore cache. Retry GPU
   * selection on a changed view, not every frame of the same oversized view. */
  const bool gpu_models = WorldNavigationModelMesh_Enabled() &&
      !g_world_nav_models.gpu_sources_unavailable &&
      !(same_projection && g_world_nav_models.gpu_current_rejected);
  if (gpu_models && same_projection && g_world_nav_models.gpu_current_ready) {
    const Sim3DDepthRadialTransform transform = WorldNavigationRadialTransform(slot, projection);
    if (WorldNavigationModelMesh_Repeat(&transform)) return true;
  }
  g_world_nav_models.gpu_current_ready = false;
  if (!gpu_models && g_world_nav_models.projected_valid && same_projection) {
    /* Bound draw-call amplification, not model count or visual quality.
     * Many interleaved windmills can turn one ordinary batch into hundreds
     * of small draws; those views keep the existing projected CPU cache. */
    enum { kMaximumRetainedStaticSpans = 16 };
    unsigned static_spans = 0;
    size_t previous = 0;
    for (unsigned i = 0; i <= g_world_nav_models.animated_count; ++i) {
      const size_t end = i < g_world_nav_models.animated_count
          ? g_world_nav_models.animated[i].static_end : g_world_nav_models.projected_count;
      if (end > previous && ++static_spans > kMaximumRetainedStaticSpans) break;
      previous = end;
    }
    const bool retain = static_spans <= kMaximumRetainedStaticSpans;
    /* Retain the existing clipped/facing/projection result byte-for-byte;
     * moving views still use the unchanged multicore path. No saved setting
     * or frame/runner contract is extended. Zero is a diagnostic opt-out. */
    if (!g_world_nav_models.solid_mesh_attempted) {
      const char *enabled = getenv("AR_SIM3D_RETAINED_SOLIDS");
      g_world_nav_models.solid_mesh_attempted = true;
      g_world_nav_models.solid_mesh_opt_out = enabled && strcmp(enabled, "0") == 0;
      g_world_nav_models.solid_mesh_unavailable = g_world_nav_models.solid_mesh_opt_out;
    }
    if (!retain) Sim3DPerformance_AddPath(kSim3DPath_Limit);
    if (g_world_nav_models.solid_mesh_unavailable)
      Sim3DPerformance_AddPath(g_world_nav_models.solid_mesh_opt_out ? kSim3DPath_OptOut
                                                                 : kSim3DPath_Rejected);
    if (retain && !g_world_nav_models.solid_mesh_unavailable &&
        g_world_nav_models.projected_count) {
      const Sim3DPerformanceScope publication =
          Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
      if (!g_world_nav_models.solid_mesh)
        g_world_nav_models.solid_mesh = Sim3DDepthPass_CreateGeometryMesh();
      if (!g_world_nav_models.solid_mesh) {
        g_world_nav_models.solid_mesh_unavailable = true;
        Sim3DPerformance_AddPath(kSim3DPath_Rejected);
      }
      else if (!g_world_nav_models.solid_mesh_published ||
          !Sim3DDepthPass_MeshReady(g_world_nav_models.solid_mesh)) {
        g_world_nav_models.solid_mesh_published = Sim3DDepthPass_UpdateGeometryMesh(
            g_world_nav_models.solid_mesh, g_world_nav_models.projected,
            g_world_nav_models.projected_count / 4);
        if (!g_world_nav_models.solid_mesh_published)
          g_world_nav_models.solid_mesh_unavailable = true;
        Sim3DPerformance_AddPath(g_world_nav_models.solid_mesh_published ? kSim3DPath_Publish
                                                                     : kSim3DPath_Rejected);
      }
      Sim3DPerformance_End(publication);
    }
    size_t first = 0;
    for (unsigned i = 0; i <= g_world_nav_models.animated_count; ++i) {
      const size_t end = i < g_world_nav_models.animated_count
          ? g_world_nav_models.animated[i].static_end : g_world_nav_models.projected_count;
      const Sim3DPerformanceScope project = Sim3DPerformance_Begin(kSim3DPerformance_DepthProject);
      bool ready = true;
      if (first != end) {
        const bool can_reuse = retain && !g_world_nav_models.solid_mesh_unavailable &&
            g_world_nav_models.solid_mesh_published;
        const bool reused = can_reuse && Sim3DDepthPass_AppendGeometryMeshRange(
            kSim3DDepthPass_Solid, g_world_nav_models.solid_mesh, first / 4, (end - first) / 4);
        Sim3DPerformance_AddPath(reused ? kSim3DPath_GpuReuse : kSim3DPath_CpuStage);
        if (can_reuse && !reused) Sim3DPerformance_AddPath(kSim3DPath_Rejected);
        ready = reused || Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Solid,
            g_world_nav_models.projected + first, (end - first) / 4);
      }
      Sim3DPerformance_End(project);
      if (!ready) return false;
      if (i < g_world_nav_models.animated_count) {
        const WorldNavigationAnimatedModel *animated = &g_world_nav_models.animated[i];
        const WorldNavigationVisibleTownObject visible = {
          .object = &towns->objects[animated->object],
          .detail = (SimBackgroundVoxelDetail)animated->detail,
        };
        if (!WorldNavigationAppendAuthoredModel(slot, viewport, projection, &visible, NULL))
          return false;
      }
      first = end;
    }
    return true;
  }
  g_world_nav_models.projected_valid = false;
  g_world_nav_models.solid_mesh_published = false;
  g_world_nav_models.projected_count = 0;
  g_world_nav_models.animated_count = 0;
  /* Do not stage an extra copy on every frame of continuous camera motion.
   * A second matching view warms the cache; later held frames replay it. */
  g_world_nav_models.capturing =
      !gpu_models && same_projection && !g_world_nav_models.projection_unavailable;
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
    g_world_nav_models.capturing = false;
    g_world_nav_models.projected_valid = true;
    g_world_nav_models.projection_key_ready = true;
    g_world_nav_models.projection_key = key;
    return true;
  }
  /* Several towns share the same authored compiler/cache. Leave headroom for
   * LOD and animated variants instead of evicting the next frame's working
   * set while iterating this one. Failure only reduces cache effectiveness. */
  (void)SimBackgroundVoxelModelCache_Reserve((uint32_t)visible_count * 2);
  if (gpu_models) {
    g_world_nav_models.gpu_current_rejected =
        !DrawWorldNavigationGpuModels(slot, projection, visible, (size_t)visible_count);
    if (!g_world_nav_models.gpu_current_rejected) {
      g_world_nav_models.projection_key = key;
      g_world_nav_models.projection_key_ready = true;
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
  if (!FlushWorldNavigationModels(batch, projection, viewport, slot->sim.world_navigation_lighting))
    valid = false;
  g_world_nav_models.projected_valid = valid && g_world_nav_models.capturing;
  g_world_nav_models.capturing = false;
  g_world_nav_models.capture_static = false;
  g_world_nav_models.projection_key = key;
  g_world_nav_models.projection_key_ready = true;
  return valid;
}

void ResetWorldNavigationModels(void) {
  WorldNavigationModelMesh_Reset();
  free(g_world_nav_models.gpu_sources);
  g_world_nav_models.gpu_sources = NULL;
  g_world_nav_models.gpu_source_capacity = 0;
  g_world_nav_models.gpu_sources_unavailable = false;
  g_world_nav_models.gpu_current_ready = false;
  g_world_nav_models.gpu_current_rejected = false;
  free(s_world_model_batch);
  s_world_model_batch = NULL;
  s_world_model_batch_unavailable = false;
  free(g_world_nav_models.projected);
  Sim3DDepthPass_DestroyMesh(g_world_nav_models.solid_mesh);
  g_world_nav_models.solid_mesh = NULL;
  g_world_nav_models.solid_mesh_published = g_world_nav_models.solid_mesh_unavailable =
      false;
  g_world_nav_models.solid_mesh_attempted = false;
  g_world_nav_models.solid_mesh_opt_out = false;
  g_world_nav_models.projected = NULL;
  g_world_nav_models.projected_count = g_world_nav_models.projected_capacity = 0;
  g_world_nav_models.projected_valid = g_world_nav_models.capturing = false;
  g_world_nav_models.capture_static = false;
  g_world_nav_models.animated_count = 0;
  g_world_nav_models.projection_key_ready = false;
  g_world_nav_models.projection_unavailable = false;
  g_world_nav_models.object_count = 0;
  g_world_nav_models.detail = g_world_nav_models.style = -1;
  g_world_nav_models.maximum_rise = 0.0f;
}
