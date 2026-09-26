/* PresentWorldNav weather: the cloud layers over the world view: the cloud
 * texture, cloud UVs split at the longitude seam and the poles, cloud bodies,
 * and the cloud shadows cast on receiving surfaces.
 * Phase: present (FrameSlot only).
 * Tests: tests/present_world_nav_test.c */
#include "sim/world_nav/present_world_nav_internal.h"

/* Private owners: artwork publication, registered surfaces, model bounds,
 * shell scratch, weather mapping, and native composition have separate reset
 * and invalidation lifetimes. These are one mutually exclusive globe view,
 * not an implicit multi-scene renderer or storage borrowed by SIM. */
typedef enum WorldNavigationCloudSurface {
  kWorldNavigationCloudSurface_Ground,
  kWorldNavigationCloudSurface_Ocean,
  kWorldNavigationCloudSurface_Body,
} WorldNavigationCloudSurface;

bool EnsureWorldNavigationCloudTexture(void) {
  enum { kPaddedPixels = kSimWorldNavigationCloudWidth * 2 };
  const int height = kSimWorldNavigationCloudHeight * kSimCloudLayerCount;
  if (g_world_nav_weather.ready) return true;
  if (g_world_nav_weather.unavailable) return false;
  uint32_t *pixels = malloc(
      (size_t)kPaddedPixels * height * sizeof(*pixels));
  if (!pixels) {
    g_world_nav_weather.unavailable = true;
    return false;
  }
  for (int layer = 0; layer < kSimCloudLayerCount; layer++) {
    uint32_t *band = pixels + (size_t)layer * kSimWorldNavigationCloudHeight * kPaddedPixels;
    if (!SimWorldNavigationClouds_Bake(
            band, kPaddedPixels, kSimWorldNavigationCloudWidth,
            kSimWorldNavigationCloudHeight, kSimCloudLayers[layer].scale)) {
      free(pixels);
      g_world_nav_weather.unavailable = true;
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
  g_world_nav_weather.ready = Sim3DDepthPass_UploadAtlasRegions(
      &g_render_device, kSim3DDepthPass_Cloud, pixels,
      kPaddedPixels, height, kPaddedPixels * (int)sizeof(*pixels),
      &region, 1);
  free(pixels);
  g_world_nav_weather.unavailable = !g_world_nav_weather.ready;
  return g_world_nav_weather.ready;
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
  if (!g_world_nav_weather.normals_ready ||
      g_world_nav_weather.chart_radius_tiles != projection->chart_radius_tiles) {
    for (int y = 0; y <= kWorldNavigationTerrainCells; y++)
      for (int x = 0; x <= kWorldNavigationTerrainCells; x++)
        SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles, (float)x, (float)y,
            g_world_nav_weather.ground_normals[y * kWorldNavigationTerrainAxis + x], NULL);
    g_world_nav_weather.normals_ready = true;
    g_world_nav_weather.chart_radius_tiles = projection->chart_radius_tiles;
    for (int layer = 0; layer < kSimCloudLayerCount; layer++)
      g_world_nav_weather.uv[layer].ground_ready = false;
  }
}

static const ArRenderPointF *WorldNavigationCloudUV(
    int bank, WorldNavigationCloudSurface surface,
    const SimWorldNavigationCloudRotation *rotation,
    const WorldNavigationProjection *projection, ArRenderRectI viewport) {
  const bool terrain = surface == kWorldNavigationCloudSurface_Ground;
  const bool ocean = surface == kWorldNavigationCloudSurface_Ocean;
  if (terrain) PrepareWorldNavigationCloudNormals(projection);
  WorldNavigationCloudUVCache *cache = &g_world_nav_weather.uv[bank];
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
    const int count =
        terrain ? kWorldNavigationTerrainVertexCount : kWorldNavigationOceanVertexCount;
    const float (*normal)[3] = terrain ? g_world_nav_weather.ground_normals
        : ocean ? g_world_nav_shells.ocean.normal : g_world_nav_shells.cloud.normal;
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
  return (ArRenderPointF){ (u * (kSimWorldNavigationCloudWidth - 1) + .5f) /
                               (kSimWorldNavigationCloudWidth * 2),
                           (bank * kSimWorldNavigationCloudHeight +
                            latitude * (kSimWorldNavigationCloudHeight - 1) + .5f) /
                               (kSimWorldNavigationCloudHeight * kSimCloudLayerCount) };
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
          color[0] += w * s->color.r;
          color[1] += w * s->color.g;
          color[2] += w * s->color.b;
          color[3] += w * s->color.a;
        }
        v->x = (float)position[0];
        v->y = (float)position[1];
        v->depth = (float)position[2];
        v->color =
            (ArRenderColorF){ (float)color[0], (float)color[1], (float)color[2], (float)color[3] };
        v->uv = WorldNavigationCloudAtlasUV(bank, u[p], patch->v[p] + offset_v);
        if (input_clip) {
          double point[4] = {0};
          for (int j = 0; j < 3; j++) {
            const Scene3DClipPoint *c = &original_clip[at[j]];
            const double weight = patch->weight[p][j];
            point[0] += weight * c->x;
            point[1] += weight * c->y;
            point[2] += weight * c->z;
            point[3] += weight * c->w;
          }
          Scene3DClipPoint *c = &output_clip[visible * 4 + p];
          *c = (Scene3DClipPoint){ (float)point[0], (float)point[1], (float)point[2],
                                   (float)point[3] };
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
  WorldNavigationCloudUVCache *cache = &g_world_nav_weather.uv[bank];
  if (cache->cliff_serial == g_world_nav_terrain.cliff_serial &&
      !memcmp(rotation, &cache->cliff_rotation, sizeof(*rotation))) return;
  for (size_t i = 0; i < g_world_nav_terrain.cliffs.face_count; i++)
    for (int p = 0; p < 4; p++) {
      WorldNavigationCliffProjection *face = &g_world_nav_terrain.cliff_projection[i];
      SimWorldNavigationClouds_UV(face->normal[p], rotation,
          &face->cloud_uv[bank][p].x, &face->cloud_uv[bank][p].y);
    }
  cache->cliff_serial = g_world_nav_terrain.cliff_serial;
  cache->cliff_rotation = *rotation;
}

static bool AppendWorldNavigationCloudGrid(
    int bank, WorldNavigationCloudSurface surface, ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    const SimWorldNavigationCloudRotation *rotation,
    float offset_u, float offset_v, ArRenderColorF colour) {
  const bool terrain = surface == kWorldNavigationCloudSurface_Ground;
  const bool ocean = surface == kWorldNavigationCloudSurface_Ocean;
  const ArRenderPointF *coordinates =
      WorldNavigationCloudUV(bank, surface, rotation, projection, viewport);
  WorldNavigationCloudUVCache *cache = &g_world_nav_weather.uv[bank];
  if (terrain) WorldNavigationPrepareCliffCloudUV(bank, rotation);
  const WorldNavigationShellGeometry *shell = ocean
      ? &g_world_nav_shells.ocean : &g_world_nav_shells.cloud;
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
        if (g_world_nav_terrain.cliffs.replacement[y * kWorldNavigationTerrainCells + x])
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
      const uint8_t *outside = terrain ? g_world_nav_terrain.outside : shell->outside;
      if (outside[at[0]] & outside[at[1]] & outside[at[2]] & outside[at[3]]) continue;
      float u[4];
      for (int p = 0; p < 4; p++) {
        u[p] = coordinates[at[p]].x + offset_u;
        u[p] -= floorf(u[p]);
      }
      SimWorldNavigationClouds_Unwrap(u);
      for (int p = 0; p < 4; p++) {
        Sim3DDepthVertex *v = &row[row_count * 4 + p];
        *v = terrain ? g_world_nav_terrain.depth[at[p]] : shell->points[at[p]];
        v->color = colour;
        if (!terrain) v->color.a *= shell->alpha[at[p]];
        v->uv = WorldNavigationCloudAtlasUV(bank, u[p], coordinates[at[p]].y + offset_v);
      }
      if (projection->clip_frustum) {
        const Scene3DClipPoint *clip = terrain ? g_world_nav_terrain.clip : shell->clip;
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
    for (size_t i = 0; i < g_world_nav_terrain.cliffs.face_count; i++) {
      const WorldNavigationCliffProjection *face = &g_world_nav_terrain.cliff_projection[i];
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
  const size_t needed = g_world_nav_weather.receiver_count + count;
  size_t maximum_receivers = 4 * 1024 * 1024 / sizeof(WorldNavigationShadowReceiver);
#if AR_WORLD_NAV_CACHE_TESTING
  if (g_world_nav_receiver_test_bytes / sizeof(WorldNavigationShadowReceiver) < maximum_receivers)
    maximum_receivers = g_world_nav_receiver_test_bytes / sizeof(WorldNavigationShadowReceiver);
#endif
  if (needed > maximum_receivers) return false;
  if (needed > g_world_nav_weather.receiver_capacity) {
    size_t capacity =
        g_world_nav_weather.receiver_capacity ? g_world_nav_weather.receiver_capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    if (capacity > maximum_receivers) capacity = maximum_receivers;
    void *receivers =
        realloc(g_world_nav_weather.receivers, capacity * sizeof(*g_world_nav_weather.receivers));
    if (!receivers) return false;
    g_world_nav_weather.receivers = receivers;
    g_world_nav_weather.receiver_capacity = capacity;
  }
  for (size_t i = 0; i < count; i++) {
    WorldNavigationShadowReceiver *receiver =
        &g_world_nav_weather.receivers[g_world_nav_weather.receiver_count++];
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
  key.projection = *projection;
  key.viewport = viewport;
  key.terrain_serial = g_world_nav_terrain.serial;
  key.cliff_serial = g_world_nav_terrain.cliff_serial;
  const bool same = !memcmp(&key, &g_world_nav_weather.receiver_key, sizeof(key));
  if (same && g_world_nav_weather.receivers_unavailable) return false;
  if (same && g_world_nav_weather.receivers_ready) return true;
  g_world_nav_weather.receiver_key = key;
  g_world_nav_weather.receivers_unavailable = false;
  g_world_nav_weather.receiver_mesh_unavailable = false;
  g_world_nav_weather.spherical_unavailable = false;
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  g_world_nav_weather.receivers_ready = false;
  g_world_nav_weather.receiver_mesh_ready = false;
  g_world_nav_weather.spherical_ready = false;
  g_world_nav_weather.receiver_count = 0;
  Sim3DDepthVertex input[4];
  Scene3DClipPoint clip[4];
  for (int y = 0; y < kWorldNavigationTerrainCells; y++)
    for (int x = 0; x < kWorldNavigationTerrainCells; x++) {
      if (g_world_nav_terrain.cliffs.replacement[y * kWorldNavigationTerrainCells + x]) continue;
      const int start = y * kWorldNavigationTerrainAxis + x;
      const int at[4] = {start, start + 1, start + 1 + kWorldNavigationTerrainAxis,
          start + kWorldNavigationTerrainAxis};
      if (g_world_nav_terrain.outside[at[0]] & g_world_nav_terrain.outside[at[1]] &
          g_world_nav_terrain.outside[at[2]] & g_world_nav_terrain.outside[at[3]]) continue;
      for (int p = 0; p < 4; p++) {
        input[p] = g_world_nav_terrain.depth[at[p]];
        if (projection->clip_frustum) clip[p] = g_world_nav_terrain.clip[at[p]];
      }
      if (!CacheWorldNavigationReceiver(kWorldNavigationReceiver_Ground, 0, at, input,
              projection->clip_frustum ? clip : NULL, viewport)) goto unavailable;
    }
  for (size_t i = 0; i < g_world_nav_terrain.cliffs.face_count; i++) {
    const WorldNavigationCliffProjection *face = &g_world_nav_terrain.cliff_projection[i];
    if (face->outside[0] & face->outside[1] & face->outside[2] & face->outside[3]) continue;
    const int at[4] = {0, 1, 2, 3};
    if (!CacheWorldNavigationReceiver(kWorldNavigationReceiver_Cliff, (uint32_t)i, at,
            face->depth, projection->clip_frustum ? face->clip : NULL, viewport)) goto unavailable;
  }
  const WorldNavigationShellGeometry *shell = &g_world_nav_shells.ocean;
  for (int y = 0; y < kWorldNavigationOceanRings; y++)
    for (int x = 0; x < kWorldNavigationOceanSectors; x++) {
      const int next = (x + 1) % kWorldNavigationOceanSectors;
      const int at[4] = {
        y ? 1 + (y - 1) * kWorldNavigationOceanSectors + x : 0,
        y ? 1 + (y - 1) * kWorldNavigationOceanSectors + next : 0,
        1 + y * kWorldNavigationOceanSectors + next, 1 + y * kWorldNavigationOceanSectors + x};
      if (!shell->front[at[0]] && !shell->front[at[1]] && !shell->front[at[2]] &&
          !shell->front[at[3]])
        continue;
      if (shell->outside[at[0]] & shell->outside[at[1]] & shell->outside[at[2]] &
          shell->outside[at[3]])
        continue;
      for (int p = 0; p < 4; p++) {
        input[p] = shell->points[at[p]];
        if (projection->clip_frustum) clip[p] = shell->clip[at[p]];
      }
      if (!CacheWorldNavigationReceiver(kWorldNavigationReceiver_Ocean, 0, at, input,
              projection->clip_frustum ? clip : NULL, viewport)) goto unavailable;
    }
  g_world_nav_weather.receiver_key = key;
  g_world_nav_weather.receivers_ready = true;
  return true;
unavailable:
  g_world_nav_weather.receivers_unavailable = true; /* Retry when this view/source changes. */
  g_world_nav_weather.receiver_count = 0;
  return false;
}

static bool PrepareWorldNavigationReceiverMesh(void) {
  if (g_world_nav_weather.receiver_mesh_unavailable || !g_world_nav_weather.receiver_count)
    return false;
  if (g_world_nav_weather.receiver_mesh_ready &&
      Sim3DDepthPass_MeshReady(g_world_nav_weather.receiver_mesh)) return true;
  if (!g_world_nav_weather.receiver_mesh)
    g_world_nav_weather.receiver_mesh = Sim3DDepthPass_CreateMesh();
  if (!g_world_nav_weather.receiver_mesh) goto unavailable;
  const size_t count = g_world_nav_weather.receiver_count;
  if (count > g_world_nav_weather.receiver_mesh_capacity) {
    /* Receiver storage already has a strict 4 MiB bound. Allocate both
     * parallel arrays before replacing the previous complete publication. */
    const size_t capacity = g_world_nav_weather.receiver_capacity;
    Sim3DDepthPosition *positions = malloc(capacity * 4 * sizeof(*positions));
    ArRenderPointF *uv = malloc(capacity * 4 * sizeof(*uv));
    if (!positions || !uv) { free(positions); free(uv); goto unavailable; }
    free(g_world_nav_weather.receiver_positions);
    free(g_world_nav_weather.receiver_uv);
    g_world_nav_weather.receiver_positions = positions;
    g_world_nav_weather.receiver_uv = uv;
    g_world_nav_weather.receiver_mesh_capacity = capacity;
  }
  for (size_t i = 0; i < count; ++i)
    for (int p = 0; p < 4; ++p) {
      const WorldNavigationClipPlan *geometry = &g_world_nav_weather.receivers[i].geometry;
      g_world_nav_weather.receiver_positions[i * 4 + p] = (Sim3DDepthPosition){
        geometry->points[p].x, geometry->points[p].y, geometry->points[p].depth,
      };
    }
  if (!Sim3DDepthPass_UpdateMesh(g_world_nav_weather.receiver_mesh,
          g_world_nav_weather.receiver_positions, count)) goto unavailable;
  g_world_nav_weather.receiver_mesh_ready = true;
  return true;
unavailable:
  g_world_nav_weather.receiver_mesh_unavailable = true;
  return false;
}

static bool PrepareWorldNavigationSphericalMesh(const WorldNavigationProjection *projection) {
  if (g_world_nav_weather.spherical_unavailable || !g_world_nav_weather.receiver_count)
    return false;
  if (g_world_nav_weather.spherical_ready &&
      Sim3DDepthPass_MeshReady(g_world_nav_weather.spherical_mesh))
    return true;
  if (!g_world_nav_weather.spherical_mesh)
    g_world_nav_weather.spherical_mesh = Sim3DDepthPass_CreateSphericalMesh();
  if (!g_world_nav_weather.spherical_mesh) goto unavailable;
  const size_t count = g_world_nav_weather.receiver_count;
  if (count > g_world_nav_weather.spherical_capacity) {
    const size_t capacity = g_world_nav_weather.receiver_capacity;
    void *quads = realloc(g_world_nav_weather.spherical_quads,
                          capacity * sizeof(*g_world_nav_weather.spherical_quads));
    if (!quads) goto unavailable;
    g_world_nav_weather.spherical_quads = quads;
    g_world_nav_weather.spherical_capacity = capacity;
  }
  PrepareWorldNavigationCloudNormals(projection);
  for (size_t i = 0; i < count; ++i) {
    const WorldNavigationShadowReceiver *receiver = &g_world_nav_weather.receivers[i];
    Sim3DDepthSphericalQuad *quad = &g_world_nav_weather.spherical_quads[i];
    quad->triangle = receiver->geometry.triangle;
    for (int p = 0; p < 4; ++p) {
      quad->positions[p] = (Sim3DDepthPosition){receiver->geometry.points[p].x,
        receiver->geometry.points[p].y, receiver->geometry.points[p].depth};
      memcpy(quad->weights[p], receiver->geometry.points[p].weights, sizeof(quad->weights[p]));
      const float *normal = receiver->surface == kWorldNavigationReceiver_Cliff
          ? g_world_nav_terrain.cliff_projection[receiver->cliff].normal[p]
          : (receiver->surface == kWorldNavigationReceiver_Ocean
                 ? g_world_nav_shells.ocean.normal
                 : g_world_nav_weather.ground_normals)[receiver->corners[p]];
      memcpy(quad->normals[p], normal, sizeof(quad->normals[p]));
    }
  }
  if (!Sim3DDepthPass_UpdateSphericalMesh(g_world_nav_weather.spherical_mesh,
          g_world_nav_weather.spherical_quads, count)) goto unavailable;
  Sim3DPerformance_AddPath(kSim3DPath_Publish);
  g_world_nav_weather.spherical_ready = true;
  return true;
unavailable:
  g_world_nav_weather.spherical_unavailable = true;
  return false;
}

static bool AppendWorldNavigationReceivers(int bank,
    const SimWorldNavigationCloudRotation *rotation,
    const WorldNavigationProjection *projection, ArRenderRectI viewport,
    float offset_u, float offset_v, ArRenderColorF color) {
  if (!g_world_nav_weather.receiver_count) return true;
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
        g_world_nav_weather.spherical_mesh, &sample);
    Sim3DPerformance_AddPath(ok ? kSim3DPath_GpuReuse : kSim3DPath_Rejected);
    return ok;
  }
  if (g_world_nav_weather.spherical_unavailable) Sim3DPerformance_AddPath(kSim3DPath_Rejected);
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
  for (size_t i = 0; i < g_world_nav_weather.receiver_count; i++) {
    const WorldNavigationShadowReceiver *receiver = &g_world_nav_weather.receivers[i];
    ArRenderPointF source[4], uv[4];
    float u[4];
    for (int p = 0; p < 4; p++) {
      source[p] = receiver->surface == kWorldNavigationReceiver_Cliff
          ? g_world_nav_terrain.cliff_projection[receiver->cliff].cloud_uv[bank][p]
          : (receiver->surface == kWorldNavigationReceiver_Ocean ? ocean
                                                                 : ground)[receiver->corners[p]];
      u[p] = source[p].x + offset_u;
      u[p] -= floorf(u[p]);
    }
    SimWorldNavigationClouds_Unwrap(u);
    for (int p = 0; p < 4; p++)
      uv[p] = WorldNavigationCloudAtlasUV(bank, u[p], source[p].y + offset_v);
    if (retained) {
      WorldNavigationApplyShadowUV(&receiver->geometry, uv,
                                   g_world_nav_weather.receiver_uv + i * 4);
      continue;
    }
    WorldNavigationApplyShadowPlan(&receiver->geometry, uv, color, vertices + count * 4);
    if (++count == kBatch) {
      if (!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_CloudShadow, vertices, count)) return false;
      count = 0;
    }
  }
  if (retained)
    return Sim3DDepthPass_AppendMeshSample(
        kSim3DDepthPass_CloudShadow, g_world_nav_weather.receiver_mesh,
        g_world_nav_weather.receiver_uv, g_world_nav_weather.receiver_count, color);
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
  if (!g_world_nav_gpu_grid.drawn || g_world_nav_weather.body_unavailable) return false;
  const float reference = projection->reference_height_units*projection->height_world_per_unit;
  const float radius = projection->globe_radius_world+reference+projection->cloud_height_world;
  const float centre_z = -projection->globe_radius_world-reference;
  float outward[3], right[3], up[3], distance;
  if (!WorldNavigationShellFrame(projection, radius, centre_z, outward, right, up, &distance))
    return false;
  if (!g_world_nav_weather.body_mesh)
    g_world_nav_weather.body_mesh = Sim3DDepthPass_CreateSphericalBodyMesh();
  if (!g_world_nav_weather.body_mesh) goto unavailable;
  if (!Sim3DDepthPass_MeshReady(g_world_nav_weather.body_mesh) ||
      g_world_nav_weather.body_radius != radius || g_world_nav_weather.body_distance != distance) {
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
        points[at] =
            (Sim3DDepthSphericalBodyVertex){ { sine * g_world_nav_shells.longitude_cos[x],
                                               sine * g_world_nav_shells.longitude_sin[x], cosine },
                                             alpha };
      }
    }
    const size_t count = kWorldNavigationOceanRings*kWorldNavigationOceanSectors;
    if (!g_world_nav_weather.body_vertices)
      g_world_nav_weather.body_vertices =
          malloc(count * 4 * sizeof(*g_world_nav_weather.body_vertices));
    Sim3DDepthSphericalBodyVertex *vertices = g_world_nav_weather.body_vertices;
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
    const bool ok =
        Sim3DDepthPass_UpdateSphericalBodyMesh(g_world_nav_weather.body_mesh, vertices, count);
    if (!ok) goto unavailable;
    g_world_nav_weather.body_radius = radius;
    g_world_nav_weather.body_distance = distance;
  }
  Sim3DDepthSphericalBodyTransform transform = {.centre = {0,0,centre_z}, .radius = radius};
  memcpy(transform.matrix,projection->matrix,sizeof(transform.matrix));
  for (unsigned r = 0; r < 3; ++r) {
    transform.basis[r][0] = right[r];
    transform.basis[r][1] = up[r];
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
    samples[bank] = (Sim3DDepthSphericalSample){
      .rotation = { r.cos_u, r.sin_u, r.cos_v, r.sin_v },
      .atlas = { 0, bank * kSimWorldNavigationCloudHeight, kSimWorldNavigationCloudWidth,
                 kSimWorldNavigationCloudHeight },
      .texture_size = { kSimWorldNavigationCloudWidth * 2,
                        kSimWorldNavigationCloudHeight * kSimCloudLayerCount },
      .color = { 1, 1, 1, opacity * layer->weight }
    };
  }
  if (Sim3DDepthPass_AppendSphericalBodies(g_world_nav_weather.body_mesh, &transform, samples,
                                           kSimCloudLayerCount))
    return true;
unavailable:
  g_world_nav_weather.body_unavailable = true;
  Sim3DPerformance_AddPath(kSim3DPath_Rejected);
  fprintf(stderr,"[world-navigation] GPU cloud body unavailable; using compatible clouds\n");
  return false; /* The group queues nothing on rejection. */
}

static PresentationOutcome OmitWorldNavigationWeather(const char *reason) {
  if (!g_world_nav_weather.failure_reported) {
    g_world_nav_weather.failure_reported = true;
    fprintf(stderr, "[world-navigation] optional weather omitted: %s\n",
            reason && reason[0] ? reason : "renderer rejected the effect");
  }
  return kPresentationOutcome_OptionalOmitted;
}

/* Immutable per-presentation shadow parameters shared by the GPU source grid
 * and the remaining CPU-projected ocean/cliff receivers. */
size_t WorldNavigationShadowSamples(const FrameSlot *slot, uint64_t elapsed_ms,
    Sim3DDepthSphericalSample samples[kSimCloudLayerCount * 3]) {
  if (!slot->sim.world_navigation_clouds || !slot->sim.cloud_opacity_pct ||
      !slot->sim.world_navigation_cloud_shadows || !slot->sim.world_navigation_lighting ||
      !slot->sim.shadow_opacity_pct) return 0;
  float light_x, light_y;
  SimShadowLight(slot, &light_x, &light_y);
  const float shadow_x = light_x * slot->sim.cloud_altitude_px / 8.0f;
  const float shadow_y = light_y * slot->sim.cloud_altitude_px / 8.0f;
  const float blur = slot->sim.shadow_softness_pct * .08f;
  const int count = blur > .01f ? 3 : 1;
  const float px = -sinf(slot->sim.light_azimuth_deg * kPi / 180),
              py = cosf(slot->sim.light_azimuth_deg * kPi / 180);
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
      samples[used++] = (Sim3DDepthSphericalSample){
        .rotation = { r.cos_u, r.sin_u, r.cos_v, r.sin_v },
        .offset = { (shadow_x + px * spread) / kSimWorldMapPixels,
                    (shadow_y + py * spread) / kSimWorldMapPixels },
        .atlas = { 0, bank * kSimWorldNavigationCloudHeight, kSimWorldNavigationCloudWidth,
                   kSimWorldNavigationCloudHeight },
        .texture_size = { kSimWorldNavigationCloudWidth * 2,
                          kSimWorldNavigationCloudHeight * kSimCloudLayerCount },
        .color = { 0, 0, 0,
                   opacity * layer->weight * (slot->sim.shadow_opacity_pct / (float)kPercentScale) *
                       .35f * weight }
      };
    }
  }
  return used;
}

PresentationOutcome DrawWorldNavigationWeather(
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
  const size_t shadow_count =
      g_world_nav_gpu_grid.drawn ? 0 : WorldNavigationShadowSamples(slot, elapsed_ms, shadows);
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
      !PresentWorldNavSky_DrawClouds(&g_render_device, slot, viewport, projection, elapsed_ms,
                                     drift, opacity))
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

void ResetWorldNavigationWeather(void) {
  Sim3DDepthPass_DestroyMesh(g_world_nav_weather.receiver_mesh);
  Sim3DDepthPass_DestroyMesh(g_world_nav_weather.spherical_mesh);
  Sim3DDepthPass_DestroyMesh(g_world_nav_weather.body_mesh);
  free(g_world_nav_weather.body_vertices);
  g_world_nav_weather.body_vertices = NULL;
  g_world_nav_weather.body_mesh = NULL;
  g_world_nav_weather.body_unavailable = false;
  g_world_nav_weather.body_radius = g_world_nav_weather.body_distance = 0;
  free(g_world_nav_weather.spherical_quads);
  g_world_nav_weather.spherical_mesh = NULL;
  g_world_nav_weather.spherical_quads = NULL;
  g_world_nav_weather.spherical_capacity = 0;
  g_world_nav_weather.spherical_ready = g_world_nav_weather.spherical_unavailable =
      false;
  g_world_nav_weather.receiver_mesh = NULL;
  free(g_world_nav_weather.receiver_positions);
  free(g_world_nav_weather.receiver_uv);
  g_world_nav_weather.receiver_positions = NULL;
  g_world_nav_weather.receiver_uv = NULL;
  g_world_nav_weather.receiver_mesh_capacity = 0;
  g_world_nav_weather.receiver_mesh_ready =
      g_world_nav_weather.receiver_mesh_unavailable = false;
  free(g_world_nav_weather.receivers);
  g_world_nav_weather.receivers = NULL;
  g_world_nav_weather.receiver_count = g_world_nav_weather.receiver_capacity = 0;
  g_world_nav_weather.receivers_ready = g_world_nav_weather.receivers_unavailable =
      false;
  g_world_nav_weather.ready = false;
  g_world_nav_weather.unavailable = false;
  PresentWorldNavSky_Reset();
  g_world_nav_weather.failure_reported = false;
}
