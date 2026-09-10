#include "sim_world_navigation_mountains.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sim_background_mountain_objects.h"
#include "sim_background_mountain_silhouette.h"
#include "sim_town_ground_art.h"
#include "sim_world_navigation_art.h"
#include "snes_bgr555.h"

enum {
  kAtlas = kSimWorldNavigationMountainAtlasPixels,
  /* Bounded scene budget for fronts, walls and clipped rear strips.
   * Overflow fails the entire optional stage back to overview relief. */
  kMaxFaces = kSimTownCount * kSimBackgroundMountainMaxObjects *
      kSimBackgroundMountainObjectMaxRows *
      kSimBackgroundMountainObjectMaxColumns * (3 + kSimTownCellPixels),
};

typedef struct FaceWriter {
  SimWorldNavigationMountainScene *scene;
  const SimWorldNavigationTownGround *ground;
  int origin_x, origin_y;
  uint8_t town;
  uint8_t tile;
  unsigned variant;
  bool (*loaded)[256];
  bool failed, rear_slope, exterior, capped_volcano, summit_roof;
} FaceWriter;

static void RetainFace(
    void *user, const float x[4], const float y[4], const float z[4],
    const SimBackgroundMountainMeshUV uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4]) {
  (void)alpha;  /* Native mountain cutout alpha is in the immutable atlas. */
  FaceWriter *writer = user;
  SimWorldNavigationMountainScene *scene = writer->scene;
  if (writer->failed) return;
  if (scene->face_count == scene->face_capacity) {
    size_t capacity = scene->face_capacity ? scene->face_capacity * 2 : 1024;
    if (capacity > kMaxFaces) capacity = kMaxFaces;
    if (capacity <= scene->face_capacity) { writer->failed = true; return; }
    void *faces = realloc(scene->faces, capacity * sizeof(*scene->faces));
    if (!faces) { writer->failed = true; return; }
    scene->faces = faces;
    scene->face_capacity = capacity;
  }
  SimWorldNavigationMountainFace *face = &scene->faces[scene->face_count++];
  for (int p = 0; p < 4; p++) {
    face->x[p] = writer->origin_x + x[p] / kSimTownCellPixels;
    face->y[p] = writer->origin_y + y[p] / kSimTownCellPixels;
    face->z[p] = z[p] / kSimTownCellPixels;
    face->uv[p] = uv[p];
    face->brightness[p] = brightness[p];
    scene->maximum_rise = fmaxf(scene->maximum_rise, face->z[p]);
    scene->town_maximum_rise[writer->town - 1] = fmaxf(
        scene->town_maximum_rise[writer->town - 1], face->z[p]);
  }
  face->town = writer->town;
  face->rear_slope = writer->rear_slope;
  face->summit_roof = writer->summit_roof;
  face->exterior = writer->exterior;
}

void SimWorldNavigationMountains_Destroy(SimWorldNavigationMountainScene *scene) {
  if (!scene) return;
  free(scene->faces);
  free(scene->atlas);
  memset(scene, 0, sizeof(*scene));
}

static bool ObjectCell(const SimBackgroundMountainObject *object, int x, int y) {
  return x >= 0 && x < object->width_cells && y >= 0 &&
      y < object->height_cells && (object->row_occupied_mask[y] & (1u << x));
}

static bool LoadOverheadCrater(SimWorldNavigationMountainScene *scene, uint8_t tier) {
  /* The intact overhead opening is the 2x2 $A6/$A7/$B6/$B7 stamp at
   * Aitos world cells (24,43)..(25,44). Source tile identity is immutable;
   * selecting another town or rebuilding developed ground cannot move it. */
  const uint8_t tiles[4] = {0xA6, 0xA7, 0xB6, 0xB7};
  uint32_t pixels[4][64];
  uint8_t indices[4][64];
  for (int tile = 0; tile < 4; tile++)
    if (!SimWorldMap_CopyTileArt(tiles[tile], pixels[tile], indices[tile])) return false;
  /* Keep the overhead outline, but use the same native rock family as the
   * attached slopes. Sorting these brown shades preserves the categorical
   * world $40..$45 dark-to-light ordering, independent of animation. */
  const uint32_t *rock = SimTownGroundArt_Metatile(4, tier, 0x89);
  uint32_t shades[16];
  int shade_count = 0;
  if (!rock) return false;
  for (int p = 0; p < 256; p++) {
    const uint32_t color = rock[p];
    if (!(color >> 24)) continue;
    int at = 0;
    while (at < shade_count && shades[at] < color) at++;
    if (at < shade_count && shades[at] == color) continue;
    if (shade_count == 16) return false;
    memmove(shades + at + 1, shades + at, (shade_count - at) * sizeof(*shades));
    shades[at] = color;
    shade_count++;
  }
  memset(scene->lava_mask, 0, sizeof(scene->lava_mask));
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      const int tile = y / 8 * 2 + x / 8, p = y % 8 * 8 + x % 8;
      const uint8_t index = indices[tile][p];
      const uint32_t color = shade_count && index >= 0x40 && index <= 0x45
          ? shades[(index - 0x40) * (shade_count - 1) / 5] : pixels[tile][p];
      scene->atlas[(kSimWorldNavigationCraterAtlasY + y) * kAtlas +
          kSimWorldNavigationCraterAtlasX + x] = color;
      /* World index zero is the black opening, not transparency. Retain
       * the same native red pulse on this one aperture, not both slopes. */
      scene->lava_mask[0][y * 16 + x] = indices[tile][p] == 0;
    }
  scene->overhead_crater = true;
  scene->lava_tiles = 1;
  scene->lava_ready = false;
  return true;
}

static bool LoadTile(FaceWriter *writer, unsigned variant, uint8_t tile) {
  if (writer->loaded[variant][tile]) return true;
  const uint32_t *source = SimTownGroundArt_Metatile(
      writer->town, writer->ground->development_tier[writer->town - 1], tile);
  if (!source) return false;
  const int sx = ((tile & 15) + (variant & 1) * 16) * kSimTownCellPixels;
  const int sy = ((tile >> 4) + (variant >> 1) * 16) * kSimTownCellPixels;
  for (int py = 0; py < kSimTownCellPixels; py++)
    for (int px = 0; px < kSimTownCellPixels; px++) {
      bool opaque = false;
      if (!SimBackgroundMountainSilhouette_Lookup(tile, px, py, &opaque)) return false;
      writer->scene->atlas[(sy + py) * kAtlas + sx + px] = opaque
          ? source[py * kSimTownCellPixels + px] | 0xFF000000u : 0;
    }
  if (writer->town == 4 && (tile == 0x70 || tile == 0x71)) {
    uint8_t *mask = writer->scene->lava_mask[tile - 0x70];
    if (!SimTownGroundArt_ColorIndexMask(4, writer->ground->development_tier[3],
            tile, 0x21, mask)) return false;
    bool any = false;
    for (int p = 0; p < kSimTownCellPixels * kSimTownCellPixels; p++) {
      mask[p] &= (writer->scene->atlas[(sy + p / kSimTownCellPixels) * kAtlas +
          sx + p % kSimTownCellPixels] >> 24) != 0;
      any |= mask[p] != 0;
    }
    if (any) {
      writer->scene->lava_tiles |= (uint8_t)(1u << (tile - 0x70));
      writer->scene->lava_variant = (uint8_t)variant;
      writer->scene->lava_ready = false;
    }
  }
  writer->loaded[variant][tile] = true;
  return true;
}

bool SimWorldNavigationMountains_UpdateLava(
    SimWorldNavigationMountainScene *scene, uint16_t game_frame,
    SimWorldNavigationMountainAtlasUpdate *update) {
  if (!update) return false;
  memset(update, 0, sizeof(*update));
  if (!scene || !scene->atlas || !scene->lava_tiles) return false;
  /* $02:AF69 is gated to SIM town 4, writes CGRAM $21, and folds bit $20
   * of the frame byte before masking to five red bits. Like native CHR
   * animation, presentation observes the completed tick before $0088's
   * increment. Continue this 64-tick cadence, not a resident PPU palette. */
  const uint8_t tick = (uint8_t)(game_frame - 1);
  const uint8_t red = (uint8_t)((tick & 0x20 ? ~tick : tick) & 0x1F);
  if (scene->lava_ready && scene->lava_red == red) return false;
  const uint32_t color = UINT32_C(0xFF000000) |
      (uint32_t)ExpandColor5(red, 15) << 16;
  const int sx = scene->overhead_crater ? kSimWorldNavigationCraterAtlasX
      : (scene->lava_variant & 1) * 16 * kSimTownCellPixels;
  const int sy = scene->overhead_crater ? kSimWorldNavigationCraterAtlasY
      : (7 + (scene->lava_variant >> 1) * 16) * kSimTownCellPixels;
  for (int tile = 0; tile < 2; tile++) {
    if (!(scene->lava_tiles & (1u << tile))) continue;
    for (int p = 0; p < kSimTownCellPixels * kSimTownCellPixels; p++)
      if (scene->lava_mask[tile][p])
        scene->atlas[(sy + p / kSimTownCellPixels) * kAtlas + sx +
            tile * kSimTownCellPixels + p % kSimTownCellPixels] = color;
  }
  const int first = (scene->lava_tiles & 1) ? 0 : 1;
  const int last = (scene->lava_tiles & 2) ? 1 : 0;
  *update = (SimWorldNavigationMountainAtlasUpdate){
      (uint16_t)(sx + first * kSimTownCellPixels), (uint16_t)sy,
      (uint16_t)((last - first + 1) * kSimTownCellPixels), kSimTownCellPixels};
  scene->lava_red = red;
  scene->lava_ready = true;
  return true;
}

static bool ExteriorCellAllowed(int x, int y) {
  if (x < 0 || y < 0 || x >= kSimWorldMapTiles || y >= kSimWorldMapTiles) return false;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    if (SimWorldMap_OriginForTown(town, &ox, &oy) &&
        x >= ox && x < ox + kSimTownCells && y >= oy && y < oy + kSimTownCells)
      return false; /* Even mountain cells in a neighbouring town are owned. */
  }
  return SimWorldMap_MountainCoverage(x, y) >= .18f;
}

static bool RearCellAllowed(const FaceWriter *writer, int x, int y) {
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy) ||
        x < ox || x >= ox + 32 || y < oy || y >= oy + 32) continue;
    const SimWorldNavigationTownGround *ground = writer->ground;
    if (!(ground->enabled_town_mask & (1u << (town - 1)))) return false;
    const int cx = x - ox, cy = y - oy;
    return !(ground->object_rows[town - 1][cy] & (1u << cx)) &&
        SimBackgroundMountains_TileFlags(town, ground->terrain[town - 1][cy * 32 + cx]);
  }
  return true; /* Buildings live only in the six registered town windows. */
}

typedef struct RearVertex { float x, y, z, u, v, brightness; } RearVertex;

static int ClipMountainPlane(const RearVertex *in, int count, RearVertex *out,
                              float boundary, bool keep_above, bool x_axis) {
  int written = 0;
  for (int i = 0; i < count; i++) {
    const RearVertex *a = &in[i], *b = &in[(i + 1) % count];
    const float aa = x_axis ? a->x : a->y, bb = x_axis ? b->x : b->y;
    const bool inside_a = keep_above ? aa >= boundary : aa <= boundary;
    const bool inside_b = keep_above ? bb >= boundary : bb <= boundary;
    if (inside_a) out[written++] = *a;
    if (inside_a != inside_b) {
      const float t = (boundary - aa) / (bb - aa);
      out[written++] = (RearVertex){
        x_axis ? boundary : a->x + (b->x - a->x) * t,
        x_axis ? a->y + (b->y - a->y) * t : boundary,
        a->z + (b->z - a->z) * t,
        a->u + (b->u - a->u) * t, a->v + (b->v - a->v) * t,
        a->brightness + (b->brightness - a->brightness) * t,
      };
    }
  }
  return written;
}

static void EmitRearClipped(const SimBackgroundMountainMeshContext *mesh,
                            const float x[4], const float y[4], const float z[4],
                            const SimBackgroundMountainMeshUV uv[4],
                            const uint8_t brightness[4], const uint8_t alpha[4]) {
  FaceWriter *writer = mesh->user;
  float minimum = y[0], maximum = y[0];
  RearVertex source[4];
  for (int p = 0; p < 4; p++) {
    minimum = fminf(minimum, y[p]); maximum = fmaxf(maximum, y[p]);
    source[p] = (RearVertex){x[p], y[p], z[p], uv[p].x, uv[p].y, brightness[p]};
  }
  /* Skyline strips never cross an x cell boundary. Clip y against the
   * actual retained occupancy, including a different town across the edge.
   * Masking just the mesh's vertices would still span a building between them. */
  const int column = writer->origin_x + (int)floorf((x[0] + x[1]) / 32);
  const int first = (int)floorf(minimum / 16), limit = (int)ceilf(maximum / 16);
  bool clear = true;
  for (int row = first; row < limit; row++)
    if (!RearCellAllowed(writer, column, writer->origin_y + row)) { clear = false; break; }
  if (clear) {
    mesh->emit(mesh->user, x, y, z, uv, brightness, alpha);
    return;
  }
  for (int row = first; row < limit; row++) {
    if (!RearCellAllowed(writer, column, writer->origin_y + row)) continue;
    if (minimum >= row * 16 && maximum <= (row + 1) * 16) {
      mesh->emit(mesh->user, x, y, z, uv, brightness, alpha);
      continue;
    }
    RearVertex lower[8], clipped[8];
    int count = ClipMountainPlane(source, 4, lower, row * 16, true, false);
    count = ClipMountainPlane(lower, count, clipped, (row + 1) * 16, false, false);
    for (int triangle = 1; triangle + 1 < count; triangle++) {
      const int index[4] = {0, triangle, triangle + 1, count == 4 ? 3 : triangle + 1};
      float xx[4], yy[4], zz[4];
      SimBackgroundMountainMeshUV tex[4];
      uint8_t light[4];
      for (int p = 0; p < 4; p++) {
        const RearVertex *v = &clipped[index[p]];
        xx[p] = v->x; yy[p] = v->y; zz[p] = v->z;
        tex[p] = (SimBackgroundMountainMeshUV){v->u, v->v};
        light[p] = (uint8_t)(v->brightness + .5f);
      }
      mesh->emit(mesh->user, xx, yy, zz, tex, light, alpha);
      if (count == 4) break;
    }
  }
}

static void EmitExteriorClipped(
    void *user, const float x[4], const float y[4], const float z[4],
    const SimBackgroundMountainMeshUV uv[4], const uint8_t brightness[4], const uint8_t alpha[4]) {
  FaceWriter *writer = user;
  if (writer->failed) return;
  float lo_x = x[0], hi_x = x[0], lo_y = y[0], hi_y = y[0];
  RearVertex source[4];
  for (int p = 0; p < 4; p++) {
    lo_x = fminf(lo_x, x[p]); hi_x = fmaxf(hi_x, x[p]);
    lo_y = fminf(lo_y, y[p]); hi_y = fmaxf(hi_y, y[p]);
    source[p] = (RearVertex){x[p], y[p], z[p], uv[p].x, uv[p].y, brightness[p]};
  }
  const int first_x = (int)floorf(lo_x / 16), first_y = (int)floorf(lo_y / 16);
  const int limit_x = (int)fmaxf(first_x + 1, ceilf(hi_x / 16));
  const int limit_y = (int)fmaxf(first_y + 1, ceilf(hi_y / 16));
  bool clear = true, any = false;
  for (int cy = first_y; cy < limit_y; cy++)
    for (int cx = first_x; cx < limit_x; cx++) {
      const bool allowed = ExteriorCellAllowed(writer->origin_x + cx, writer->origin_y + cy);
      clear &= allowed; any |= allowed;
    }
  if (!any) return;
  if (!LoadTile(writer, writer->variant, writer->tile)) { writer->failed = true; return; }
  if (clear) { RetainFace(user, x, y, z, uv, brightness, alpha); return; }
  /* Clip the entire polygon, not only its corners. A fitted side wall can
   * lean slightly into another x cell; fronts/rears can span several y rows. */
  for (int cy = first_y; cy < limit_y; cy++)
    for (int cx = first_x; cx < limit_x; cx++) {
      if (!ExteriorCellAllowed(writer->origin_x + cx, writer->origin_y + cy)) continue;
      RearVertex a[12], b[12];
      int count = ClipMountainPlane(source, 4, a, cx * 16, true, true);
      count = ClipMountainPlane(a, count, b, (cx + 1) * 16, false, true);
      count = ClipMountainPlane(b, count, a, cy * 16, true, false);
      count = ClipMountainPlane(a, count, b, (cy + 1) * 16, false, false);
      for (int triangle = 1; triangle + 1 < count; triangle++) {
        const int index[4] = {0, triangle, triangle + 1, count == 4 ? 3 : triangle + 1};
        float xx[4], yy[4], zz[4];
        SimBackgroundMountainMeshUV tex[4];
        uint8_t light[4];
        for (int p = 0; p < 4; p++) {
          const RearVertex *v = &b[index[p]];
          xx[p] = v->x; yy[p] = v->y; zz[p] = v->z;
          tex[p] = (SimBackgroundMountainMeshUV){v->u, v->v};
          light[p] = (uint8_t)(v->brightness + .5f);
        }
        RetainFace(user, xx, yy, zz, tex, light, alpha);
        if (count == 4) break;
      }
    }
}

/* The native inclined front and fitted side walls leave the ridge open from
 * behind. Fold each source column back from its own audited crest, not the
 * rectangular stamp's top: one shared mirror line produces two separate
 * peaks with a hole between their shoulders. This uses the original art and
 * native front transform, adding no proxy silhouette or renderer dependency. */
enum { kCrestColumns = kSimBackgroundMountainObjectMaxColumns * kSimTownCellPixels };

static void ObjectCrest(const SimBackgroundMountainObject *object, float crest[kCrestColumns + 1]) {
  const int width = object->width_cells * kSimTownCellPixels;
  const int height = object->height_cells * kSimTownCellPixels;
  int first[kCrestColumns];
  for (int x = 0; x < width; x++) {
    first[x] = height;
    for (int y = 0; y < height; y++) {
      if (!ObjectCell(object, x / 16, y / 16)) continue;
      bool opaque = false;
      SimBackgroundMountainSilhouette_Lookup(
          object->source_tile[y / 16][x / 16], x % 16, y % 16, &opaque);
      if (opaque) { first[x] = y; break; }
    }
  }
  for (int x = 0; x <= width; x++)
    crest[x] = (first[x ? x - 1 : 0] + first[x < width ? x : width - 1]) * .5f;
}

static void VolcanoProfile(const SimBackgroundMountainObject *object,
    const SimBackgroundMountainMeshContext *mesh, float crest,
    float *front, float *back, float *height) {
  const float baseline = (object->cell_y + object->height_cells) * 16.0f;
  const float cut_rise = (object->height_cells - 1) * 16.0f;
  const float rise = fminf(cut_rise, object->height_cells * 16.0f - crest);
  const float shoulder = rise / cut_rise;
  *front = baseline - rise * mesh->relief->face_depth_scale;
  /* The 32-pixel-wide crown gets a 28-pixel-deep top. Shoulder depth
   * tapers to zero at the native silhouette, staying inside its original
   * rear ground contact. This adds no land behind the mountain footprint. */
  *back = *front - 28.0f * shoulder * shoulder;
  *height = rise * mesh->relief->face_height_scale * mesh->height_scale;
}

static void VolcanoRoof(const SimBackgroundMountainObject *object,
                        const SimBackgroundMountainMeshContext *mesh, unsigned variant) {
  FaceWriter *writer = mesh->user;
  float crest[kCrestColumns + 1];
  ObjectCrest(object, crest);
  const int width = object->width_cells * 16;
  float top_front, top_back, top_height;
  VolcanoProfile(object, mesh, 16, &top_front, &top_back, &top_height);
  const float cap_left = (object->cell_x + object->width_cells * .5f) * 16 - 16;
  const uint8_t brightness[4] = {255, 255, 255, 255};
  for (int column = 0; column < width; column++) {
    float x[4], y[4], z[4];
    SimBackgroundMountainMeshUV uv[4];
    const bool crown = column >= width / 2 - 16 && column < width / 2 + 16;
    const int cx = column / 16;
    int cy = (int)fmaxf(1, fminf(object->height_cells - 1, ceilf(crest[column] / 16)));
    while (cy < object->height_cells - 1 && !ObjectCell(object, cx, cy)) cy++;
    const uint8_t tile = object->source_tile[cy][cx];
    if (!crown && !LoadTile(writer, variant, tile)) { writer->failed = true; return; }
    for (int p = 0; p < 4; p++) {
      const int at = column + (p == 1 || p == 2);
      float front, back, height;
      VolcanoProfile(object, mesh, crest[at], &front, &back, &height);
      x[p] = object->cell_x * 16 + at;
      y[p] = p < 2 ? front : back;
      z[p] = height;
      const float u = fminf(1, fmaxf(0, (x[p] - cap_left) / 32));
      const float v = fminf(1, fmaxf(0, (y[p] - top_back) / (top_front - top_back)));
      uv[p] = (SimBackgroundMountainMeshUV){
        (kSimWorldNavigationCraterAtlasX + .5f + u * 15) / kAtlas,
        (kSimWorldNavigationCraterAtlasY + .5f + v * 15) / kAtlas,
      };
      if (!crown) {
        const int sx = ((tile & 15) + (variant & 1) * 16) * 16;
        const int sy = ((tile >> 4) + (variant >> 1) * 16) * 16;
        uv[p] = (SimBackgroundMountainMeshUV){
          (sx + fminf(15.5f, fmaxf(.5f, at - cx * 16))) / kAtlas,
          (sy + (p < 2 ? 8.5f : 14.5f)) / kAtlas,
        };
      }
    }
    EmitRearClipped(mesh, x, y, z, uv, brightness, brightness);
  }
}

static void BackObject(const SimBackgroundMountainObject *object,
                       const SimBackgroundMountainMeshContext *mesh, unsigned variant) {
  FaceWriter *writer = mesh->user;
  const int width = object->width_cells * kSimTownCellPixels;
  float crest[kCrestColumns + 1];
  ObjectCrest(object, crest);
  const float baseline = (object->cell_y + object->height_cells) * (float)kSimTownCellPixels;
  /* The front uses 62% of the original stamp depth. Its rear receives only
   * the remaining 38%, placing every rear ground contact on the original
   * silhouette rather than extending a mirrored peak over nearby roads or
   * water. This also keeps the inferred-relief replacement footprint valid. */
  const float rear_depth = (1 - mesh->relief->face_depth_scale) / mesh->relief->face_depth_scale;
  static const uint8_t brightness[4] = {235, 235, 190, 190};
  static const uint8_t alpha[4] = {255, 255, 255, 255};
  for (int begin = 0; begin < width;) {
    const int cx = begin / 16;
    /* Never cross an atlas tile boundary. Within it, retain the widest
     * linear skyline span within half a native pixel, avoiding per-pixel
     * geometry on the long straight shoulders of the authored silhouettes. */
    int end = (cx + 1) * 16;
    if (writer->capped_volcano) end = begin + 1; /* Roof/rear share exact edge vertices. */
    for (; end > begin + 1; end--) {
      bool straight = true;
      for (int x = begin + 1; x < end; x++)
        if (fabsf(crest[x] - (crest[begin] + (crest[end] - crest[begin]) *
            (x - begin) / (end - begin))) > .5f) { straight = false; break; }
      if (straight) break;
    }
    const int destination_x = object->cell_x + cx;
    if ((destination_x >= 0 && destination_x < kSimTownCells) != writer->exterior)
      for (int cy = 0; cy < object->height_cells; cy++) {
        if (writer->capped_volcano && cy == 0) continue;
        if (!ObjectCell(object, cx, cy)) continue;
        const float bottom = (cy + 1) * 16;
        if (crest[begin] >= bottom && crest[end] >= bottom) continue;
        const uint8_t tile = object->source_tile[cy][cx];
        const int sx = (tile & 15) + (variant & 1) * 16;
        const int sy = (tile >> 4) + (variant >> 1) * 16;
        float x[4], y[4], z[4];
        SimBackgroundMountainMeshUV uv[4];
        for (int p = 0; p < 4; p++) {
          const int column = p == 0 || p == 3 ? begin : end;
          const float source_y = p < 2 ? fminf(bottom, fmaxf(cy * 16, crest[column])) : bottom;
          const float row = object->cell_y * 16 + source_y;
          const float peak = object->cell_y * 16 + crest[column];
          SimBackgroundMountainMesh_PlanePoint(object->cell_x * 16 + column,
              row, baseline, mesh->relief, 0, 0, &x[p], &y[p], &z[p]);
          const float crest_y = baseline - (baseline - peak) * mesh->relief->face_depth_scale;
          y[p] = crest_y - (y[p] - crest_y) * rear_depth;
          z[p] *= mesh->height_scale;
          if (writer->capped_volcano) {
            float front, back, height;
            VolcanoProfile(object, mesh, crest[column], &front, &back, &height);
            const float contact = object->cell_y * 16 + crest[column];
            y[p] = height > 0 ? contact + (back - contact) * z[p] / height : contact;
          }
          uv[p] = (SimBackgroundMountainMeshUV){
            (sx * 16 + column - cx * 16) / mesh->atlas_pixels,
            (sy * 16 + source_y - cy * 16) / mesh->atlas_pixels,
          };
        }
        if (writer->exterior) {
          writer->tile = tile;
          mesh->emit(mesh->user, x, y, z, uv, brightness, alpha);
        } else EmitRearClipped(mesh, x, y, z, uv, brightness, alpha);
      }
    begin = end;
  }
}

bool SimWorldNavigationMountains_Build(
    const SimWorldNavigationTownGround *ground,
    SimWorldNavigationMountainScene *out) {
  if (!out) return false;
  SimWorldNavigationMountains_Destroy(out);
  if (!ground || !SimTownGroundArt_Available()) return false;
  out->atlas = calloc((size_t)kAtlas * kAtlas, sizeof(*out->atlas));
  if (!out->atlas) return false;
  bool loaded[4][256] = {{false}};
  SimBackgroundMountainField field;
  SimBackgroundMountainCaps caps;
  SimBackgroundMountainObjectList objects;
  SimBackgroundMountainRelief relief;
  SimBackgroundMountainRelief_Resolve(kSimBackgroundVoxelDetail_Low, &relief);
  FaceWriter writer = {.scene = out, .ground = ground, .loaded = loaded};
  SimBackgroundMountainMeshContext mesh = {
    .relief = &relief, .stack_direction = {0, -1}, .height_scale = 1,
    .atlas_pixels = kAtlas, .emit = RetainFace, .user = &writer,
  };
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(ground->enabled_town_mask & (1u << (town - 1)))) continue;
    SimBackgroundMountains_ClassifyCells(town, ground->terrain[town - 1], &field);
    SimBackgroundMountains_BuildNorthCaps(&field, &caps);
    if (!field.cell_count || !SimBackgroundMountainObjects_Build(
            &field, &caps, &objects)) continue;
    writer.town = town;
    if (!SimWorldMap_OriginForTown(town, &writer.origin_x, &writer.origin_y))
      continue;
    const unsigned variant = (town == 6 ? 2u : 0u) |
        (ground->development_tier[town - 1] >= 2 ? 1u : 0u);
    out->town_mask |= (uint8_t)(1u << (town - 1));
    for (uint8_t at = 0; at < objects.count; at++) {
      const SimBackgroundMountainObject *object = &objects.objects[at];
      mesh.height_scale = object->flags & kSimBackgroundMountainObject_Volcano
          ? 1.12f : 1.0f;
      writer.capped_volcano = (object->flags & kSimBackgroundMountainObject_Volcano) &&
          LoadOverheadCrater(out, ground->development_tier[town - 1]);
      const float baseline = (object->cell_y + object->height_cells) *
          (float)kSimTownCellPixels;
      const float rise = object->height_cells * (float)kSimTownCellPixels;
      for (int y = 0; y < object->height_cells; y++)
        for (int x = 0; x < object->width_cells; x++) {
          if (!ObjectCell(object, x, y)) continue;
          const int cx = object->cell_x + x, cy = object->cell_y + y;
          /* Same horizontal half-peak policy as the town renderer. Full
           * north/south continuations retain their real ground contacts. */
          if (cx < 0 || cx >= kSimTownCells) continue;
          const uint8_t tile = object->source_tile[y][x];
          const int sx = (tile & 15) + (variant & 1) * 16;
          const int sy = (tile >> 4) + (variant >> 1) * 16;
          const int wx = writer.origin_x + cx, wy = writer.origin_y + cy;
          if (wx >= 0 && wy >= 0 && wx < kSimWorldMapTiles && wy < kSimWorldMapTiles)
            out->replacement[wy * kSimWorldMapTiles + wx] = town;
          if (writer.capped_volcano && y == 0) continue;
          if (!LoadTile(&writer, variant, tile)) { writer.failed = true; break; }
          SimBackgroundMountainMesh_StackTile(
              &mesh, baseline, baseline, rise, rise, cx, cy, sx, sy, 0);
          for (int side = 0; side < 2; side++)
            if (!ObjectCell(object, x + (side ? 1 : -1), y))
              SimBackgroundMountainMesh_SkirtTile(
                  &mesh, baseline, rise, cx, cy, sx, sy, tile, side != 0);
        }
      writer.rear_slope = true;
      BackObject(object, &mesh, variant);
      if (writer.capped_volcano) {
        writer.summit_roof = true;
        VolcanoRoof(object, &mesh, variant);
        writer.summit_roof = false;
      }
      writer.rear_slope = false;
    }
  }
  if (writer.failed) { SimWorldNavigationMountains_Destroy(out); return false; }
  return true;
}

bool SimWorldNavigationMountains_ContinueEdges(
    const SimWorldNavigationTownGround *ground, SimWorldNavigationMountainScene *scene) {
  if (!ground || !scene || !scene->atlas || !scene->town_mask ||
      !SimTownGroundArt_Available() || !SimWorldMap_DevelopedAvailable()) return false;
  if (scene->exterior_face_count) return true; /* Caller rebuilds on changed geography. */
  uint32_t *original_atlas = scene->atlas;
  uint32_t *atlas = malloc((size_t)kAtlas * kAtlas * sizeof(*atlas));
  if (!atlas) return false;
  memcpy(atlas, original_atlas, (size_t)kAtlas * kAtlas * sizeof(*atlas));
  scene->atlas = atlas;
  const size_t original_count = scene->face_count;
  const float original_rise = scene->maximum_rise;
  float original_town_rise[kSimTownCount];
  memcpy(original_town_rise, scene->town_maximum_rise, sizeof(original_town_rise));
  bool loaded[4][256] = {{false}};
  FaceWriter writer = {.scene = scene, .ground = ground, .exterior = true, .loaded = loaded};
  SimBackgroundMountainRelief relief;
  SimBackgroundMountainRelief_Resolve(kSimBackgroundVoxelDetail_Low, &relief);
  SimBackgroundMountainMeshContext mesh = {
    .relief = &relief, .stack_direction = {0, -1}, .height_scale = 1,
    .atlas_pixels = kAtlas, .emit = EmitExteriorClipped, .user = &writer,
  };
  for (uint8_t town = 1; town <= kSimTownCount && !writer.failed; town++) {
    if (!(scene->town_mask & ground->enabled_town_mask & (1u << (town - 1)))) continue;
    SimBackgroundMountainField field;
    SimBackgroundMountainCaps caps;
    SimBackgroundMountainObjectList objects;
    SimBackgroundMountains_ClassifyCells(town, ground->terrain[town - 1], &field);
    SimBackgroundMountains_BuildNorthCaps(&field, &caps);
    if (!SimBackgroundMountainObjects_Build(&field, &caps, &objects)) { writer.failed = true; break; }
    writer.town = town;
    SimWorldMap_OriginForTown(town, &writer.origin_x, &writer.origin_y);
    writer.variant = (town == 6 ? 2u : 0u) | (ground->development_tier[town - 1] >= 2 ? 1u : 0u);
    for (uint8_t at = 0; at < objects.count; at++) {
      const SimBackgroundMountainObject *object = &objects.objects[at];
      if (object->cell_x >= 0 && object->cell_x + object->width_cells <= kSimTownCells) continue;
      mesh.height_scale = object->flags & kSimBackgroundMountainObject_Volcano ? 1.12f : 1;
      const float baseline = (object->cell_y + object->height_cells) * (float)kSimTownCellPixels;
      const float rise = object->height_cells * (float)kSimTownCellPixels;
      for (int y = 0; y < object->height_cells; y++)
        for (int x = 0; x < object->width_cells; x++) {
          const int cx = object->cell_x + x, cy = object->cell_y + y;
          if ((cx >= 0 && cx < kSimTownCells) || !ObjectCell(object, x, y)) continue;
          writer.tile = object->source_tile[y][x];
          const int sx = (writer.tile & 15) + (writer.variant & 1) * 16;
          const int sy = (writer.tile >> 4) + (writer.variant >> 1) * 16;
          SimBackgroundMountainMesh_StackTile(&mesh, baseline, baseline, rise, rise, cx, cy, sx, sy, 0);
          for (int side = 0; side < 2; side++)
            if (!ObjectCell(object, x + (side ? 1 : -1), y))
              SimBackgroundMountainMesh_SkirtTile(&mesh, baseline, rise, cx, cy, sx, sy, writer.tile, side != 0);
        }
      writer.rear_slope = true;
      BackObject(object, &mesh, writer.variant);
      writer.rear_slope = false;
    }
  }
  if (writer.failed) {
    scene->face_count = original_count;
    scene->maximum_rise = original_rise;
    memcpy(scene->town_maximum_rise, original_town_rise, sizeof(original_town_rise));
    free(scene->atlas);
    scene->atlas = original_atlas;
    return false;
  }
  free(original_atlas);
  scene->exterior_face_count = scene->face_count - original_count;
  /* Commit relief ownership only after every optional face has succeeded.
   * Existing native faces and their masks stay byte-identical on failure. */
  for (size_t i = original_count; i < scene->face_count; i++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[i];
    float lo_x = face->x[0], hi_x = lo_x, lo_y = face->y[0], hi_y = lo_y;
    for (int p = 1; p < 4; p++) {
      lo_x = fminf(lo_x, face->x[p]); hi_x = fmaxf(hi_x, face->x[p]);
      lo_y = fminf(lo_y, face->y[p]); hi_y = fmaxf(hi_y, face->y[p]);
    }
    for (int y = (int)floorf(lo_y); y < (int)ceilf(hi_y); y++)
      for (int x = (int)floorf(lo_x); x < (int)ceilf(hi_x); x++)
        if (ExteriorCellAllowed(x, y)) {
          scene->replacement[y * kSimWorldMapTiles + x] = face->town;
          scene->exterior_cells[y * kSimWorldMapTiles + x] = face->town;
        }
  }
  return true;
}

bool SimWorldNavigationMountains_ClearGround(
    uint32_t *pixels, int pitch, const SimWorldNavigationTownGround *ground,
    uint8_t town_mask, const uint8_t *cells) {
  if (!pixels || pitch < kSimWorldNavigationArtPixels || !ground) return false;
  const uint32_t *plain[kSimTownCount] = {0};
  for (uint8_t town = 1; town <= kSimTownCount; town++)
    if (town_mask & (1u << (town - 1))) {
      plain[town - 1] = SimTownGroundArt_Metatile(
          town, ground->development_tier[town - 1], town == 6 ? 0xFF : 0x08);
      if (!plain[town - 1]) return false;
    }
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!plain[town - 1]) continue;
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) continue;
    for (int y = 0; y < kSimTownCells; y++)
      for (int x = 0; x < kSimTownCells; x++) {
        if (cells && !cells[(oy + y) * kSimWorldMapTiles + ox + x]) continue;
        if (!SimBackgroundMountains_TileFlags(
                town, ground->terrain[town - 1][y * kSimTownCells + x])) continue;
        for (int py = 0; py < kSimTownCellPixels; py++)
          for (int px = 0; px < kSimTownCellPixels; px++) {
            const uint32_t color = plain[town - 1][py * kSimTownCellPixels + px];
            if (color >> 24) pixels[((oy + y) * kSimTownCellPixels + py) * pitch +
                (ox + x) * kSimTownCellPixels + px] = color;
          }
      }
  }
  return true;
}
