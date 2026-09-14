#include "present_sim_globe_mountains.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sim/sim3d_mesh_set.h"
#include "sim/sim_background_mountain_render.h"
#include "sim/sim_background_voxels.h"
#include "sim/sim_town_terrain.h"
#include "sim/sim_world_navigation_terrain.h"

/* One bounded source. Cache the resolved source recipe, not the camera matrix:
 * pan, fixed-LOD zoom and unrelated model animation do not rebuild mountains.
 * Genuine facing/stack-direction/LOD changes still rebuild authored relief. */
typedef struct MountainKey {
  SimGlobeMapping map;
  SimBackgroundMountainSourceStyle style;
  uint32_t geography;
} MountainKey;
static struct {
  Sim3DMeshSet meshes;
  MountainKey key;
  SimBackgroundMountainField field;
  SimBackgroundMountainCaps caps;
  SimBackgroundMountainEffectSource effects;
  SimBackgroundCraterAnchor crater;
  uint32_t scene_serial;
  bool valid;
  bool published;
  uint64_t revision;
  size_t count;
  Sim3DDepthSurfaceVertex *vertices;
} s_mountains;

typedef struct MountainBuilder {
  const SimGlobeMapping *map;
  PresentSimGlobeGroundSample sample;
  Sim3DDepthSurfaceVertex *vertices;
  size_t count, capacity;
  bool failed;
} MountainBuilder;

static bool MountainPoint(const SimGlobeMapping *map, PresentSimGlobeGroundSample sample,
    float x, float y, float z, const SimBackgroundProjectionAxis *axis, float point[3]) {
  const float cx=map->origin_x+x/16, cy=map->origin_y+y/16;
  float floor, normal[3], metric;
  if (x>=0 && x<=512 && y>=0 && y<=512) {
    if (!SimWorldNavigationTerrain_RegisterTownFloor(map->town,x/16,y/16,
        SimTownTerrain_HeightUnitsAt(map->town,fminf(x,511.999f),fminf(y,511.999f)),&floor)) return false;
  } else if (sample) floor=sample(cx,cy);
  else return false;
  if (!SimGlobeMapping_Point(map,cx,cy,floor,0,point) ||
      !SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius,cx,cy,normal,&metric)) return false;
  const float height=z/16*metric/map->metric;
  point[0]+=axis->x_per_height*height;
  point[1]-=axis->y_per_height*height;
  point[2]+=axis->height_scale*height;
  return true;
}

static bool ResolveCurvedCrater(const SimGlobeMapping *map,
    const SimBackgroundMountainEffectSource *effects, PresentSimGlobeGroundSample sample,
    SimBackgroundCraterAnchor *out) {
  *out=(SimBackgroundCraterAnchor){0};
  if (!effects->count) return true;
  const SimBackgroundCraterSource *c=&effects->craters[effects->count-1];
  float point[3], x,y,height,floor;
  if (!MountainPoint(map,sample,c->x,c->y,c->z,&c->axis,point) ||
      !SimGlobeMapping_Source(map,point,&x,&y,&height)) return false;
  x=(x-map->origin_x)*16; y=(y-map->origin_y)*16;
  if (!SimWorldNavigationTerrain_RegisterTownFloor(map->town,x/16,y/16,
      SimTownTerrain_HeightUnitsAt(map->town,x,y),&floor)) return false;
  *out=(SimBackgroundCraterAnchor){true,x,y,
      (height-floor*map->landscape/map->metric)*16};
  return true;
}

static void EmbedMountain(void *user, const float x[4], const float y[4],
    const float z[4], const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainMeshUV uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4]) {
  MountainBuilder *b = user;
  if (b->failed) return;
  if (b->count == b->capacity) {
    size_t capacity = b->capacity ? b->capacity * 2 : 1024;
    if (capacity > kSim3DMeshSetMaximumQuads) capacity = kSim3DMeshSetMaximumQuads;
    if (capacity <= b->count) { b->failed = true; return; }
    void *vertices = realloc(b->vertices,capacity * 4 * sizeof(*b->vertices));
    if (!vertices) { b->failed = true; return; }
    b->vertices = vertices; b->capacity = capacity;
  }
  const SimGlobeMapping *map = b->map;
  for (int p = 0; p < 4; ++p) {
    float point[3];
    if (!MountainPoint(map,b->sample,x[p],y[p],z[p],axis,point)) {
      b->failed = true; return;
    }
    point[2] += map->radius;
    const float length = sqrtf(point[0]*point[0]+point[1]*point[1]+point[2]*point[2]);
    if (!isfinite(length) || length <= 0) { b->failed = true; return; }
    const float shade = brightness[p]/255.0f;
    Sim3DDepthSurfaceVertex *v = &b->vertices[b->count*4+p];
    *v = (Sim3DDepthSurfaceVertex){
      .normal = {point[0]/length,point[1]/length,point[2]/length},
      .elevation = {length-map->radius,0},
      .color = {shade,shade,shade,alpha[p]/255.0f}, .uv = {uv[p].x,uv[p].y}};
  }
  ++b->count;
}

bool PresentSimGlobeMountains_Prepare(
    const SimBackgroundVoxelRenderParams *params, const SimGlobeMapping *map,
    PresentSimGlobeGroundSample sample, uint32_t geography) {
  if (!params || !params->matrix || !map || !sample ||
      !map->town || params->town != map->town ||
      !SimBackgroundVoxelRenderer_Ready(params->serial) ||
      SimBackgroundVoxels_Scene()->town != map->town) return false;
  MountainKey key;
  memset(&key,0,sizeof(key));
  key.map = *map; key.geography = geography;
  if (!SimBackgroundMountainRender_SourceStyle(params,&key.style)) return false;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  const uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  if (s_mountains.valid && !memcmp(&key,&s_mountains.key,sizeof(key)) &&
      (s_mountains.scene_serial == scene_serial ||
       (!memcmp(&s_mountains.field,&scene->mountains,sizeof(s_mountains.field)) &&
        !memcmp(&s_mountains.caps,&scene->mountain_caps,sizeof(s_mountains.caps))))) {
    s_mountains.scene_serial = scene_serial;
    return true;
  }
  MountainBuilder b = {.map = map,.sample = sample};
  const int count = SimBackgroundMountainRender_EmitSource(params,EmbedMountain,&b);
  SimBackgroundMountainEffectSource effects;
  SimBackgroundCraterAnchor crater;
  const bool ok = !b.failed && count >= 0 && (size_t)count == b.count &&
      SimBackgroundMountainRender_EffectSource(params,&effects) &&
      ResolveCurvedCrater(map,&effects,sample,&crater);
  s_mountains.valid = ok;
  if (ok) {
    free(s_mountains.vertices); s_mountains.vertices = b.vertices;
    s_mountains.key = key; s_mountains.count = b.count;
    s_mountains.field = scene->mountains; s_mountains.caps = scene->mountain_caps;
    s_mountains.scene_serial = scene_serial;
    s_mountains.effects=effects; s_mountains.crater=crater;
    s_mountains.published = false; ++s_mountains.revision;
  } else free(b.vertices);
  return ok;
}

uint64_t PresentSimGlobeMountains_Revision(void) { return s_mountains.revision; }

bool PresentSimGlobeMountains_CraterAnchor(SimBackgroundCraterAnchor *out) {
  if (!out) return false;
  *out=s_mountains.valid ? s_mountains.crater : (SimBackgroundCraterAnchor){0};
  return out->valid;
}

typedef struct MountainEffectProjection {
  const float *matrix;
  ArRenderRectI viewport;
  PresentSimGlobeGroundSample sample;
} MountainEffectProjection;
static bool AppendCurvedEffect(void *user, const float x[4], const float y[4],
    const float z[4], const SimBackgroundProjectionAxis *axis, ArRenderColorF color) {
  const MountainEffectProjection *projection=user;
  Sim3DDepthVertex vertices[4];
  for (unsigned p=0;p<4;++p) {
    float point[3]; Scene3DClipPoint clip;
    if (!MountainPoint(&s_mountains.key.map,projection->sample,x[p],y[p],z[p],axis,point) ||
        !Scene3D_TransformToClip(projection->matrix,point[0],point[1],point[2],&clip)) return false;
    if (clip.w<=kScene3DMinimumProjectionDepth) return true;
    vertices[p]=(Sim3DDepthVertex){
      (clip.x/clip.w*.5f+.5f)*projection->viewport.w,
      (.5f-clip.y/clip.w*.5f)*projection->viewport.h,
      clip.z/clip.w*.5f+.5f,color,{-1,-1}};
  }
  return Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Effect,vertices);
}

bool PresentSimGlobeMountains_AppendEffects(const float matrix[16], ArRenderRectI viewport,
    uint16_t frame, uint8_t detail, uint8_t style, PresentSimGlobeGroundSample sample) {
  if (!s_mountains.valid || !matrix || !sample || viewport.w<=0 || viewport.h<=0) return false;
  const MountainEffectProjection projection={matrix,viewport,sample};
  return SimBackgroundMountainRender_EmitEffects(&s_mountains.effects,frame,detail,style,
      AppendCurvedEffect,(void *)&projection);
}

bool PresentSimGlobeMountains_Append(const float matrix[16], float radius) {
  if (!s_mountains.valid) return false;
  if (!s_mountains.published) {
    if (!Sim3DMeshSet_UpdateSurface(&s_mountains.meshes,
            s_mountains.vertices,NULL,s_mountains.count)) return false;
    free(s_mountains.vertices); s_mountains.vertices = NULL;
    s_mountains.published = true;
  }
  if (!s_mountains.count) return true;
  Sim3DDepthSurfaceBatch batch = {.layer = kSim3DDepthPass_Mountain,
    .range = {0,s_mountains.count},
    .transform = {.radial = {.sphere_radius = radius,.height_scale = 1},.ambient = 1}};
  memcpy(batch.transform.radial.matrix,matrix,sizeof(batch.transform.radial.matrix));
  for (int i = 0; i < 3; ++i) batch.transform.radial.basis[i][i] = 1;
  return Sim3DMeshSet_AppendSurface(&s_mountains.meshes,&batch,1);
}

void PresentSimGlobeMountains_Reset(void) {
  free(s_mountains.vertices);
  Sim3DMeshSet_Destroy(&s_mountains.meshes);
  memset(&s_mountains,0,sizeof(s_mountains));
}
