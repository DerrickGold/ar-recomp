#include "sim/church/church_exterior.h"

#include <math.h>
#include <stdlib.h>
#include "sim/voxels/sim_background_voxel_model_cache.h"
#include "sim/voxels/sim_background_voxel_project.h"
#include "sim/voxels/sim_background_voxel_proportions.h"

enum { kAtlasSize = 512 };
typedef struct ShadowPoint {
  float x, y;
} ShadowPoint;

static float Edge(ShadowPoint a, ShadowPoint b, ShadowPoint p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

/* Union silhouettes before filtering, so overlapping tree faces/casters do
 * not compound the darkness. The mask belongs only to this captured view. */
static void RasterTriangle(uint8_t *mask, ShadowPoint a, ShadowPoint b, ShadowPoint c) {
  const float area = Edge(a, b, c);
  if (fabsf(area) < .00001f) return;
  const int x0 = (int)fmaxf(0, floorf(fminf(a.x, fminf(b.x, c.x))));
  const int y0 = (int)fmaxf(0, floorf(fminf(a.y, fminf(b.y, c.y))));
  const int x1 = (int)fminf(kAtlasSize - 1, ceilf(fmaxf(a.x, fmaxf(b.x, c.x))));
  const int y1 = (int)fminf(kAtlasSize - 1, ceilf(fmaxf(a.y, fmaxf(b.y, c.y))));
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      const ShadowPoint p = {x + .5f, y + .5f};
      if (Edge(b, c, p) / area >= -.00001f && Edge(c, a, p) / area >= -.00001f &&
          Edge(a, b, p) / area >= -.00001f)
        mask[y * kAtlasSize + x] = 1;
    }
  }
}

static float ExteriorHeight(const ChurchSceneOptions *options, uint8_t town, float x, float y) {
#if AR_SIM3D_TERRAIN_ELEVATION
  return ChurchLandscape_Terrain(options->landscape, town, x, y) * options->landscape_height_pct /
         100;
#else
  (void)options;
  (void)town;
  (void)x;
  (void)y;
  return 0;
#endif
}

static ShadowPoint GroundPoint(const ChurchSceneOptions *options, uint8_t town, ShadowPoint p,
                               float z, ShadowPoint cast) {
  float drop = fmaxf(0, z - ExteriorHeight(options, town, p.x, p.y));
  /* Refine against the same terrain as the visible ground instead of casting
   * onto a flat plane below hills. A bounded slope avoids runaway dawn rays. */
  for (int i = 0; i < 6; i++) {
    const float height = ExteriorHeight(options, town, p.x + cast.x * drop, p.y + cast.y * drop);
    const float next = fmaxf(0, z - height);
    if (fabsf(next - drop) < .05f) break;
    drop = (drop + next) * .5f;
  }
  return (ShadowPoint){p.x + cast.x * drop, p.y + cast.y * drop};
}

bool ChurchExterior_BakeGround(const SimBackgroundVoxelScene *town,
                               const ChurchSceneOptions *options, const uint32_t *source,
                               uint32_t *output) {
  if (!town || !options || !source || !output || source == output ||
      town->object_count > kSimBackgroundMaxObjects)
    return false;
  uint8_t *mask = calloc(kAtlasSize * kAtlasSize, sizeof(*mask));
  if (!mask) return false;
  const float radians = 3.141592654f / 180;
  const float azimuth = options->light_azimuth_deg * radians;
  const float elevation = options->light_elevation_deg * radians;
  const float sine = sinf(elevation);
  const float slope = fminf(4, fmaxf(0, sine > .05f ? cosf(elevation) / sine : 4));
  /* Native ground shadows use +X toward the cast and inverted atlas Y. */
  const ShadowPoint cast = {cosf(azimuth) * slope, -sinf(azimuth) * slope};
  for (unsigned i = 0; i < town->object_count; i++) {
    const SimBackgroundVoxelObject *object = &town->objects[i];
    if (!SimBackgroundVoxelModel_CastsShadow(object)) continue;
    const SimBackgroundVoxelModelView *model =
        SimBackgroundVoxelModelCache_Get(object, kSimBackgroundVoxelDetail_Ultra,
                                         (SimBackgroundVoxelStyle)options->style, NULL, NULL);
    if (!model || model->overflow) {
      free(mask);
      return false;
    }
    const float cx = object->footprint_cells_w * 8.0f, cy = object->footprint_cells_d * 8.0f;
    const float x = object->cell_x * 16 + cx;
    const float y = (object->cell_y + object->source_cells_h - object->footprint_cells_d) * 16 + cy;
    const float base = ExteriorHeight(options, town->town, x, y);
    const SimBackgroundVoxelProportions *proportions =
        SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)object->kind);
    for (unsigned f = 0; f < model->face_count; f++) {
      ShadowPoint p[4];
      for (int j = 0; j < 4; j++) {
        const SimBackgroundVoxelModelPoint point = model->faces[f].points[j];
        const ShadowPoint position = {x + (point.x - cx) * proportions->footprint_scale,
                                      y + (point.y - cy) * proportions->footprint_scale};
        p[j] = GroundPoint(options, town->town, position,
                           base + point.z * proportions->height_scale, cast);
      }
      RasterTriangle(mask, p[0], p[1], p[2]);
      RasterTriangle(mask, p[0], p[2], p[3]);
    }
  }
  /* A small binomial filter gives contact silhouettes soft edges without
   * blurring or recoloring the native ground/road pixels underneath. */
  const int weights[5] = {1, 4, 6, 4, 1};
  for (int y = 0; y < kAtlasSize; y++) {
    for (int x = 0; x < kAtlasSize; x++) {
      unsigned coverage = 0;
      for (int dy = -2; dy <= 2; dy++) {
        const int sy = y + dy;
        if (sy < 0 || sy >= kAtlasSize) continue;
        for (int dx = -2; dx <= 2; dx++) {
          const int sx = x + dx;
          if (sx >= 0 && sx < kAtlasSize)
            coverage += mask[sy * kAtlasSize + sx] * weights[dy + 2] * weights[dx + 2];
        }
      }
      const uint32_t pixel = source[y * kAtlasSize + x];
      const float factor = 1 - .42f * coverage / 256;
      const uint32_t red = (uint32_t)(((pixel >> 16) & 255) * factor + .5f);
      const uint32_t green = (uint32_t)(((pixel >> 8) & 255) * factor + .5f);
      const uint32_t blue = (uint32_t)((pixel & 255) * factor + .5f);
      output[y * kAtlasSize + x] = (pixel & 0xff000000u) | (red << 16) | (green << 8) | blue;
    }
  }
  free(mask);
  return true;
}
