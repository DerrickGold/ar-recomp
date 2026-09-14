#include "present_sim_globe_terrain.h"
#include "present_sim3d_terrain.h"
#include "sim/sim3d_mesh_set.h"
#include "sim/sim_world_navigation_terrain.h"
#include <stdlib.h>
#include <string.h>

enum { kMaximumTerrainTops = 32 * 32, kMaximumTerrainQuads = 32 * 32 * 5 };
static struct {
  SimGlobeMapping map;
  uint32_t geography;
  bool valid, published;
  Sim3DMeshSet meshes;
  Sim3DDepthSurfaceVertex *vertices;
  size_t count, tops;
} s_terrain;

#if AR_SIM3D_TERRAIN_ELEVATION
typedef struct TerrainBuilder {
  const SimGlobeMapping *map;
  Sim3DDepthSurfaceVertex *vertices;
  size_t tops, cliffs;
} TerrainBuilder;

static bool EmbedTerrain(void *user, const float xy[4][2],
    const float heights[4], const float uv[4][2], const float shade[4]) {
  TerrainBuilder *b = user;
  /* Cliff faces collapse to a line in native XY. Keep tops as one contiguous
   * receiver range so shadows never need a duplicate mesh or touch walls. */
  /* Differences keep collapsed edges exactly zero even with fused arithmetic;
   * subtracting large absolute-coordinate cross products loses that property. */
  const float area = (xy[1][0]-xy[0][0])*(xy[3][1]-xy[0][1]) -
      (xy[1][1]-xy[0][1])*(xy[3][0]-xy[0][0]);
  const bool top = area != 0;
  if ((top && b->tops == kMaximumTerrainTops) ||
      (!top && b->cliffs == kMaximumTerrainQuads-kMaximumTerrainTops)) return false;
  const size_t index = top ? b->tops : kMaximumTerrainTops+b->cliffs;
  const SimGlobeMapping *map = b->map;
  for (int p = 0; p < 4; ++p) {
    float height;
    const float x = xy[p][0]/16, y = xy[p][1]/16;
    Sim3DDepthSurfaceVertex *v = &b->vertices[index*4+p];
    *v = (Sim3DDepthSurfaceVertex){.uv = {uv[p][0],uv[p][1]},
      .color = {shade[p],shade[p],shade[p],1}};
    if (!SimWorldNavigationTerrain_RegisterTownFloor(map->town,x,y,heights[p],&height) ||
        !SimGlobeMapping_Encode(map,map->origin_x+x,map->origin_y+y,
            height,0,v->normal,v->elevation)) return false;
  }
  if (top) ++b->tops; else ++b->cliffs;
  return true;
}
#endif

bool PresentSimGlobeTerrain_Prepare(const SimGlobeMapping *map, uint32_t geography) {
  if (!map || !map->town) return false;
  if (s_terrain.valid && s_terrain.geography == geography &&
      !memcmp(map,&s_terrain.map,sizeof(*map))) return true;
  s_terrain.valid = false;
#if AR_SIM3D_TERRAIN_ELEVATION
  TerrainBuilder b = {.map = map,
    .vertices = malloc(kMaximumTerrainQuads*4*sizeof(*b.vertices))};
  if (!b.vertices) return false;
  if (!EmitSimTownTerrainSource(map->town,(uint16_t)(map->town_landscape*100+.5f),EmbedTerrain,&b)) {
    free(b.vertices); return false;
  }
  free(s_terrain.vertices); s_terrain.vertices = b.vertices;
  memmove(b.vertices+b.tops*4,b.vertices+kMaximumTerrainTops*4,
      b.cliffs*4*sizeof(*b.vertices));
  s_terrain.map = *map; s_terrain.geography = geography;
  s_terrain.count = b.tops+b.cliffs; s_terrain.tops = b.tops;
  s_terrain.valid = true; s_terrain.published = false;
  return true;
#else
  return false;
#endif
}

bool PresentSimGlobeTerrain_Append(const float matrix[16], float radius,
    ArRenderTexture ground, ArRenderTexture shadow, float shadow_opacity) {
  if (!s_terrain.valid || !ArRenderTexture_IsValid(ground)) return false;
  if (!s_terrain.published) {
    if (!Sim3DMeshSet_UpdateSurface(&s_terrain.meshes,
            s_terrain.vertices,NULL,s_terrain.count)) return false;
    free(s_terrain.vertices); s_terrain.vertices = NULL;
    s_terrain.published = true;
  }
  Sim3DDepthSurfaceBatch batch = {.layer = kSim3DDepthPass_Ground,
    .texture = ground,.range = {0,s_terrain.count},
    /* The source already contains SIM's slope/contact/skirt shading. */
    .transform = {.radial = {.sphere_radius = radius,.height_scale = 1},.ambient = 1}};
  memcpy(batch.transform.radial.matrix,matrix,sizeof(batch.transform.radial.matrix));
  for (int i = 0; i < 3; ++i) batch.transform.radial.basis[i][i] = 1;
  if (!ArRenderTexture_IsValid(shadow) || shadow_opacity <= 0)
    return Sim3DMeshSet_AppendSurface(&s_terrain.meshes,&batch,1);
  Sim3DDepthSurfaceOverlay overlay = {.layer = kSim3DDepthPass_ShadowReceiver,
    .color = {0,0,0,shadow_opacity},.texture = shadow};
  Sim3DDepthSurfaceBatch batches[2] = {batch,batch};
  batches[0].range.quad_count = s_terrain.tops;
  batches[0].overlays = &overlay; batches[0].overlay_count = 1;
  batches[1].range = (Sim3DDepthMeshRange){s_terrain.tops,s_terrain.count-s_terrain.tops};
  return Sim3DMeshSet_AppendSurface(&s_terrain.meshes,batches,2);
}

void PresentSimGlobeTerrain_Reset(void) {
  free(s_terrain.vertices);
  Sim3DMeshSet_Destroy(&s_terrain.meshes);
  memset(&s_terrain,0,sizeof(s_terrain));
}
