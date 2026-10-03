/* PresentSimGlobe: the continuous-globe SIM view (present_sim_globe.h). Builds
 * the globe surface around the active town from the shared world resources,
 * places the town models on it, and draws the scene.
 * Phase: present (FrameSlot only).
 * Tests: tests/present_world_nav_gpu_test.c */
#include "sim/world_nav/present_world_nav_internal.h"
static struct {
  Sim3DMeshSet surface;
  SimGlobeMapping map;
  uint32_t geography, mountains, cliffs;
  uint16_t height_percent;
  size_t quads, mountain_first;
  bool ready, detailed_town;
} s_sim_globe;

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
  return !g_world_nav_terrain.cliffs.replacement[y*kWorldNavigationTerrainCells+x];
}

static bool SimGlobeKeepFace(const SimGlobeMapping *map,
    const float x[4], const float y[4], bool mountain, bool detailed_town) {
  float cx = 0, cy = 0;
  for (int p = 0; p < 4; ++p) { cx += x[p]*.25f; cy += y[p]*.25f; }
  if (detailed_town && !mountain && SimGlobeInsideTown(map,cx,cy)) return false;
  return !mountain || SimGlobeNearby(map,cx,cy);
}

typedef struct SimGlobeGridSourceWork {
  WorldNavigationGridSourceWork source;
  const SimGlobeMapping *map;
  bool valid[kWorldNavigationTerrainAxis];
} SimGlobeGridSourceWork;

static void BuildSimGlobeGridRows(void *context, size_t first, size_t end) {
  SimGlobeGridSourceWork *work = context;
  BuildWorldNavigationGridSourceRange(&work->source,
      first*kWorldNavigationTerrainAxis,end*kWorldNavigationTerrainAxis);
  for (int y = (int)first; y < (int)end; ++y) {
    work->valid[y] = true;
    for (int x = 0; x < kWorldNavigationTerrainAxis; ++x)
      work->valid[y] &= SimGlobeEmbedVertex(work->map,x,y,0,
          &work->source.vertices[WorldNavigationTerrainVertexIndex(x,y)]);
  }
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
      s_sim_globe.mountains == g_world_nav_mountains.geometry_revision &&
      s_sim_globe.cliffs == g_world_nav_terrain.cliff_serial &&
      s_sim_globe.height_percent == slot->sim.height_scale_x100 && water_ready) return true;
  if (!PrepareWorldNavigationGroundSamples(projection)) return false;
  const size_t cliffs = g_world_nav_terrain.cliffs.face_count;
  const size_t mountains =
      g_world_nav_mountains.active ? g_world_nav_mountains.scene.face_count : 0;
  /* Count only published geometry. Active detailed surfaces replace their
   * overview counterparts; distant mountains consume no SIM storage. */
  size_t capacity = kWorldNavigationOceanQuads;
  for (int y = 0; y < kWorldNavigationTerrainCells; ++y)
    for (int x = 0; x < kWorldNavigationTerrainCells; ++x)
      capacity += SimGlobeKeepCell(map,x,y,detailed_town);
  for (size_t i = 0; i < cliffs; ++i) {
    const SimWorldNavigationCliffFace *f = &g_world_nav_terrain.cliffs.faces[i];
    capacity += SimGlobeKeepFace(map,f->x,f->y,false,detailed_town);
  }
  for (size_t i = 0; i < mountains; ++i) {
    const SimWorldNavigationMountainFace *f = &g_world_nav_mountains.scene.faces[i];
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
  SimGlobeGridSourceWork work = {.source={g_world_nav_terrain.samples,points,ratio},.map=map};
  const PerformanceScope bake = PerformanceMetrics_Begin(kPerformance_GlobeBuild);
  HostParallelWork_Run(WorldNavigationWorkers(), kWorldNavigationTerrainAxis, 16,
      BuildSimGlobeGridRows, &work);
  PerformanceMetrics_End(bake);
  for (int y = 0; y < kWorldNavigationTerrainAxis; ++y) ok &= work.valid[y];
  size_t count = kWorldNavigationOceanQuads;
  for (int y = 0; ok && y < kWorldNavigationTerrainCells; ++y)
    for (int x = 0; x < kWorldNavigationTerrainCells; ++x) {
      /* Detailed active tops replace the overview cells without a second
       * backing canvas or flattened footprint. */
      if (!SimGlobeKeepCell(map,x,y,detailed_town)) continue;
      const int at = WorldNavigationTerrainVertexIndex(x,y);
      const int corners[4] = { at, at + 1, at + kWorldNavigationTerrainAxis + 1,
                               at + kWorldNavigationTerrainAxis };
      for (int p = 0; p < 4; ++p) {
        vertices[count*4+p] = points[corners[p]];
        mask[count*4+p]=(ArRenderPointF){(x+(p==1 || p==2))/128.0f,(y+(p>=2))/128.0f};
      }
      ++count;
    }
  for (size_t i = 0; ok && i < cliffs; ++i) {
    const SimWorldNavigationCliffFace *f = &g_world_nav_terrain.cliffs.faces[i];
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
    const SimWorldNavigationMountainFace *f = &g_world_nav_mountains.scene.faces[i];
    if ((detailed_town && f->town == map->town) ||
        !SimGlobeKeepFace(map,f->x,f->y,true,detailed_town)) continue;
    for (int p = 0; p < 4; ++p) {
      Sim3DDepthSurfaceVertex *v = &vertices[count*4+p];
      const float shade = f->brightness[p]/255.0f;
      *v = (Sim3DDepthSurfaceVertex){.uv = {f->uv[p].x,f->uv[p].y},
          .color = {shade,shade,shade,1}};
      float normal[3], metric;
      ok &= SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius, f->x[p], f->y[p], normal,
                                                   &metric);
      v->elevation[0] = WorldNavigationTerrainHeightAtPrepared(
          f->x[p]*kSimWorldMapTilePixels,f->y[p]*kSimWorldMapTilePixels,NULL,true);
      v->elevation[1] = f->z[p]*metric;
      ok &= SimGlobeEmbedVertex(map,f->x[p],f->y[p],
          slot->sim.height_scale_x100/(100.0f*map->metric),v);
      mask[count*4+p]=(ArRenderPointF){f->x[p]/128,f->y[p]/128};
    }
    ++count;
  }
  ok = ok && count == capacity &&
      Sim3DMeshSet_UpdateSurface(&s_sim_globe.surface, vertices, mask, count);
  if (ok && detailed_town)
    ok = PresentSimGlobeWater_Prepare(map,&slot->sim.world_navigation_towns.ground,points);
  free(vertices);
  free(points);
  free(mask);
  if (!ok) {
    fprintf(stderr,"[sim-globe-underlay] surface publication rejected quads=%zu\n",count);
    return false;
  }
  s_sim_globe.map = *map;
  s_sim_globe.geography = geography;
  s_sim_globe.mountains = g_world_nav_mountains.geometry_revision;
  s_sim_globe.cliffs = g_world_nav_terrain.cliff_serial;
  s_sim_globe.height_percent = slot->sim.height_scale_x100;
  s_sim_globe.quads = count;
  s_sim_globe.mountain_first = mountain_first;
  s_sim_globe.ready = true;
  s_sim_globe.detailed_town = detailed_town;
  if (Sim3DPerformance_Enabled())
    fprintf(stderr,"[sim-globe-town] source town=%u quads=%zu mountains=%zu\n",
        slot->sim.town,count,count-mountain_first);
  return true;
}

#if AR_SIM_GLOBE_TESTING
bool PresentSimGlobe_TestSurfaceSource(const FrameSlot *slot,
    SimWorldNavigationMountainFace *faces, size_t count,
    size_t *published_mountains, size_t *chunks) {
  const SimWorldNavigationMountainScene saved = g_world_nav_mountains.scene;
  const bool active = g_world_nav_mountains.active;
  const uint32_t revision = g_world_nav_mountains.geometry_revision;
  const SimGlobeMapping map = s_sim_globe.map;
  const WorldNavigationProjection projection = {.chart_radius_tiles = map.chart_radius,
    .height_world_per_unit = map.landscape > 0 ? 1 : 0};
  g_world_nav_mountains.scene.faces = faces;
  g_world_nav_mountains.scene.face_count = count;
  g_world_nav_mountains.active = true;
  ++g_world_nav_mountains.geometry_revision;
  s_sim_globe.ready = false;
  const bool ok = SimGlobeBuildSurface(slot,&projection,&map,false);
  *published_mountains = ok ? s_sim_globe.quads - s_sim_globe.mountain_first : 0;
  *chunks = s_sim_globe.surface.count;
  g_world_nav_mountains.scene = saved;
  g_world_nav_mountains.active = active;
  g_world_nav_mountains.geometry_revision = revision;
  Sim3DMeshSet_Destroy(&s_sim_globe.surface);
  s_sim_globe.ready = false;
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

static bool PrepareSimGlobeView(const FrameSlot *slot, ArRenderRectI source, ArRenderRectI viewport,
                                const Scene3DCamera *camera, const float matrix[16],
                                float radius_scale, bool sim_facades,
                                WorldNavigationProjection *out_projection,
                                PresentSimGlobeView *out_view) {
  if (!slot || !camera || !matrix || source.w <= 0 || source.h <= 0 || viewport.w <= 0 ||
      viewport.h <= 0 || !isfinite(radius_scale) || radius_scale < 1 || radius_scale > 4 ||
      (unsigned)slot->sim.background_voxel_detail >= kSimBackgroundVoxelDetail_Count ||
      (sim_facades &&
       ((unsigned)slot->sim.background_voxel_facing >= kSimBackgroundVoxelFacing_Count ||
        (unsigned)slot->sim.background_voxel_shading >= kSimBackgroundVoxelShading_Count)))
    return false;
  const float chart_radius = WorldNavigationChartRadius(slot) * radius_scale;
  if (!EnsureWorldNavigationResourcesAtRadius(slot, chart_radius)) return false;
  /* SIM owns its active landscape and model shading. Navigation toggles may
   * reduce the neighbouring overview, but cannot flatten or unlight the town
   * the player is editing. Do not copy/mutate a large captured FrameSlot to
   * manufacture settings for this consumer. */
  const bool relief = AR_SIM3D_TERRAIN_ELEVATION;
  if (relief && slot->sim.landscape_height_pct) PrepareWorldNavigationTerrain();
  /* The private GPU model cache now holds embedded sources. A later return
   * to navigation must not repeat its old projection key over these meshes. */
  g_world_nav_models.gpu_current_ready = false;
  g_world_nav_models.gpu_current_rejected = false;
  g_world_nav_models.projection_key_ready = false;
  const float ox = slot->sim.underlay_origin_tile_x, oy = slot->sim.underlay_origin_tile_y;
  float reference = 0;
  if (relief && !SimWorldNavigationTerrain_RegisterTownFloor(
                    slot->sim.town, 16, 16, SimTownTerrain_HeightUnitsAt(slot->sim.town, 256, 256),
                    &reference))
    return false;
  SimGlobeMapping map;
  if (!SimGlobeMapping_Build(slot->sim.town, ox, oy, chart_radius, reference,
                             relief ? slot->sim.landscape_height_pct / 100.0f : 0, &map))
    return false;
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
    map.reference_height -= SimTownTerrain_HeightUnitsAt(map.town, 256, 256) * map.town_landscape *
                            map.metric / map.landscape;
  WorldNavigationProjection projection = {
      .chart_radius_tiles = map.chart_radius,
      .globe_radius_world = map.radius,
      .height_world_per_unit = map.landscape > 0 ? 1 : 0,
      .clip_frustum = true,
      .tile_world = 1 / map.metric,
      .globe_frame = {.right = {1, 0, 0}, .up = {0, 1, 0}, .outward = {0, 0, 1}}};
  const float aspect = (float)viewport.w / viewport.h;
  const float scale[3] = {16 * aspect / source.w, 16.0f / source.h, 16.0f / source.h};
  const float offset[3] = {
      ((slot->sim.underlay_screen_x0 - slot->sim.camera_x + 256.0f - source.x) / source.w - .5f) *
          aspect,
      .5f - (256.0f - slot->sim.camera_y - source.y) / source.h, 0};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col)
      projection.matrix[col * 4 + row] = matrix[col * 4 + row] * scale[col];
    projection.matrix[12 + row] = matrix[12 + row];
    for (int col = 0; col < 3; ++col)
      projection.matrix[12 + row] += matrix[col * 4 + row] * offset[col];
  }
  for (int i = 0; i < 3; ++i)
    projection.camera_world[i] = (-camera->distance * matrix[i * 4 + 3] - offset[i]) / scale[i];
  PresentSimGlobeView view = {.map = map};
  memcpy(view.matrix, projection.matrix, sizeof(view.matrix));
  memcpy(view.camera, projection.camera_world, sizeof(view.camera));
  *out_projection = projection;
  *out_view = view;
  return true;
}

static Sim3DDepthSurfaceTransform SimGlobeSurfaceTransform(const FrameSlot *slot,
                                                           const PresentSimGlobeView *view) {
  const SimGlobeMapping map = view->map;
  Sim3DDepthSurfaceTransform t = {
      .radial = {.sphere_radius = map.radius, .height_scale = 1}, .ambient = .76f, .diffuse = .24f};
  t.focus = PresentSimGlobeFocus_Resolve(
      &map, (slot->sim.effective_features & kSimFeature_CullHaze) != 0, slot->sim.cull_dim_pct,
      slot->sim.underlay_haze_pct, slot->sim.cull_haze_lead_px);
  memcpy(t.radial.matrix, view->matrix, sizeof(t.radial.matrix));
  for (int i = 0; i < 3; ++i) t.radial.basis[i][i] = 1;
  const float azimuth = slot->sim.light_azimuth_deg * kPi / 180,
              elevation = slot->sim.light_elevation_deg * kPi / 180;
  t.light[0] = -cosf(azimuth) * cosf(elevation);
  t.light[1] = -sinf(azimuth) * cosf(elevation);
  t.light[2] = sinf(elevation);
  return t;
}

static bool AppendSimGlobeSurfaces(const FrameSlot *slot, ArRenderRectI source,
                                   ArRenderRectI viewport,
                                   const WorldNavigationProjection *projection,
                                   const SimGlobeMapping *mapping, bool detailed_town,
                                   const PresentSimGlobeContent *content,
                                   const PresentSimGlobeGroundShadow *shadow,
                                   Sim3DDepthSurfaceTransform t) {
  const SimGlobeMapping map = *mapping;
  const PresentSimGlobeGroundShadow town_shadow = *shadow;
  bool ok = g_world_nav_art.displayed_version < 0 ||
            Sim3DDepthPass_SelectAtlasVersion(g_world_nav_art.atlas_cache,
                                              (unsigned)g_world_nav_art.displayed_version);
  Sim3DDepthSurfaceBatch batches[3] = {
      {.layer = kSim3DDepthPass_Ground, .range = {0, kWorldNavigationOceanQuads}},
      {.layer = kSim3DDepthPass_Ground,
       .range = {kWorldNavigationOceanQuads,
                 s_sim_globe.mountain_first - kWorldNavigationOceanQuads},
       .transform = t},
      {.layer = kSim3DDepthPass_WorldMountain,
       .range = {s_sim_globe.mountain_first, s_sim_globe.quads - s_sim_globe.mountain_first},
       .transform = t}};
  batches[2].transform.ambient = .90f;
  batches[2].transform.diffuse = 0;
  WorldNavigationProjection ocean = *projection;
  ocean.reference_height_units = map.reference_height * map.landscape / map.metric;
  ok = ok && WorldNavigationOceanTransform(&ocean, &batches[0].transform);
  batches[0].transform.focus = t.focus;
  ok = ok && Sim3DMeshSet_AppendSurface(&s_sim_globe.surface, batches, 3);
  if (!ok) fprintf(stderr, "[sim-globe-underlay] surface batch rejected\n");
  if (ok && detailed_town) {
    /* Geographic focus marks the selected town; this separate ground cue
     * follows its smaller live sprite window. Both are draw-time materials,
     * never reasons to rebuild resident terrain/model sources on a pan. */
    const Sim3DDepthSurfaceFocus visibility =
        PresentSimGlobeFocus_ResolveVisibility(&map, &slot->sim);
    ok = PresentSimGlobeTerrain_Append(
             projection->matrix, map.radius,
             SimBackgroundVoxelRenderer_GroundTexture(slot->sim.background_voxel_serial),
             town_shadow.texture, town_shadow.opacity, &visibility) &&
         PresentSimGlobeMountains_Append(projection->matrix, map.radius) &&
         PresentSimGlobeWater_Append(
             projection->matrix, map.radius,
             SimBackgroundVoxelRenderer_GroundTexture(slot->sim.background_voxel_serial),
             &visibility);
    if (ok && content)
      ok = PresentSimGlobeMountains_AppendEffects(
          projection->matrix, viewport, slot->sim.game_frame, slot->sim.background_voxel_detail,
          slot->sim.background_voxel_style, SimGlobeMountainGround);
    if (!ok) fprintf(stderr, "[sim-globe-town] native surface/shadow append rejected\n");
  }
  return ok;
}

static bool AppendSimGlobeModels(const FrameSlot *slot, ArRenderRectI source,
                                 ArRenderRectI viewport, const float matrix[16],
                                 const PresentSimGlobeView *view, bool sim_facades,
                                 bool detailed_town, Sim3DDepthSurfaceTransform t) {
  const SimGlobeMapping map = view->map;
  bool ok = true;
  const SimWorldNavigationTowns *towns = &slot->sim.world_navigation_towns;
  /* The detailed town consumes the already published active SIM
   * identities, not navigation's approximation of currently resident art.
   * Borrowed only during this call; retained sources own their values. */
  const SimBackgroundVoxelObject *active_objects = NULL;
  size_t active_count = 0;
  if (detailed_town) {
    const SimBackgroundVoxelScene *active = SimBackgroundVoxels_Scene();
    if (active->overflow || active->town != map.town ||
        active->object_count > kSimBackgroundMaxObjects)
      ok = false;
    else {
      active_objects = active->objects;
      active_count = active->object_count;
    }
  }
  if (towns->overflow || towns->object_count > kSimWorldNavigationTownObjectCapacity) ok = false;
  const size_t candidates = towns->object_count + active_count;
  if (ok && candidates > g_world_nav_models.gpu_source_capacity) {
    void *buffer =
        realloc(g_world_nav_models.gpu_sources, candidates * sizeof(WorldNavigationModelSource));
    if (!buffer)
      ok = false;
    else {
      g_world_nav_models.gpu_sources = buffer;
      g_world_nav_models.gpu_source_capacity = candidates;
    }
  }
  WorldNavigationModelSource *sources = g_world_nav_models.gpu_sources;
  size_t count = 0, radial_count = 0;
  /* Active bridges use the town stream's separate visual/depth datums, but
   * retain geometric height. Neighbours keep navigation's radial stream. */
  for (unsigned group = 0; group < 2; ++group) {
    for (size_t i = 0; ok && i < candidates; ++i) {
      const SimWorldNavigationTownObject *object =
          i < towns->object_count ? &towns->objects[i] : &active_objects[i - towns->object_count];
      if (active_objects && i < towns->object_count && object->town == map.town) continue;
      if (object->town != map.town && !slot->sim.world_navigation_models) continue;
      const bool facing =
          object->town == map.town && (sim_facades || object->kind == kSimBackgroundVoxel_Bridge);
      if (facing != (group == 1)) continue;
      if (object->kind >= kSimBackgroundVoxelKindCount) continue;
      const SimBackgroundBridgeBounds bounds = WorldNavigationObjectBounds(object);
      int tx, ty;
      if (!SimWorldMap_OriginForTown(object->town, &tx, &ty)) {
        ok = false;
        break;
      }
      const float x = tx + (bounds.origin_x + bounds.width * .5f) / 16;
      const float y = ty + (bounds.origin_y + bounds.depth * .5f) / 16;
      if (!SimGlobeNearby(&map, x, y)) continue;
      if (count >= kSimWorldNavigationTownObjectCapacity) {
        ok = false;
        break;
      }
      WorldNavigationModelSource *s = &sources[count++];
      memset(s, 0, sizeof(*s));
      s->object = *object;
      if (object->kind == kSimBackgroundVoxel_Windmill && !(active_objects && facing))
        s->object.animation_phase = 0;
      s->detail = object->town == slot->sim.town
                      ? (SimBackgroundVoxelDetail)slot->sim.background_voxel_detail
                      : kSimBackgroundVoxelDetail_Low;
      s->object_index = active_objects ? count - 1 : i;
      s->source_x = tx * kSimWorldMapTilePixels + bounds.origin_x * .5f;
      s->source_y = ty * kSimWorldMapTilePixels + bounds.origin_y * .5f;
      s->centre_x = bounds.width * .5f;
      s->centre_y = bounds.depth * .5f;
      if (detailed_town && object->town == map.town) {
        if (!SimWorldNavigationTerrain_RegisterTownFloor(
                map.town, x - tx, y - ty,
                SimTownTerrain_HeightUnitsAt(map.town, (x - tx) * 16, (y - ty) * 16),
                &s->anchor_height)) {
          ok = false;
          break;
        }
      } else
        s->anchor_height = map.landscape > 0
                               ? WorldNavigationTerrainHeightAt(x * kSimWorldMapTilePixels,
                                                                y * kSimWorldMapTilePixels, NULL)
                               : 0;
      s->depth_height = s->anchor_height;
      if (object->town == map.town && object->kind == kSimBackgroundVoxel_Bridge) {
        float approach, envelope;
        if (!SimBackgroundBridge_TerrainHeights(object, map.town, &approach, &envelope) ||
            !SimWorldNavigationTerrain_RegisterTownFloor(map.town, x - tx, y - ty, approach,
                                                         &s->anchor_height) ||
            !SimWorldNavigationTerrain_RegisterTownFloor(map.town, x - tx, y - ty, envelope,
                                                         &s->depth_height)) {
          ok = false;
          break;
        }
      }
    }
    if (!group) radial_count = count;
  }
  WorldNavigationModelSourceStyle style;
  memset(&style, 0, sizeof(style));
  style.embedding = map;
  style.surface_revision = SimWorldMap_GeographySerial();
  style.chart_radius_tiles = map.chart_radius;
  style.tile_world = 1 / map.metric;
  style.height_percent = slot->sim.height_scale_x100;
  style.light_azimuth = slot->sim.light_azimuth_deg;
  style.light_elevation = slot->sim.light_elevation_deg;
  style.style = slot->sim.background_voxel_style;
  style.lighting = true;
  style.focus = t.focus;
  t.radial.variant = (unsigned)((slot->sim.game_frame / 12) % 3) + 1;
  if (ok && count) ok = WorldNavigationModelMesh_Enabled();
  if (ok && radial_count)
    ok = WorldNavigationModelMesh_Draw(sources, radial_count, &style, &t.radial);
  if (ok) {
    /* Active facades are wholly inside the clear town. Do not invalidate
     * their resident sources when a neighbour-only focus setting changes. */
    style.focus = (Sim3DDepthSurfaceFocus){0};
    style.captured_poses = active_objects != NULL;
    const SimBackgroundVoxelRenderParams params = {
        .source = source,
        .viewport = viewport,
        .matrix = matrix,
        .facing = slot->sim.background_voxel_facing,
    };
    SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
    SimBackgroundVoxelProject_ResolveAxes(&params, axes);
    ok = WorldNavigationModelMesh_DrawFacingTown(
        sources ? sources + radial_count : NULL, count - radial_count, &style,
        slot->sim.background_voxel_shading, axes, view->matrix, (unsigned)t.radial.variant - 1);
  }
  if (!ok) fprintf(stderr, "[sim-globe-underlay] models rejected count=%zu\n", count);
  return ok;
}

static PresentationOutcome DrawSimGlobeScene(const FrameSlot *slot, ArRenderRectI source,
                                             ArRenderRectI viewport, const Scene3DCamera *camera,
                                             const float matrix[16], float radius_scale,
                                             bool sim_facades, bool detailed_town,
                                             const PresentSimGlobeContent *content,
                                             PresentSimGlobeView *out_view) {
  WorldNavigationProjection projection;
  PresentSimGlobeView view;
  if (!PrepareSimGlobeView(slot, source, viewport, camera, matrix, radius_scale, sim_facades,
                           &projection, &view))
    return kPresentationOutcome_CoreFailure;
  const SimGlobeMapping map = view.map;
  if (out_view) *out_view = view;
  if (detailed_town) {
    SimBackgroundVoxelRenderParams params = SimVoxelRenderParams(slot, source, viewport, matrix);
    SimBackgroundVoxelProject_Prepare(&params);
    if (!PresentSimGlobeMountains_Prepare(&params, &map, SimGlobeMountainGround,
                                          SimWorldMap_GeographySerial())) {
      fprintf(stderr, "[sim-globe-town] native mountain preparation rejected\n");
      goto failed;
    }
    if (!PresentSimGlobeTerrain_Prepare(&map, SimWorldMap_GeographySerial())) {
      fprintf(stderr, "[sim-globe-town] native terrain preparation rejected\n");
      goto failed;
    }
  }
  PresentSimGlobeGroundShadow town_shadow = {0};
  if (content && content->prepare && !content->prepare(content->userdata, &view, &town_shadow)) {
    fprintf(stderr, "[sim-globe-town] dynamic content preparation rejected\n");
    goto failed;
  }
  if (!Sim3DDepthPass_Begin(&g_render_device, viewport.w, viewport.h, kArRenderFilter_Linear))
    goto failed;
  if (!SimGlobeBuildSurface(slot, &projection, &map, detailed_town)) goto failed;
  const Sim3DDepthSurfaceTransform transform = SimGlobeSurfaceTransform(slot, &view);
  bool ok = AppendSimGlobeSurfaces(slot, source, viewport, &projection, &map, detailed_town,
                                   content, &town_shadow, transform);
  if (ok)
    ok = AppendSimGlobeModels(slot, source, viewport, matrix, &view, sim_facades, detailed_town,
                              transform);
  if (ok && content) ok = content->append && content->append(content->userdata, &view);
  ArRenderTexture composite = Sim3DDepthPass_Submit(&g_render_device, ArRenderTexture_Invalid());
  if (!ArRenderTexture_IsValid(composite))
    fprintf(stderr, "[sim-globe-underlay] composite rejected\n");
  if (!ok || !ArRenderTexture_IsValid(composite)) goto failed;
  return DrawSimGlobeImage(viewport, composite);
failed:
  if (Sim3DDepthPass_IsCollecting())
    (void)Sim3DDepthPass_Submit(&g_render_device, ArRenderTexture_Invalid());
  fprintf(stderr, "[sim-globe-underlay] selected connected world could not be rendered\n");
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






void ResetWorldNavigationGlobeSurfaces(void) {
  PresentSimGlobeMountains_Reset();
  PresentSimGlobeTerrain_Reset();
  PresentSimGlobeWater_Reset();
  Sim3DMeshSet_Destroy(&s_sim_globe.surface);
  memset(&s_sim_globe, 0, sizeof(s_sim_globe));
  Sim3DDepthPass_DestroyMesh(g_world_nav_surfaces.mesh);
  memset(&g_world_nav_surfaces, 0, sizeof(g_world_nav_surfaces));
  Sim3DMeshSet_Destroy(&g_world_nav_gpu_grid.meshes);
  memset(&g_world_nav_gpu_grid, 0, sizeof(g_world_nav_gpu_grid));
  free(g_world_nav_shells.atmosphere_draw.vertices);
  free(g_world_nav_shells.atmosphere_draw.indices);
  g_world_nav_shells.atmosphere_draw = (WorldNavigationAtmosphereDrawCache){0};
  g_world_nav_shells.ocean.ready = g_world_nav_shells.cloud.ready =
      g_world_nav_shells.atmosphere.ready = false;
}
