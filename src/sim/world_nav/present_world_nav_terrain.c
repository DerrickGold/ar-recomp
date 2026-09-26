/* PresentWorldNav terrain: the world terrain as the presenter sees it:
 * heights (including elevated towns), cliffs, and the surface geometry every
 * layer shares: normals, shading, radial points, ground samples, and the grid,
 * cliff, mountain and ocean source ranges.
 * Phase: present (FrameSlot only).
 * Tests: tests/present_world_nav_test.c */
#include "sim/world_nav/present_world_nav_internal.h"


int WorldNavigationTerrainVertexIndex(int tile_x, int tile_y) {
  return tile_y * kWorldNavigationTerrainAxis + tile_x;
}

static float WorldNavigationSmoothstep(float value) {
  if (value <= 0.0f) return 0.0f;
  if (value >= 1.0f) return 1.0f;
  return value * value * (3.0f - 2.0f * value);
}

void EnsureWorldNavigationCliffs(const FrameSlot *slot, float radius_tiles) {
  uint8_t mask = 0;
#if AR_SIM3D_TERRAIN_ELEVATION
  if (slot->sim.world_navigation_ground_detail && slot->sim.world_navigation_relief &&
      slot->sim.landscape_height_pct && !g_world_nav_art.unavailable &&
      SimTownGroundArt_Available())
    mask = slot->sim.world_navigation_towns.ground.enabled_town_mask;
#endif
  const uint32_t geography = SimWorldMap_GeographySerial();
  if (g_world_nav_terrain.cliffs_ready && g_world_nav_terrain.cliff_mask == mask &&
      g_world_nav_terrain.cliff_chart_radius_tiles == radius_tiles &&
      g_world_nav_terrain.cliff_geography == geography && (!mask || g_world_nav_terrain.ready))
    return;
  if (mask) PrepareWorldNavigationTerrain();
  /* The atmosphere bound must restore exactly when detailed cliffs turn off,
   * including a high owned corner absent from the shared overview vertices. */
  g_world_nav_terrain.maximum_height = 0;
  for (int i = 0; i < kWorldNavigationTerrainVertexCount; i++)
    g_world_nav_terrain.maximum_height = fmaxf(
        g_world_nav_terrain.maximum_height, g_world_nav_terrain.height[i]);
  SimWorldNavigationCliffs_Destroy(&g_world_nav_terrain.cliffs);
  free(g_world_nav_terrain.cliff_projection);
  g_world_nav_terrain.cliff_projection = NULL;
  /* Validate the same immutable variants required by the native overlay.
   * Geometry must not activate over a failed native-art allocation. */
  for (uint8_t town = 1; town <= kSimTownCount; town++)
    if ((mask & (1u << (town - 1))) && !SimTownGroundArt_Metatile(town,
            slot->sim.world_navigation_towns.ground.development_tier[town - 1], 8)) {
      mask = 0;
      break;
    }
  if (mask && SimWorldNavigationCliffs_Build(mask, &g_world_nav_terrain.cliffs) &&
      g_world_nav_terrain.cliffs.face_count) {
    g_world_nav_terrain.cliff_projection = calloc(g_world_nav_terrain.cliffs.face_count,
        sizeof(*g_world_nav_terrain.cliff_projection));
    if (!g_world_nav_terrain.cliff_projection)
      SimWorldNavigationCliffs_Destroy(&g_world_nav_terrain.cliffs);
  }
  for (size_t i = 0; i < g_world_nav_terrain.cliffs.face_count; i++)
    for (int p = 0; p < 4; p++) {
      const SimWorldNavigationCliffFace *face = &g_world_nav_terrain.cliffs.faces[i];
      SimWorldNavigationGlobe_SampleAtRadius(radius_tiles, face->x[p], face->y[p],
          g_world_nav_terrain.cliff_projection[i].normal[p], NULL);
      g_world_nav_terrain.maximum_height = fmaxf(
          g_world_nav_terrain.maximum_height, face->height[p]);
    }
  g_world_nav_terrain.cliff_mask = mask;
  g_world_nav_terrain.cliff_geography = geography;
  g_world_nav_terrain.cliff_chart_radius_tiles = radius_tiles;
  g_world_nav_terrain.cliffs_ready = true;
  g_world_nav_terrain.cliff_serial++;
  g_world_nav_terrain.projection_ready = false;
  g_world_nav_mountains.samples_ready = false;
  g_world_nav_mountains.projection_ready = false;
}

typedef struct WorldNavigationTerrainWork {
  float *height, *floor, *authored, *maximum;
} WorldNavigationTerrainWork;

static void BuildWorldNavigationTerrainRows(void *context, size_t first, size_t end) {
  WorldNavigationTerrainWork *work = context;
  for (int y = (int)first; y < (int)end; y++) {
    work->maximum[y] = 0;
    for (int x = 0; x <= kWorldNavigationTerrainCells; x++) {
      SimWorldNavigationTerrainHeights sample;
      (void)SimWorldNavigationTerrain_SampleHeights((float)x, (float)y, &sample);
      const int at = WorldNavigationTerrainVertexIndex(x, y);
#if AR_SIM3D_TERRAIN_ELEVATION
      work->height[at] = sample.height_units;
      work->floor[at] = sample.floor_height_units;
      work->authored[at] = sample.authored_weight;
#else
      work->height[at] = 0.0f;
      work->floor[at] = 0.0f;
      work->authored[at] = 0.0f;
#endif
      work->maximum[y] = fmaxf(work->maximum[y], work->height[at]);
    }
  }
}

void PrepareWorldNavigationTerrain(void) {
  const uint32_t world_serial = SimWorldMap_GeographySerial();
  if (g_world_nav_terrain.ready && g_world_nav_terrain.serial == world_serial) return;
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_TerrainPrepare);
  g_world_nav_terrain.samples_ready = false;
  /* Prior publication is owner-only. All following samples read immutable
   * world/town inputs, and each job writes disjoint rows. */
  (void)SimWorldNavigationTerrain_RebuildWorldPrior();
  float maximum[kWorldNavigationTerrainAxis];
  WorldNavigationTerrainWork work = {g_world_nav_terrain.height, g_world_nav_terrain.floor,
      g_world_nav_terrain.authored, maximum};
  HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainAxis, 16,
      BuildWorldNavigationTerrainRows, &work);
  g_world_nav_terrain.maximum_height = 0;
  for (int y = 0; y < kWorldNavigationTerrainAxis; ++y)
    g_world_nav_terrain.maximum_height = fmaxf(g_world_nav_terrain.maximum_height, maximum[y]);
  g_world_nav_terrain.ready = true;
  g_world_nav_terrain.serial = world_serial;
  PerformanceMetrics_End(performance);
}

/* Read-only after owner preparation; source-building workers share the frozen
 * height/cap arrays only until their bounded fork/join completes. */
void ResetWorldNavigationTerrain(void) {
  g_world_nav_terrain.samples_ready = false;
  SimWorldNavigationCliffs_Destroy(&g_world_nav_terrain.cliffs);
  free(g_world_nav_terrain.cliff_projection);
  g_world_nav_terrain.cliff_projection = NULL;
  g_world_nav_terrain.cliffs_ready = false;
  SimWorldNavigationTerrain_SetMountainReplacement(NULL);
  SimWorldNavigationTerrain_SetMountainTransition(NULL);
  SimWorldNavigationTerrain_SetMountainJoin(NULL, NULL, 0);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(NULL, NULL, 0);
  g_world_nav_terrain.projection_ready = false;
  g_world_nav_terrain.ready = false;
}


float WorldNavigationTerrainHeightAtPrepared(float source_x, float source_y,
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
    const unsigned cap = g_world_nav_terrain.cliffs.replacement[
        y0 * kWorldNavigationTerrainCells + x0];
    if (cap) {
      float h[4];
      memcpy(h, g_world_nav_terrain.cliffs.faces[cap - 1].height, sizeof(h));
      if (floor_only) {
        static const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
        for (int p = 0; p < 4; p++) {
          const int at = WorldNavigationTerrainVertexIndex(x0 + dx[p], y0 + dy[p]);
          h[p] -= g_world_nav_terrain.height[at] - g_world_nav_terrain.floor[at];
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
  const float *heights = floor_only ? g_world_nav_terrain.floor : g_world_nav_terrain.height;
  const float north = heights[nw] + (heights[ne] - heights[nw]) * u;
  const float south = heights[sw] + (heights[se] - heights[sw]) * u;
  if (authored_weight) {
    const float authored_north = g_world_nav_terrain.authored[nw] +
        (g_world_nav_terrain.authored[ne] -
         g_world_nav_terrain.authored[nw]) * u;
    const float authored_south = g_world_nav_terrain.authored[sw] +
        (g_world_nav_terrain.authored[se] -
         g_world_nav_terrain.authored[sw]) * u;
    *authored_weight = authored_north +
        (authored_south - authored_north) * v;
  }
  return north + (south - north) * v;
}

float WorldNavigationTerrainHeightAtImpl(float source_x, float source_y,
    float *authored_weight, bool floor_only) {
  PrepareWorldNavigationTerrain();
  return WorldNavigationTerrainHeightAtPrepared(source_x,source_y,authored_weight,floor_only);
}

float WorldNavigationTerrainHeightAt(float source_x, float source_y,
                                            float *authored_weight) {
  return WorldNavigationTerrainHeightAtImpl(source_x, source_y, authored_weight, false);
}

bool WorldNavigationSurfaceNormal(
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

void WorldNavigationRadialPoint(
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

bool WorldNavigationProjectSurface(
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

float WorldNavigationShadeFromPoints(const float light[3],
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

float WorldNavigationSurfaceShade(
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

typedef struct WorldNavigationSampleWork {
  WorldNavigationGroundSampleKey key;
  WorldNavigationGroundSample *samples;
  bool valid[kWorldNavigationTerrainAxis];
} WorldNavigationSampleWork;

static void BuildWorldNavigationSampleRows(void *context, size_t first, size_t end) {
  WorldNavigationSampleWork *work = context;
  const WorldNavigationGroundSampleKey key = work->key;
  for (int y = (int)first; y < (int)end; ++y) {
    work->valid[y] = true;
    for (int x = 0; x <= kWorldNavigationTerrainCells; ++x) {
      WorldNavigationGroundSample *sample = &work->samples[WorldNavigationTerrainVertexIndex(x,y)];
      for (int p = 0; p < 3; ++p) {
        const float source_x = (x + (p == 1 ? .5f : 0)) * kSimWorldMapTilePixels;
        const float source_y = (y + (p == 2 ? .5f : 0)) * kSimWorldMapTilePixels;
        if (!SimWorldNavigationGlobe_SampleAtRadius(key.chart_radius_tiles,
                source_x / kSimWorldMapTilePixels, source_y / kSimWorldMapTilePixels,
                sample->normal[p], NULL)) work->valid[y] = false;
        sample->height[p] = key.heights
            ? WorldNavigationTerrainHeightAtPrepared(source_x, source_y, NULL, false)
            : 0;
      }
      const float edge_tiles = fminf(fminf((float)x, (float)y),
                                     fminf((float)(kWorldNavigationTerrainCells - x),
                                           (float)(kWorldNavigationTerrainCells - y)));
      sample->edge_alpha = WorldNavigationSmoothstep(edge_tiles / 10.0f);
      /* Preserve every land-adjacent corner of the chart's ocean blend strip. */
      if (sample->edge_alpha < 1)
        for (int dy = -1; dy <= 0; ++dy) for (int dx = -1; dx <= 0; ++dx) {
          const int cx = x + dx, cy = y + dy;
          if (cx >= 0 && cy >= 0 && cx < kWorldNavigationTerrainCells &&
              cy < kWorldNavigationTerrainCells && !SimWorldMap_CellIsOpenWater(cx, cy))
            sample->edge_alpha = 1;
        }
    }
  }
}

bool PrepareWorldNavigationGroundSamples(const WorldNavigationProjection *projection) {
  /* Resolve all mutable caches on the owner before publishing read-only
   * terrain and water semantics to the bounded row jobs. */
  if (projection->height_world_per_unit > 0) PrepareWorldNavigationTerrain();
  WorldNavigationSampleWork work;
  /* Preserve canonical padding: this key is compared byte-for-byte. */
  memset(&work.key, 0, sizeof(work.key));
  work.samples = g_world_nav_terrain.samples;
  work.key.chart_radius_tiles = projection->chart_radius_tiles;
  work.key.geography_serial = SimWorldMap_GeographySerial();
  work.key.cliff_serial = g_world_nav_terrain.cliff_serial;
  work.key.heights = projection->height_world_per_unit > 0;
  if (g_world_nav_terrain.samples_ready &&
      !memcmp(&work.key, &g_world_nav_terrain.sample_key, sizeof(work.key)))
    return true;
  g_world_nav_terrain.samples_ready = false;
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_TerrainSamples);
  HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainAxis, 16,
      BuildWorldNavigationSampleRows, &work);
  PerformanceMetrics_End(performance);
  for (int y = 0; y < kWorldNavigationTerrainAxis; ++y)
    if (!work.valid[y]) return false;
  g_world_nav_terrain.sample_key = work.key;
  g_world_nav_terrain.samples_ready = true;
  return true;
}

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

void BuildWorldNavigationGridSourceRange(void *context, size_t first, size_t end) {
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

void BuildWorldNavigationCliffSourceRange(void *context, size_t first, size_t end) {
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

/* The owner prepares terrain before dispatch. Helpers read only immutable
 * source/terrain and write disjoint faces, including the original art UVs. */
void BuildWorldNavigationMountainSourceRange(void *context, size_t first, size_t end) {
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

WorldNavigationRadialBounds WorldNavigationSourceBounds(
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
bool BuildWorldNavigationOceanSource(Sim3DDepthSurfaceVertex *quads, ArRenderPointF *mask) {
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
        quads[at] = points[corners[p]];
        mask[at] = (ArRenderPointF){ 0, 0 };
      }
    }
  free(points);
  return true;
}

bool WorldNavigationShellFrame(const WorldNavigationProjection *projection,
    float radius, float centre_z, float outward[3], float right[3], float up[3], float *distance) {
  outward[0] = projection->camera_world[0];
  outward[1] = projection->camera_world[1];
  outward[2] = projection->camera_world[2]-centre_z;
  const float eye_distance = hypotf(hypotf(outward[0],outward[1]),outward[2]);
  if (!isfinite(eye_distance) || eye_distance <= radius) return false;
  for (int i = 0; i < 3; ++i) outward[i] /= eye_distance;
  right[0] = outward[2];
  right[1] = 0;
  right[2] = -outward[0];
  float length = hypotf(right[0],right[2]);
  if (length < .0001f) { right[0] = 1; right[2] = 0; length = 1; }
  for (int i = 0; i < 3; ++i) right[i] /= length;
  up[0] = outward[1]*right[2]-outward[2]*right[1];
  up[1] = outward[2]*right[0]-outward[0]*right[2];
  up[2] = outward[0]*right[1]-outward[1]*right[0];
  *distance = eye_distance;
  return true;
}

bool WorldNavigationOceanTransform(const WorldNavigationProjection *projection,
    Sim3DDepthSurfaceTransform *out) {
  float outward[3], right[3], up[3], distance;
  const float reference = projection->reference_height_units*projection->height_world_per_unit;
  if (!WorldNavigationShellFrame(projection,projection->globe_radius_world*.9975f,
      -projection->globe_radius_world-reference,outward,right,up,&distance)) return false;
  *out =
      (Sim3DDepthSurfaceTransform){ .radial = { .sphere_radius = projection->globe_radius_world,
                                                .reference_height =
                                                    projection->reference_height_units,
                                                .height_scale = projection->height_world_per_unit },
                                    .extra_scale = projection->globe_radius_world,
                                    .ambient = 1 };
  memcpy(out->radial.matrix,projection->matrix,sizeof(out->radial.matrix));
  for (unsigned row = 0; row < 3; ++row) {
    out->radial.basis[row][0] = right[row];
    out->radial.basis[row][1] = up[row];
    out->radial.basis[row][2] = outward[row];
  }
  for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 3; ++col)
    out->shadow_basis[row][col] = projection->globe_frame.right[row]*out->radial.basis[0][col] +
      projection->globe_frame.up[row]*out->radial.basis[1][col] +
      projection->globe_frame.outward[row]*out->radial.basis[2][col];
  return true;
}
