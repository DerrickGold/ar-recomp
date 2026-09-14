#include "present_sim_globe_water.h"
#include "sim/sim3d_mesh_set.h"
#include "sim/sim_town_ground_art.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
  kWaterFeatherCells = 4,
  kWaterAxis = kSimTownCells + 2 * kWaterFeatherCells,
  /* One extra pixel on each side for conservative Scale2x/filter protection. */
  kWaterMaskAxis = kWaterAxis * kSimTownCellPixels + 2,
};
static struct {
  Sim3DMeshSet mesh;
  SimGlobeMapping map;
  SimWorldNavigationTownGround ground;
  bool ready;
  size_t count;
} s_water;

static bool NativeWater(const SimWorldNavigationTownGround *ground,
    uint8_t town, int x, int y) {
  return (ground->enabled_town_mask & (1u << (town - 1))) &&
      !(ground->object_rows[town - 1][y] & (UINT32_C(1) << x)) &&
      SimTownGroundArt_IsOpenWater(town, ground->development_tier[town - 1],
          ground->terrain[town - 1][y * kSimTownCells + x]);
}

static bool DestinationMask(const SimWorldNavigationTownGround *ground,
    int x, int y, uint8_t mask[256]) {
  /* Outside the authored chart is the ocean sphere. Inside it, protect
   * every non-water pixel in BOTH the developed overview and native art.
   * Never use the pristine map (Bloodpool may already have been cleansed). */
  if (x < 0 || y < 0 || x >= kSimWorldMapTiles || y >= kSimWorldMapTiles) {
    memset(mask, 1, 256);
    return true;
  }
  uint8_t overview[64];
  if (!SimWorldMap_OpenWaterMask(x, y, overview)) return false;
  for (int py = 0; py < 16; ++py) for (int px = 0; px < 16; ++px)
    mask[py * 16 + px] = overview[(py / 2) * 8 + px / 2];
  for (uint8_t town = 1; town <= kSimTownCount; ++town) {
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) return false;
    if (x < ox || x >= ox + 32 || y < oy || y >= oy + 32 ||
        !(ground->enabled_town_mask & (1u << (town - 1)))) continue;
    const int cx = x - ox, cy = y - oy;
    if (ground->object_rows[town - 1][cy] & (UINT32_C(1) << cx)) {
      memset(mask, 0, 256);
    } else {
      uint8_t native[256];
      if (!SimTownGroundArt_OpenWaterMask(town, ground->development_tier[town - 1],
              ground->terrain[town - 1][cy * 32 + cx], native)) return false;
      for (int p = 0; p < 256; ++p) mask[p] &= native[p];
    }
  }
  return true;
}

static float EdgeDistance(float x, float y) {
  return hypotf(fmaxf(0, fmaxf(-x, x - 32)), fmaxf(0, fmaxf(-y, y - 32)));
}

/* A whole-water source keeps live wave/palette updates on the existing native
 * atlas. A coastal boundary cell need not itself be completely water: select
 * its nearest safe source instead of leaving a rectangular gap in the sea. */
static int SourceCell(const uint8_t water[32 * 32], int x, int y) {
  int best = -1, distance = INT32_MAX;
  for (int p = 0; p < 32 * 32; ++p) {
    if (!water[p]) continue;
    const int dx = p % 32 - x, dy = p / 32 - y, d = dx * dx + dy * dy;
    if (d < distance) { best = p; distance = d; }
  }
  return best;
}

typedef struct WaterBuilder {
  const SimGlobeMapping *map;
  const uint8_t *coverage;
  const Sim3DDepthSurfaceVertex *grid;
  uint8_t sources[32 * 32];
  Sim3DDepthSurfaceVertex *vertices;
  ArRenderPointF *focus;
  size_t count, capacity;
} WaterBuilder;

static bool EmitPolygon(WaterBuilder *b, int x, int y, int source,
    const ArRenderPointF *uv, unsigned corners) {
  if (corners > 4) {
    /* A rectangle split by the parent diagonal can leave a pentagon. */
    const ArRenderPointF triangle[3] = {uv[0],uv[3],uv[4]};
    return EmitPolygon(b,x,y,source,uv,4) && EmitPolygon(b,x,y,source,triangle,3);
  }
  if (!b->vertices) { ++b->count; return true; }
  if (b->count >= b->capacity) return false;
  const SimGlobeMapping *map = b->map;
  const int sx = source % 32, sy = source / 32;
  const int gx = (int)map->origin_x + x, gy = (int)map->origin_y + y;
  const bool on_chart = gx >= 0 && gy >= 0 && gx < 128 && gy < 128;
  float positions[4][3];
  if (on_chart) {
    const int first = gy * 129 + gx, indices[4] = {first,first+1,first+130,first+129};
    for (int p=0;p<4;++p) {
      const Sim3DDepthSurfaceVertex *v=&b->grid[indices[p]];
      for (int a=0;a<3;++a)
        positions[p][a]=v->normal[a]*(map->radius+v->elevation[0]+.0005f);
    }
  }
  for (unsigned p = 0; p < 4; ++p) {
    const float u=uv[p<corners?p:corners-1].x, v=uv[p<corners?p:corners-1].y;
    const float px = x + u, py = y + v, cx = map->origin_x + px, cy = map->origin_y + py;
    const float t = fminf(1, EdgeDistance(px, py) / kWaterFeatherCells);
    const float fade = t * t * (3 - 2 * t);
    /* Same half-texel interval as a native top. Splitting a coastal tile does
     * not restart its wave UVs, shade, or fade at each little rectangle. */
    Sim3DDepthSurfaceVertex *out = &b->vertices[b->count * 4 + p];
    *out = (Sim3DDepthSurfaceVertex){.color = {1, 1, 1, 1 - fade},
      .uv = {(sx * 16 + .5f + u * 15) / 512, (sy * 16 + .5f + v * 15) / 512}};
    if (on_chart) {
      /* Interpolate positions on the original 0-2 triangle diagonal. Sampling
       * terrain again here sinks the material into non-linear coastal slopes.
       * The tiny radial bias only separates coincident materials, not heights. */
      const float weights[4] = {1-fmaxf(u,v),fmaxf(0,u-v),fminf(u,v),fmaxf(0,v-u)};
      float point[3]={0};
      for (int q=0;q<4;++q) for (int a=0;a<3;++a) point[a]+=weights[q]*positions[q][a];
      const float length=hypotf(hypotf(point[0],point[1]),point[2]);
      if (!(length>0) || !isfinite(length)) return false;
      for (int a=0;a<3;++a) out->normal[a]=point[a]/length;
      out->elevation[0]=length-map->radius;
    } else if (!SimGlobeMapping_Encode(map,cx,cy,0,.0005f,out->normal,out->elevation)) return false;
    b->focus[b->count * 4 + p] = (ArRenderPointF){cx / 128, cy / 128};
  }
  ++b->count;
  return true;
}

static bool EmitRectangle(WaterBuilder *b, int x, int y, int source,
    int left, int top, int width, int height) {
  const float l=left/16.0f, t=top/16.0f, r=(left+width)/16.0f, d=(top+height)/16.0f;
  const ArRenderPointF rect[4]={{l,t},{r,t},{r,d},{l,d}};
  if (r<=t || d<=l || (l==t && r==d)) return EmitPolygon(b,x,y,source,rect,4);
  /* Clip the rectangle against each side of the parent triangle diagonal.
   * No output polygon straddles two different ground planes. */
  for (int side=-1;side<=1;side+=2) {
    ArRenderPointF polygon[6]; unsigned count=0;
    for (unsigned p=0;p<4;++p) {
      const ArRenderPointF a=rect[p], c=rect[(p+1)%4];
      const float da=side*(a.x-a.y), dc=side*(c.x-c.y);
      if (da>=0) polygon[count++]=a;
      if ((da<0 && dc>0) || (da>0 && dc<0)) {
        const float ratio=da/(da-dc), diagonal=a.x+(c.x-a.x)*ratio;
        polygon[count++]=(ArRenderPointF){diagonal,diagonal};
      }
    }
    if (count>=3 && !EmitPolygon(b,x,y,source,polygon,count)) return false;
  }
  return true;
}

static bool BuildRectangles(WaterBuilder *b) {
  for (int y = -kWaterFeatherCells; y < 32 + kWaterFeatherCells; ++y)
    for (int x = -kWaterFeatherCells; x < 32 + kWaterFeatherCells; ++x) {
      if (x >= 0 && x < 32 && y >= 0 && y < 32) continue;
      if (EdgeDistance(x < 0 ? x + 1 : x, y < 0 ? y + 1 : y) >= kWaterFeatherCells) continue;
      const int source = SourceCell(b->sources, x, y);
      if (source < 0) continue;
      uint16_t rows[16] = {0};
      for (int py = 0; py < 16; ++py) for (int px = 0; px < 16; ++px) {
        const int at = ((y + kWaterFeatherCells) * 16 + py + 1) * kWaterMaskAxis +
            (x + kWaterFeatherCells) * 16 + px + 1;
        bool water = true;
        /* Scale2x can move a shore by one native pixel. This one-pixel guard
         * includes neighbouring cells, so a mixed tile never repaints land,
         * sand, foam, marsh or buildings, even with linear atlas sampling. */
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
          water &= b->coverage[at + dy * kWaterMaskAxis + dx] != 0;
        if (water) rows[py] |= (uint16_t)(1u << px);
      }
      /* Compact semantic coverage into disjoint rectangles. Full ocean is
       * still one quad/cell; only mixed shores need sub-cell geometry. */
      for (int py = 0; py < 16; ++py) for (int px = 0; px < 16; ++px) {
        if (!(rows[py] & (1u << px))) continue;
        int width = 1, height = 1;
        while (px + width < 16 && (rows[py] & (1u << (px + width)))) ++width;
        const uint16_t bits = (uint16_t)(((1u << width) - 1) << px);
        while (py + height < 16 && (rows[py + height] & bits) == bits) ++height;
        for (int dy = 0; dy < height; ++dy) rows[py + dy] &= (uint16_t)~bits;
        if (!EmitRectangle(b, x, y, source, px, py, width, height)) return false;
      }
    }
  return true;
}

bool PresentSimGlobeWater_Matches(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground) {
  return map && ground && s_water.ready && Sim3DMeshSet_Ready(&s_water.mesh) &&
      !memcmp(map,&s_water.map,sizeof(*map)) && !memcmp(ground,&s_water.ground,sizeof(*ground));
}

bool PresentSimGlobeWater_Prepare(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground, const Sim3DDepthSurfaceVertex *grid) {
  int ox,oy;
  if (!map || !ground || !grid || !map->town ||
      !SimWorldMap_OriginForTown(map->town,&ox,&oy) ||
      map->origin_x!=ox || map->origin_y!=oy) return false;
  s_water.ready = false;
  uint8_t *coverage = calloc(kWaterMaskAxis * kWaterMaskAxis, 1);
  if (!coverage) return false;
  bool ok = true;
  for (int y = 0; y < kWaterAxis && ok; ++y) for (int x = 0; x < kWaterAxis && ok; ++x) {
    uint8_t cell[256];
    ok = DestinationMask(ground, (int)map->origin_x + x - kWaterFeatherCells,
        (int)map->origin_y + y - kWaterFeatherCells, cell);
    if (ok) for (int py = 0; py < 16; ++py)
      memcpy(coverage + (y * 16 + py + 1) * kWaterMaskAxis + x * 16 + 1, cell + py * 16, 16);
  }
  WaterBuilder b = {.map = map, .coverage = coverage, .grid = grid};
  for (int p = 0; p < 32 * 32; ++p) b.sources[p] = NativeWater(ground, map->town, p % 32, p / 32);
  ok = ok && BuildRectangles(&b) && b.count <= kSim3DMeshSetMaximumQuads;
  if (ok && b.count) {
    b.capacity = b.count;
    b.vertices = malloc(b.capacity * 4 * sizeof(*b.vertices));
    b.focus = malloc(b.capacity * 4 * sizeof(*b.focus));
    b.count = 0;
    ok = b.vertices && b.focus && BuildRectangles(&b) && b.count == b.capacity;
  }
  ok = ok && Sim3DMeshSet_UpdateSurface(&s_water.mesh, b.vertices, b.focus, b.count);
  free(b.vertices); free(b.focus); free(coverage);
  s_water.ready = ok;
  if (ok) { s_water.map = *map; s_water.ground = *ground; s_water.count = b.count; }
  return ok;
}

bool PresentSimGlobeWater_Append(const float matrix[16], float radius,
    ArRenderTexture ground, const Sim3DDepthSurfaceFocus *focus) {
  if (!s_water.ready || !matrix || !focus || !ArRenderTexture_IsValid(ground)) return false;
  if (!s_water.count) return true;
  Sim3DDepthSurfaceBatch batch = {.layer = kSim3DDepthPass_Ground, .texture = ground,
    .range = {0, s_water.count}, .transform = {.radial = {.sphere_radius = radius, .height_scale = 1},
      .ambient = 1, .focus = *focus}};
  memcpy(batch.transform.radial.matrix, matrix, sizeof(batch.transform.radial.matrix));
  for (unsigned i = 0; i < 3; ++i) batch.transform.radial.basis[i][i] = 1;
  return Sim3DMeshSet_AppendSurface(&s_water.mesh, &batch, 1);
}

size_t PresentSimGlobeWater_QuadCount(void) { return s_water.ready ? s_water.count : 0; }

void PresentSimGlobeWater_Reset(void) {
  Sim3DMeshSet_Destroy(&s_water.mesh); memset(&s_water, 0, sizeof(s_water));
}
