/* PresentWorldNav ground: draws the world's ground: the GPU terrain grid, the
 * atmosphere shell and space backdrop, ground and cliff layers, the
 * compatibility ground path, and the light treatment and active-region haze
 * over it.
 * Phase: present (FrameSlot only).
 * Tests: tests/present_world_nav_test.c */
#include "sim/world_nav/present_world_nav_internal.h"

typedef struct WorldNavigationGroundWork {
  const WorldNavigationGroundSample *samples;
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  float light[3];
  bool lighting;
  Sim3DDepthSurfaceFocus focus;
  ArRenderVertex2D *vertices;
  Sim3DDepthVertex *depth;
  Scene3DClipPoint *clip;
  uint8_t *outside;
  bool *valid;
} WorldNavigationGroundWork;

bool WorldNavigationGpuGridEnabled(void) {
  if (!g_world_nav_gpu_grid.attempted) {
    const char *enabled = getenv("AR_SIM3D_WORLD_GPU_GRID");
    /* Normal GPU world path; retain one explicit compatibility opt-out. */
    g_world_nav_gpu_grid.enabled = !enabled || strcmp(enabled, "0");
    const char *cull = getenv("AR_SIM3D_WORLD_GPU_GRID_CULL");
    /* Reduce offscreen GPU work by default within the source-grid path;
     * keep an opt-out for backend comparisons and memory-constrained hosts. */
    g_world_nav_gpu_grid.cull = !cull || strcmp(cull,"0");
    g_world_nav_gpu_grid.attempted = true;
  }
  return g_world_nav_gpu_grid.enabled;
}

static bool DrawWorldNavigationGpuGrid(const FrameSlot *slot,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms) {
  if (!WorldNavigationGpuGridEnabled()) return false;
  if (g_world_nav_gpu_grid.unavailable) {
    if (!memcmp(&g_world_nav_gpu_grid.rejected_projection, projection, sizeof(*projection)) &&
        g_world_nav_gpu_grid.rejected_geography == SimWorldMap_GeographySerial() &&
        g_world_nav_gpu_grid.rejected_cliffs == g_world_nav_terrain.cliff_serial &&
        g_world_nav_gpu_grid.rejected_mountains == g_world_nav_mountains.geometry_revision)
      return false;
    g_world_nav_gpu_grid.unavailable = false;
  }
  if (!PrepareWorldNavigationGroundSamples(projection)) goto unavailable;
  /* Avoid camera-scale floating noise in the source key. Above the radius
   * floor the ratio is exactly a scene setting divided by chart radius. */
  const float ratio = projection->globe_radius_world > .25f
      ? (slot->sim.world_navigation_relief ? slot->sim.landscape_height_pct / (float)kPercentScale
                                           : 0) /
          projection->chart_radius_tiles
      : projection->height_world_per_unit / projection->globe_radius_world;
  if (!g_world_nav_gpu_grid.meshes.count) {
    Sim3DDepthPass_DestroyMesh(g_world_nav_surfaces.mesh);
    g_world_nav_surfaces.mesh = NULL;
    g_world_nav_surfaces.published = false;
  }
  if (!g_world_nav_gpu_grid.ready || !Sim3DMeshSet_Ready(&g_world_nav_gpu_grid.meshes) ||
      memcmp(&g_world_nav_gpu_grid.key, &g_world_nav_terrain.sample_key,
             sizeof(g_world_nav_gpu_grid.key)) ||
      g_world_nav_gpu_grid.mountain_revision != g_world_nav_mountains.geometry_revision ||
      ratio != g_world_nav_gpu_grid.height_ratio) {
    const size_t cliff_count = g_world_nav_terrain.cliffs.face_count;
    const size_t mountain_count =
        g_world_nav_mountains.active ? g_world_nav_mountains.scene.face_count : 0;
    size_t grid_capacity = kWorldNavigationOceanQuads;
    for (size_t i = 0; i < kWorldNavigationTerrainCells*kWorldNavigationTerrainCells; ++i)
      grid_capacity += !g_world_nav_terrain.cliffs.replacement[i];
    if (cliff_count > kWorldNavigationSurfaceMaximumQuads-grid_capacity) goto unavailable;
    if (mountain_count > kWorldNavigationSurfaceMaximumQuads - grid_capacity - cliff_count)
      goto unavailable;
    const size_t capacity = grid_capacity+cliff_count+mountain_count;
    const size_t authored_count = cliff_count+mountain_count;
    Sim3DDepthSurfaceVertex *points = malloc(kWorldNavigationTerrainVertexCount*sizeof(*points));
    Sim3DDepthSurfaceVertex *quads = malloc(capacity*4*sizeof(*quads));
    ArRenderPointF *mask_uv = malloc(capacity*4*sizeof(*mask_uv));
    bool *valid = authored_count ? malloc(authored_count*sizeof(*valid)) : NULL;
    if (!points || !quads || !mask_uv || (authored_count && !valid)) {
      free(points);
      free(quads);
      free(mask_uv);
      free(valid);
      goto unavailable;
    }
    WorldNavigationGridSourceWork work = {g_world_nav_terrain.samples, points, ratio};
    HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainVertexCount, 2048,
        BuildWorldNavigationGridSourceRange, &work);
    if (!BuildWorldNavigationOceanSource(quads,mask_uv)) {
      free(points);
      free(quads);
      free(mask_uv);
      free(valid);
      goto unavailable;
    }
    size_t count = kWorldNavigationOceanQuads;
    g_world_nav_gpu_grid.chunks[0].range = (Sim3DDepthMeshRange){0,count};
    unsigned chunk = 1;
    for (int cy = 0; cy < kWorldNavigationTerrainCells; cy += kWorldNavigationGridChunkCells)
      for (int cx = 0; cx < kWorldNavigationTerrainCells;
           cx += kWorldNavigationGridChunkCells, ++chunk) {
        const size_t first = count;
        for (int y = cy; y < cy+kWorldNavigationGridChunkCells; ++y)
          for (int x = cx; x < cx+kWorldNavigationGridChunkCells; ++x) {
            if (g_world_nav_terrain.cliffs.replacement[y*kWorldNavigationTerrainCells+x]) continue;
            const int at = WorldNavigationTerrainVertexIndex(x,y);
            const int corners[4] = { at, at + 1, at + kWorldNavigationTerrainAxis + 1,
                                     at + kWorldNavigationTerrainAxis };
            for (int p = 0; p < 4; ++p) {
              const Sim3DDepthSurfaceVertex *v = &points[corners[p]];
              quads[count*4+p] = *v;
              mask_uv[count*4+p] = v->uv;
            }
            ++count;
          }
        g_world_nav_gpu_grid.chunks[chunk].bounds =
            WorldNavigationSourceBounds(quads + first * 4, count - first);
        g_world_nav_gpu_grid.chunks[chunk].range = (Sim3DDepthMeshRange){first,count-first};
      }
    if (authored_count) PrepareWorldNavigationTerrain();
    WorldNavigationCliffSourceWork cliffs = {g_world_nav_terrain.cliffs.faces,
      quads+count*4,mask_uv+count*4,valid,projection->chart_radius_tiles,ratio};
    HostParallelWork_Run(WorldNavigationWorkers(), cliff_count, 256,
                         BuildWorldNavigationCliffSourceRange, &cliffs);
    bool ok = true;
    for (size_t i = 0; i < cliff_count; ++i) ok &= valid[i];
    g_world_nav_gpu_grid.chunks[chunk].bounds =
        WorldNavigationSourceBounds(quads + count * 4, cliff_count);
    g_world_nav_gpu_grid.chunks[chunk].range = (Sim3DDepthMeshRange){count,cliff_count};
    count += cliff_count;
    WorldNavigationMountainSourceWork mountains = { g_world_nav_mountains.scene.faces,
                                                    quads + count * 4, mask_uv + count * 4,
                                                    valid ? valid + cliff_count : NULL,
                                                    projection->chart_radius_tiles };
    HostParallelWork_Run(WorldNavigationWorkers(),mountain_count,512,
        BuildWorldNavigationMountainSourceRange,&mountains);
    for (size_t i = cliff_count; i < authored_count; ++i) ok &= valid[i];
    g_world_nav_gpu_grid.chunks[++chunk].range = (Sim3DDepthMeshRange){count,mountain_count};
    count += mountain_count;
    ok = ok && Sim3DMeshSet_UpdateSurface(&g_world_nav_gpu_grid.meshes,quads,mask_uv,count);
    free(points);
    free(quads);
    free(mask_uv);
    free(valid);
    if (!ok) goto unavailable;
    g_world_nav_gpu_grid.key = g_world_nav_terrain.sample_key;
    g_world_nav_gpu_grid.height_ratio = ratio;
    g_world_nav_gpu_grid.ready = true;
    g_world_nav_gpu_grid.source_quads = g_world_nav_gpu_grid.selected_quads = count;
    g_world_nav_gpu_grid.mountain_quads = mountain_count;
    g_world_nav_gpu_grid.mountain_revision = g_world_nav_mountains.geometry_revision;
    g_world_nav_gpu_grid.selection_ready = false;
    g_world_nav_gpu_grid.cull_unavailable = false;
    Sim3DPerformance_AddPath(kSim3DPath_Publish);
  }
  Sim3DDepthSurfaceTransform t = {
    .radial = { .sphere_radius = projection->globe_radius_world,
                .reference_height = projection->reference_height_units,
                .height_scale = projection->height_world_per_unit },
    .ambient = slot->sim.world_navigation_lighting ? kWorldNavigationTerrainAmbient : 1,
    .diffuse = slot->sim.world_navigation_lighting ? 1 - kWorldNavigationTerrainAmbient : 0,
    .focus = PresentWorldNavigationFocus_Resolve(&slot->sim)
  };
  memcpy(t.radial.matrix, projection->matrix, sizeof(t.radial.matrix));
  memcpy(t.radial.basis[0], projection->globe_frame.right, sizeof(t.radial.basis[0]));
  memcpy(t.radial.basis[1], projection->globe_frame.up, sizeof(t.radial.basis[1]));
  memcpy(t.radial.basis[2], projection->globe_frame.outward, sizeof(t.radial.basis[2]));
  if (g_world_nav_gpu_grid.cull && !g_world_nav_gpu_grid.cull_unavailable &&
      (!g_world_nav_gpu_grid.selection_ready ||
       memcmp(&g_world_nav_gpu_grid.selection_transform, &t.radial, sizeof(t.radial)))) {
    Sim3DDepthMeshRange ranges[(kWorldNavigationSurfaceChunks+1)/2] = {0};
    size_t count = 0;
    bool culled = false;
    for (unsigned i = 0; i < kWorldNavigationSurfaceChunks; ++i) {
      const Sim3DDepthMeshRange r = g_world_nav_gpu_grid.chunks[i].range;
      if (!r.quad_count) continue;
      /* Ocean is camera-local. Mountains have an independent extra-rise
       * scale absent from these land bounds. Keep both conservatively and
       * let hardware clip/depth reject them, including eye-plane crossings. */
      if (i && i != kWorldNavigationSurfaceChunks-1 &&
          WorldNavigationRadialBoundsOutside(&g_world_nav_gpu_grid.chunks[i].bounds,&t.radial)) {
        culled = true;
        continue;
      }
      if (count && ranges[count-1].first_quad+ranges[count-1].quad_count == r.first_quad)
        ranges[count-1].quad_count += r.quad_count;
      else ranges[count++] = r;
    }
    /* A single zero-length range explicitly selects nothing; NULL/zero means
     * the whole source. Adjacent chunks merge without adding draw calls. */
    const bool ok = Sim3DMeshSet_SelectSurface(&g_world_nav_gpu_grid.meshes,
        culled ? ranges : NULL, culled ? (count ? count : 1) : 0);
    if (!ok) {
      if (!Sim3DMeshSet_Ready(&g_world_nav_gpu_grid.meshes)) goto unavailable;
      g_world_nav_gpu_grid.cull_unavailable = true;
      Sim3DPerformance_AddPath(kSim3DPath_Rejected);
      fprintf(stderr,"[world-navigation] GPU grid selection unavailable; drawing full source\n");
    }
    g_world_nav_gpu_grid.selected_quads = g_world_nav_gpu_grid.source_quads;
    if (ok && culled) {
      g_world_nav_gpu_grid.selected_quads = 0;
      for (size_t i = 0; i < count; ++i)
        g_world_nav_gpu_grid.selected_quads += ranges[i].quad_count;
    }
    g_world_nav_gpu_grid.selection_transform = t.radial;
    g_world_nav_gpu_grid.selection_ready = true;
  }
  const float azimuth = slot->sim.light_azimuth_deg * kPi / 180,
              elevation = slot->sim.light_elevation_deg * kPi / 180;
  const float light[3] = { -cosf(azimuth) * cosf(elevation), -sinf(azimuth) * cosf(elevation),
                           sinf(elevation) };
  for (int axis = 0; axis < 3; ++axis)
    t.light[axis] = t.radial.basis[0][axis] * light[0] + t.radial.basis[1][axis] * light[1] +
        t.radial.basis[2][axis] * light[2];
  Sim3DDepthSphericalSample shadows[kSimCloudLayerCount*3];
  const size_t shadow_count = WorldNavigationShadowSamples(slot, elapsed_ms, shadows);
  if (shadow_count && !EnsureWorldNavigationCloudTexture()) goto unavailable;
  Sim3DDepthSurfaceOverlay overlays[2];
  size_t overlay_count = 0;
  if (slot->sim.world_navigation_haze) {
    const SimWorldNavigationScene *scene = &slot->sim.world_navigation_scene;
    const Sim3DDepthSurfaceOverlay mask = {
      .clear_rect = { scene->active_region_x / (float)kSimWorldMapPixels,
                      scene->active_region_y / (float)kSimWorldMapPixels,
                      scene->active_region_width / (float)kSimWorldMapPixels,
                      scene->active_region_height / (float)kSimWorldMapPixels },
      .feather = scene->active_region_valid
          ? fmaxf(1, slot->sim.cull_haze_lead_px * .5f) / kSimWorldMapPixels
          : 0,
    };
    if (slot->sim.underlay_defocus_pct) {
      if (!EnsureWorldNavigationBlur(slot)) goto unavailable;
      overlays[overlay_count] = mask;
      overlays[overlay_count].layer = kSim3DDepthPass_GroundBlur;
      overlays[overlay_count++].color =
          (ArRenderColorF){ 1, 1, 1, slot->sim.underlay_defocus_pct / (float)kPercentScale };
    }
    if (slot->sim.underlay_haze_pct) {
      overlays[overlay_count] = mask;
      overlays[overlay_count].layer = kSim3DDepthPass_GroundHaze;
      overlays[overlay_count++].color =
          (ArRenderColorF){ .24f, .37f, .56f,
                            slot->sim.underlay_haze_pct / (float)kPercentScale * .35f };
    }
  }
  const size_t mountain_first =
      g_world_nav_gpu_grid.selected_quads - g_world_nav_gpu_grid.mountain_quads;
  Sim3DDepthSurfaceBatch batches[3] = {
    { .layer = kSim3DDepthPass_Ground,
      .range = { 0, kWorldNavigationOceanQuads },
      .shadows = shadows,
      .shadow_count = shadow_count },
    { .layer = kSim3DDepthPass_Ground,
      .range = { kWorldNavigationOceanQuads, mountain_first - kWorldNavigationOceanQuads },
      .transform = t,
      .shadows = shadows,
      .shadow_count = shadow_count,
      .overlays = overlays,
      .overlay_count = overlay_count },
    { .layer = kSim3DDepthPass_WorldMountain,
      .range = { mountain_first, g_world_nav_gpu_grid.mountain_quads },
      .transform = { .radial = t.radial,
                     .extra_scale = projection->tile_world,
                     .ambient = slot->sim.world_navigation_lighting ? .90f : 1,
                     .focus = t.focus } }
  };
  if (!WorldNavigationOceanTransform(projection,&batches[0].transform)) goto unavailable;
  /* The uncharted ocean shares the distant ground's dimming. Otherwise the
   * chart's water darkens while the surrounding sphere stays full-bright. */
  batches[0].transform.ambient *= 1 - t.focus.dim;
  if (!Sim3DMeshSet_AppendSurface(&g_world_nav_gpu_grid.meshes,batches,3))
    goto unavailable;
  Sim3DPerformance_AddPath(kSim3DPath_GpuReuse);
  return true;
unavailable:
  Sim3DMeshSet_Destroy(&g_world_nav_gpu_grid.meshes);
  g_world_nav_gpu_grid.rejected_projection = *projection;
  g_world_nav_gpu_grid.rejected_geography = SimWorldMap_GeographySerial();
  g_world_nav_gpu_grid.rejected_cliffs = g_world_nav_terrain.cliff_serial;
  g_world_nav_gpu_grid.rejected_mountains = g_world_nav_mountains.geometry_revision;
  g_world_nav_gpu_grid.unavailable = true;
  g_world_nav_gpu_grid.ready = false;
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
        radial_height += (sample->height[p] - projection->reference_height_units) *
            projection->height_world_per_unit;
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
    work->depth[at].x = output.x;
    work->depth[at].y = output.y;
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
    const ArRenderPointF uv = work->vertices[at].tex_coord;
    work->vertices[at].color = PresentSimGlobeFocus_Color(&work->focus,
        PresentSimGlobeFocus_Weight(&work->focus, uv.x, uv.y), work->vertices[at].color);
  }
}

void PrepareWorldNavigationOceanIndices(void) {
  if (g_world_nav_shells.indices_ready) return;
  int index_count = 0;
  for (int sector = 0; sector < kWorldNavigationOceanSectors; sector++) {
    const float longitude = 2.0f * kPi * sector / kWorldNavigationOceanSectors;
    g_world_nav_shells.longitude_cos[sector] = cosf(longitude);
    g_world_nav_shells.longitude_sin[sector] = sinf(longitude);
    const int next = (sector + 1) % kWorldNavigationOceanSectors;
    g_world_nav_shells.indices[index_count++] = 0;
    g_world_nav_shells.indices[index_count++] = 1 + sector;
    g_world_nav_shells.indices[index_count++] = 1 + next;
  }
  for (int ring = 1; ring < kWorldNavigationOceanRings; ring++) {
    const int inner = 1 + (ring - 1) * kWorldNavigationOceanSectors;
    const int outer = inner + kWorldNavigationOceanSectors;
    for (int sector = 0; sector < kWorldNavigationOceanSectors; sector++) {
      const int next = (sector + 1) % kWorldNavigationOceanSectors;
      g_world_nav_shells.indices[index_count++] = inner + sector;
      g_world_nav_shells.indices[index_count++] = outer + sector;
      g_world_nav_shells.indices[index_count++] = outer + next;
      g_world_nav_shells.indices[index_count++] = inner + sector;
      g_world_nav_shells.indices[index_count++] = outer + next;
      g_world_nav_shells.indices[index_count++] = inner + next;
    }
  }
  g_world_nav_shells.indices_ready = true;
}


/* Ocean closes the entire sphere, including the uncharted hemisphere.
 * The atmospheric silhouette is an exact camera-tangent cap on a larger
 * concentric sphere. It is background color, not an opaque occluder. */
static bool PrepareWorldNavigationSphereShell(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection, WorldNavigationShell kind,
    WorldNavigationShellGeometry *geometry) {
  const bool occlusion =
      kind == kWorldNavigationShell_Atmosphere && WorldNavigationGpuGridEnabled();
  if (geometry->ready && geometry->occlusion == occlusion &&
      !memcmp(&geometry->projection, projection, sizeof(*projection)) &&
      !memcmp(&geometry->viewport, &viewport, sizeof(viewport))) return true;
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  geometry->ready = false;
  const bool atmosphere = kind == kWorldNavigationShell_Atmosphere;
  const bool cloud = kind == kWorldNavigationShell_Cloud;
  if (atmosphere) {
    g_world_nav_shells.atmosphere_draw.ready = g_world_nav_shells.atmosphere_draw.repeated = false;
    g_world_nav_shells.atmosphere_draw.unavailable = false;
    g_world_nav_shells.atmosphere_draw.quad_count = 0;
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
  if (!WorldNavigationShellFrame(projection, radius, centre_z, outward, right, up, &eye_distance))
    return false;
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
      const float cx = g_world_nav_shells.longitude_cos[sector];
      const float sy = g_world_nav_shells.longitude_sin[sector];
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
        g_world_nav_shells.vertices[vertex_count] =
            (ArRenderVertex2D){{output.x, output.y}, colour, {0, 0}};
      ++vertex_count;
    }
  }
  if (atmosphere && !projection->clip_frustum)
    for (int i = geometry->first_index; i < kWorldNavigationOceanIndexCount; ++i)
      g_world_nav_shells.atmosphere_indices[i - geometry->first_index] =
          g_world_nav_shells.indices[i] - geometry->first_vertex;
  geometry->projection = *projection;
  geometry->viewport = viewport;
  geometry->ready = true;
  return true;
}

/* Memoize the existing clipped quad stream on a repeated view only. Camera
 * motion keeps the bounded scratch path; allocation/size failures likewise
 * fall back to drawing it. Inputs are copied, never borrowed from a renderer. */
static void CacheWorldNavigationAtmosphereBatch(const ArRenderVertex2D *vertices, size_t quads) {
  WorldNavigationAtmosphereDrawCache *cache = &g_world_nav_shells.atmosphere_draw;
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
  free(cache->vertices);
  free(cache->indices);
  cache->vertices = NULL;
  cache->indices = NULL;
  cache->quad_count = cache->capacity = 0;
}

/* Shell geometry is immutable between camera/viewport changes. Each kind
 * owns its snapshot, so drawing clouds cannot overwrite the ocean receiver
 * or the atmospheric backdrop. Wind UVs and effect opacity remain dynamic. */
bool DrawWorldNavigationSphereShell(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection, WorldNavigationShell kind, float ocean_gain) {
  const bool atmosphere = kind == kWorldNavigationShell_Atmosphere;
  const bool cloud = kind == kWorldNavigationShell_Cloud;
  WorldNavigationShellGeometry *geometry = atmosphere ? &g_world_nav_shells.atmosphere
      : cloud ? &g_world_nav_shells.cloud : &g_world_nav_shells.ocean;
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
      WorldNavigationAtmosphereDrawCache *cache = &g_world_nav_shells.atmosphere_draw;
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
          const int at = g_world_nav_shells.indices[i + (p < 3 ? p : 2)];
          input[p] = depth_vertices[at];
          clip[p] = clip_vertices[at];
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
        g_world_nav_shells.vertices + geometry->first_vertex,
        kWorldNavigationOceanVertexCount - geometry->first_vertex,
        g_world_nav_shells.atmosphere_indices,
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
          ArRenderColorF *color = &batch[batch_count * 4 + p].color;
          color->r *= ocean_gain;
          color->g *= ocean_gain;
          color->b *= ocean_gain;
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
      triangle[corner] = depth_vertices[g_world_nav_shells.indices[i + corner]];
      triangle[corner].color.r *= ocean_gain;
      triangle[corner].color.g *= ocean_gain;
      triangle[corner].color.b *= ocean_gain;
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
bool DrawWorldNavigationSpaceBackdrop(ArRenderRectI viewport) {
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
      if (g_world_nav_terrain.cliffs.replacement[y * kWorldNavigationTerrainCells + x])
        continue;
      const int at = WorldNavigationTerrainVertexIndex(x, y);
      const int corners[4] = {at, at + 1, at + kWorldNavigationTerrainAxis + 1,
                             at + kWorldNavigationTerrainAxis};
      /* The same conservative edge test already guards weather receivers.
       * Reject before attribute staging; retain every straddling face. */
      if (g_world_nav_terrain.outside[corners[0]] & g_world_nav_terrain.outside[corners[1]] &
          g_world_nav_terrain.outside[corners[2]] & g_world_nav_terrain.outside[corners[3]])
        continue;
      Sim3DDepthVertex *face = batch + count * 4;
      for (int i = 0; i < 4; i++) {
        const int vertex = corners[i];
        face[i] = g_world_nav_terrain.depth[vertex];
        face[i].color = colours[vertex].color;
        face[i].uv = layer == kSim3DDepthPass_GroundHaze
            ? (ArRenderPointF){-1.0f, -1.0f} : colours[vertex].tex_coord;
        if (projection->clip_frustum) clip[count * 4 + i] = g_world_nav_terrain.clip[vertex];
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
  for (size_t i = 0; i < g_world_nav_terrain.cliffs.face_count; i++) {
    const SimWorldNavigationCliffFace *face = &g_world_nav_terrain.cliffs.faces[i];
    const WorldNavigationCliffProjection *projected = &g_world_nav_terrain.cliff_projection[i];
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
  key.focus = PresentWorldNavigationFocus_Resolve(&slot->sim);
  return key;
}

static bool DrawWorldNavigationCompatibilityGround(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!slot->sim.world_navigation_scene.valid || slot->visible_width <= 0 || slot->snes_height <= 0)
    return false;
  if (projection->height_world_per_unit > 0.0f) PrepareWorldNavigationTerrain();
  const WorldNavigationGroundKey key = WorldNavigationGroundKeyFor(slot, viewport, projection);
  if (!g_world_nav_terrain.projection_ready ||
      memcmp(&key, &g_world_nav_terrain.projection_key, sizeof(key)) != 0) {
    Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
    g_world_nav_terrain.projection_ready = false;
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
        .samples = g_world_nav_terrain.samples, .projection = *projection, .viewport = viewport,
        .light = {light[0], light[1], light[2]}, .lighting = slot->sim.world_navigation_lighting,
        .focus = key.focus,
        .vertices = g_world_nav_terrain.vertices, .depth = g_world_nav_terrain.depth,
        .clip = g_world_nav_terrain.clip, .outside = g_world_nav_terrain.outside, .valid = valid,
      };
      HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainVertexCount, 2048,
          ProjectWorldNavigationGroundRange, &work);
      for (int i = 0; i < kWorldNavigationTerrainVertexCount; ++i) if (!valid[i]) return false;
    }
    for (size_t i = 0; i < g_world_nav_terrain.cliffs.face_count; i++) {
      const SimWorldNavigationCliffFace *face = &g_world_nav_terrain.cliffs.faces[i];
      WorldNavigationCliffProjection *projected = &g_world_nav_terrain.cliff_projection[i];
      for (int p = 0; p < 4; p++) {
        float normal[3], world[3];
        Scene3DPoint screen;
        SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, projected->normal[p],
                                                normal);
        WorldNavigationRadialPoint(projection, normal,
                                   (face->height[p] - projection->reference_height_units) *
                                       projection->height_world_per_unit,
                                   world);
        if (!WorldNavigationProjectPoint(projection, viewport, world,
                &screen, &projected->depth[p].depth, &projected->clip[p])) return false;
        projected->depth[p].x = screen.x;
        projected->depth[p].y = screen.y;
        projected->outside[p] = projection->clip_frustum
            ? WorldNavigationClipOutside(projected->clip[p])
            : WorldNavigationViewportOutside(screen.x, screen.y, viewport.w, viewport.h);
        const float shade = face->shade *
            WorldNavigationSurfaceShade(slot->sim.world_navigation_lighting, projection, light,
                                        face->x[p] * kSimWorldMapTilePixels,
                                        face->y[p] * kSimWorldMapTilePixels);
        projected->colour[p] = PresentSimGlobeFocus_Color(&key.focus,
            PresentSimGlobeFocus_Weight(&key.focus, face->x[p] / kSimWorldMapTiles,
                face->y[p] / kSimWorldMapTiles), (ArRenderColorF){shade, shade, shade, 1});
      }
    }
    g_world_nav_terrain.projection_key = key;
    g_world_nav_terrain.projection_ready = true;
  }
  return WorldNavigationAppendGroundLayer(
      kSim3DDepthPass_Ground, g_world_nav_terrain.vertices, projection, viewport) &&
      WorldNavigationAppendCliffLayer(kSim3DDepthPass_Ground, slot, projection, viewport);
}

bool DrawWorldNavigationSurfaceLayers(const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms) {
  bool ok = Sim3DDepthPass_Begin(&g_render_device, viewport.w, viewport.h, kArRenderFilter_Linear);
  if (!ok) return false;
  if (g_world_nav_art.displayed_version >= 0 &&
      !Sim3DDepthPass_SelectAtlasVersion(g_world_nav_art.atlas_cache,
          (unsigned)g_world_nav_art.displayed_version)) return false;
  const Sim3DPerformanceScope source = Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
  const bool gpu = DrawWorldNavigationGpuGrid(slot,projection,elapsed_ms);
  Sim3DPerformance_End(source);
  if (g_world_nav_gpu_grid.drawn != gpu) g_world_nav_terrain.projection_ready = false;
  g_world_nav_gpu_grid.drawn = gpu;
  if (gpu) {
    /* All opaque surfaces and their permitted depth-matched consumers are
     * queued atomically. Cutout holes remain holes, with no CPU receivers. */
    return true;
  }
  const Sim3DPerformanceScope ocean = Sim3DPerformance_Begin(kSim3DPerformance_WorldOcean);
  const WorldNavigationGroundKey key = WorldNavigationGroundKeyFor(slot, viewport, projection);
  const bool repeated = g_world_nav_surfaces.key_ready &&
      g_world_nav_surfaces.cliff_serial == g_world_nav_terrain.cliff_serial &&
      g_world_nav_surfaces.mountain_revision == g_world_nav_mountains.geometry_revision &&
      !memcmp(&key, &g_world_nav_surfaces.retained_key, sizeof(key));
  if (!g_world_nav_surfaces.attempted) {
    const char *enabled = getenv("AR_SIM3D_RETAINED_GROUND");
    g_world_nav_surfaces.attempted = true;
    /* Same shader/geometry as ordinary submission; keep a startup opt-out
     * for driver diagnosis without changing saved graphics quality. */
    g_world_nav_surfaces.opt_out = enabled && strcmp(enabled, "0") == 0;
    g_world_nav_surfaces.unavailable = g_world_nav_surfaces.opt_out;
  }
  /* Preserve ocean THEN mainland/cliff order within Ground, plus the separate
   * WorldMountain material. The adapter's atomic append queues all ranges or
   * none, so resource pressure cannot duplicate only half of the surfaces.
   * CPU arrays remain valid for exact-depth weather/haze consumers. */
  const bool retained = ok && !g_world_nav_surfaces.unavailable && repeated &&
      g_world_nav_surfaces.published && g_world_nav_terrain.projection_ready &&
      g_world_nav_shells.ocean.ready && Sim3DDepthPass_MeshReady(g_world_nav_surfaces.mesh) &&
      Sim3DDepthPass_AppendGeometryRanges(g_world_nav_surfaces.mesh, g_world_nav_surfaces.ranges,
                                          2);
  if (!retained) {
    Sim3DPerformance_AddPath(kSim3DPath_CpuStage);
    if (g_world_nav_surfaces.unavailable)
      Sim3DPerformance_AddPath(g_world_nav_surfaces.opt_out ? kSim3DPath_OptOut
                                                            : kSim3DPath_Rejected);
    g_world_nav_surfaces.published = false;
    ok = ok && DrawWorldNavigationSphereShell(viewport, projection, kWorldNavigationShell_Ocean,
        1 - PresentWorldNavigationFocus_Resolve(&slot->sim).dim);
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
  if (ok && (!retained || !g_world_nav_surfaces.mountains_retained)) {
    const Sim3DPerformanceScope mountain = Sim3DPerformance_Begin(kSim3DPerformance_DepthMountain);
    ok = DrawWorldNavigationMountains(slot, viewport, projection);
    Sim3DPerformance_End(mountain);
  }
  if (ok && !retained && repeated && !g_world_nav_surfaces.unavailable) {
    const Sim3DPerformanceScope publication = Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
    const Sim3DDepthPassLayer layers[] = {kSim3DDepthPass_Ground, kSim3DDepthPass_WorldMountain};
    if (!g_world_nav_surfaces.mesh) g_world_nav_surfaces.mesh = Sim3DDepthPass_CreateGeometryMesh();
    g_world_nav_surfaces.mountains_retained = g_world_nav_surfaces.mesh &&
        Sim3DDepthPass_CaptureGeometryLayers(g_world_nav_surfaces.mesh, layers, 2,
                                             g_world_nav_surfaces.ranges);
    g_world_nav_surfaces.published = g_world_nav_surfaces.mountains_retained;
    if (!g_world_nav_surfaces.published && g_world_nav_surfaces.mesh) {
      /* An unusually large cutout set must not evict the old ground-only
       * optimization. Both captures reject before modifying queued draws. */
      g_world_nav_surfaces.ranges[1] =
          (Sim3DDepthGeometryRange){ .layer = kSim3DDepthPass_WorldMountain };
      g_world_nav_surfaces.published = Sim3DDepthPass_CaptureGeometryLayers(
          g_world_nav_surfaces.mesh, layers, 1, g_world_nav_surfaces.ranges);
    }
    if (!g_world_nav_surfaces.published) g_world_nav_surfaces.unavailable = true;
    Sim3DPerformance_AddPath(g_world_nav_surfaces.published ? kSim3DPath_Publish
                                                            : kSim3DPath_Rejected);
    Sim3DPerformance_End(publication);
  }
  g_world_nav_surfaces.key_ready = ok;
  g_world_nav_surfaces.retained_key = key;
  g_world_nav_surfaces.cliff_serial = g_world_nav_terrain.cliff_serial;
  g_world_nav_surfaces.mountain_revision = g_world_nav_mountains.geometry_revision;
  return ok;
}


bool DrawWorldNavigationLightTreatment(
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
bool DrawWorldNavigationActiveRegionHaze(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection) {
  if (!slot->sim.world_navigation_haze ||
      (!slot->sim.underlay_defocus_pct && !slot->sim.underlay_haze_pct))
    return true;
  if (g_world_nav_gpu_grid.drawn) return true;
  /* The atmosphere must share every terrain vertex. The former sparse
   * rectangular overlay cut across the curved ocean and left large straight
   * edges, which made the sphere look like a map pasted onto a blue ball. */
  static ArRenderVertex2D vertices[kWorldNavigationTerrainVertexCount];
  static float haze_samples[kWorldNavigationTerrainVertexCount];
  const float lead = fmaxf(1.0f, slot->sim.cull_haze_lead_px * 0.5f);
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++) {
    vertices[i] = g_world_nav_terrain.vertices[i];
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
       !WorldNavigationAppendGroundLayer(kSim3DDepthPass_GroundBlur, vertices, projection,
                                         viewport) ||
       !WorldNavigationAppendCliffLayer(kSim3DDepthPass_GroundBlur, slot, projection, viewport)))
    return false;
  if (!slot->sim.underlay_haze_pct) return true;
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++) {
    const float haze = haze_samples[i];
    vertices[i].color = (ArRenderColorF){
      0.24f, 0.37f, 0.56f,
      g_world_nav_terrain.vertices[i].color.a * haze *
          slot->sim.underlay_haze_pct / (float)kPercentScale * 0.35f,
    };
  }
  return WorldNavigationAppendGroundLayer(kSim3DDepthPass_GroundHaze, vertices, projection,
                                          viewport) &&
      WorldNavigationAppendCliffLayer(kSim3DDepthPass_GroundHaze, slot, projection, viewport);
}
