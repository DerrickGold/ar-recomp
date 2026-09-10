#include "sim_world_navigation_mountain_transition.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sim_world_navigation_art.h"
#include "sim_world_navigation_globe.h"
#include "sim_world_navigation_terrain.h"
#include "sim_background_mountains.h"

enum { kCells = kSimWorldMapTiles, kAxis = kCells + 1,
       kVertices = kAxis * kAxis, kRockShades = 6 };
static const float kBlendTiles = 12.0f;
static const float kJoinTiles = 4.0f;

static float Smooth(float x) {
  x = fminf(1, fmaxf(0, x));
  return x * x * (3 - 2 * x);
}

static unsigned Luminance(uint32_t c) {
  return ((c >> 16) & 255u) * 3 + ((c >> 8) & 255u) * 6 + (c & 255u);
}

static int CompareColor(const void *a, const void *b) {
  const uint32_t ca = *(const uint32_t *)a, cb = *(const uint32_t *)b;
  const unsigned la = Luminance(ca), lb = Luminance(cb);
  return la != lb ? (la < lb ? -1 : 1) : ca < cb ? -1 : ca > cb ? 1 : 0;
}

static bool NativeRamp(const SimWorldNavigationMountainScene *scene,
                       unsigned variant, uint32_t ramp[kRockShades]) {
  uint32_t colours[128];
  unsigned count = 0;
  const int ox = (variant & 1) * 256, oy = (variant >> 1) * 256;
  /* Only accepted, silhouette-masked source texels occupy this bank: no
   * grass, water or colour-inferred material classification enters the ramp. */
  for (int y = 0; y < 256; y++)
    for (int x = 0; x < 256; x++) {
      if (scene->overhead_crater &&
          ox + x >= kSimWorldNavigationCraterAtlasX &&
          oy + y >= kSimWorldNavigationCraterAtlasY) continue;
      const uint32_t c = scene->atlas[(oy + y) * kSimWorldNavigationMountainAtlasPixels + ox + x];
      if (!(c >> 24)) continue;
      unsigned i = 0;
      while (i < count && colours[i] != c) i++;
      if (i == count) {
        if (count == 128) return false;
        colours[count++] = c;
      }
    }
  if (!count) return false;
  qsort(colours, count, sizeof(*colours), CompareColor);
  for (unsigned i = 0; i < kRockShades; i++)
    ramp[i] = colours[i * (count - 1) / (kRockShades - 1)];
  return true;
}

static uint8_t TownAt(int x, int y) {
  /* Even disabled/unrecognized town stamps retain their fallback verbatim. */
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    if (SimWorldMap_OriginForTown(town, &ox, &oy) &&
        x >= ox && x < ox + kSimTownCells && y >= oy && y < oy + kSimTownCells)
      return town;
  }
  return 0;
}

static float OutsideFallbackWeight(int x, int y, uint8_t accepted) {
  float weight = 1;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (accepted & (1u << (town - 1))) continue;
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) continue;
    const float dx = fmaxf(0, fmaxf((float)(ox - x), (float)(x - ox - kSimTownCells)));
    const float dy = fmaxf(0, fmaxf((float)(oy - y), (float)(y - oy - kSimTownCells)));
    /* Keep fallback towns exact, but approach that constraint continuously
     * over four cells instead of introducing a new cliff at their border. */
    weight = fminf(weight, Smooth(hypotf(dx, dy) / 4));
  }
  return weight;
}

static void DistanceFromTown(const uint8_t *sources, uint8_t town, float *distance) {
  for (int i = 0; i < kCells * kCells; i++) distance[i] = sources[i] == town ? 0 : kBlendTiles;
  for (int pass = 0; pass < 2; pass++) {
    const int step = pass ? -1 : 1;
    for (int y = pass ? kCells - 1 : 0; y >= 0 && y < kCells; y += step)
      for (int x = pass ? kCells - 1 : 0; x >= 0 && x < kCells; x += step) {
        const int offsets[4][2] = {{-step, 0}, {0, -step}, {-step, -step}, {step, -step}};
        for (int n = 0; n < 4; n++) {
          const int nx = x + offsets[n][0], ny = y + offsets[n][1];
          if (nx >= 0 && ny >= 0 && nx < kCells && ny < kCells)
            distance[y * kCells + x] = fminf(distance[y * kCells + x],
                distance[ny * kCells + nx] + (n < 2 ? 1 : 1.41421356f));
        }
      }
  }
}

static float Interpolate(const float *field, int x, int y, float u, float v) {
  const int i = y * kAxis + x;
  return (field[i] * (1 - u) + field[i + 1] * u) * (1 - v) +
      (field[i + kAxis] * (1 - u) + field[i + kAxis + 1] * u) * v;
}

static bool JoinVertexAllowed(int x, int y, const SimWorldNavigationTownGround *ground,
                              const SimWorldNavigationMountainScene *scene,
                              bool allow_continued) {
  bool outside = false;
  for (int dy = -1; dy <= 0; dy++)
    for (int dx = -1; dx <= 0; dx++) {
      const int cx = x + dx, cy = y + dy;
      if (cx < 0 || cy < 0 || cx >= kCells || cy >= kCells) return false;
      if (!allow_continued && scene->exterior_cells[cy * kCells + cx]) return false;
      const uint8_t town = TownAt(cx, cy);
      if (town) {
        if (!(scene->town_mask & ground->enabled_town_mask & (1u << (town - 1)))) return false;
        int ox, oy;
        SimWorldMap_OriginForTown(town, &ox, &oy);
        const int tx = cx - ox, ty = cy - oy;
        if ((ground->object_rows[town - 1][ty] & (1u << tx)) ||
            !SimBackgroundMountains_TileFlags(town, ground->terrain[town - 1][ty * 32 + tx]))
          return false;
      } else {
        outside = true;
        if (SimWorldMap_MountainCoverage(cx, cy) < .18f) return false;
      }
    }
  return outside; /* Never change an interior town floor or span a non-rock cell. */
}

static bool EdgeOpaque(const SimWorldNavigationMountainScene *scene,
                        const SimWorldNavigationMountainFace *face, int a, int b, float t) {
  float low[2] = {face->uv[0].x, face->uv[0].y}, high[2] = {low[0], low[1]};
  for (int p = 1; p < 4; p++) {
    low[0] = fminf(low[0], face->uv[p].x); high[0] = fmaxf(high[0], face->uv[p].x);
    low[1] = fminf(low[1], face->uv[p].y); high[1] = fmaxf(high[1], face->uv[p].y);
  }
  const int atlas = kSimWorldNavigationMountainAtlasPixels;
  float uv[2] = {face->uv[a].x + (face->uv[b].x - face->uv[a].x) * t,
                 face->uv[a].y + (face->uv[b].y - face->uv[a].y) * t};
  int texel[2];
  for (int axis = 0; axis < 2; axis++) {
    /* Exact UV edges otherwise sample the next packed metatile. */
    const float lo = low[axis] * atlas + .5f, hi = high[axis] * atlas - .5f;
    const float value = lo > hi ? (lo + hi) * .5f : fminf(hi, fmaxf(lo, uv[axis] * atlas));
    texel[axis] = (int)fminf(atlas - 1, fmaxf(0, value));
  }
  return (scene->atlas[texel[1] * atlas + texel[0]] >> 24) != 0;
}

typedef struct JoinWork {
  bool allowed[kVertices];
  float distance[kVertices], temporary[kVertices];
} JoinWork;

static void SeedGeometryJoins(const SimWorldNavigationMountainScene *scene,
                              float radius_tiles, JoinWork *work,
                              SimWorldNavigationMountainTransition *out) {
  for (size_t at = 0; at < scene->face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[at];
    int ox, oy;
    if (!SimWorldMap_OriginForTown(face->town, &ox, &oy)) continue;
    for (int edge = 0; edge < 4; edge++) {
      const int a = edge, b = (edge + 1) & 3;
      for (int axis = 0; axis < 2; axis++) {
        const float *fixed = axis ? face->y : face->x;
        const float *along = axis ? face->x : face->y;
        const int origin = axis ? oy : ox;
        if (fabsf(fixed[a] - fixed[b]) > .00001f ||
            (fabsf(fixed[a] - origin) > .00001f && fabsf(fixed[a] - origin - 32) > .00001f)) continue;
        const int boundary = (int)roundf(fixed[a]);
        for (int step = (int)ceilf(fminf(along[a], along[b]));
             step <= (int)floorf(fmaxf(along[a], along[b])); step++) {
          const int x = axis ? step : boundary, y = axis ? boundary : step;
          if (x < 0 || y < 0 || x > kCells || y > kCells || !work->allowed[y * kAxis + x]) continue;
          const float span = along[b] - along[a];
          const float t = fabsf(span) > .00001f ? (step - along[a]) / span : face->z[a] >= face->z[b] ? 0 : 1;
          if (!EdgeOpaque(scene, face, a, b, t)) continue;
          float normal[3], metric;
          SimWorldNavigationGlobe_SampleAtRadius(radius_tiles, x, y, normal, &metric);
          const int i = y * kAxis + x;
          if (work->distance[i] != 0) out->join_anchor_count++;
          work->distance[i] = 0;
          out->join_rise[i] = fmaxf(out->join_rise[i], (face->z[a] + (face->z[b] - face->z[a]) * t) * metric);
        }
      }
    }
  }
}

static void SpreadGeometryConstraints(JoinWork *work, float *rise, float *weight) {
  /* Spread constraints through connected rock, never across a town/road/
   * water barrier. Anchor samples stay fixed; four relaxation passes smooth
   * nearest-source boundaries without flattening the authored ridge edge. */
  for (int pass = 0; pass < 2; pass++) {
    const int step = pass ? -1 : 1;
    for (int y = pass ? kCells : 0; y >= 0 && y <= kCells; y += step)
      for (int x = pass ? kCells : 0; x >= 0 && x <= kCells; x += step) {
        const int i = y * kAxis + x;
        if (!work->allowed[i] || work->distance[i] == 0) continue;
        const int offset[2][2] = {{-step, 0}, {0, -step}};
        for (int p = 0; p < 2; p++) {
          const int nx = x + offset[p][0], ny = y + offset[p][1];
          if (nx < 0 || ny < 0 || nx > kCells || ny > kCells) continue;
          const int n = ny * kAxis + nx;
          if (work->allowed[n] && work->distance[n] + 1 < work->distance[i]) {
            work->distance[i] = work->distance[n] + 1;
            rise[i] = rise[n];
          }
        }
      }
  }
  for (int pass = 0; pass < 4; pass++) {
    memcpy(work->temporary, rise, sizeof(work->temporary));
    for (int y = 1; y < kCells; y++)
      for (int x = 1; x < kCells; x++) {
        const int i = y * kAxis + x;
        if (!work->allowed[i] || work->distance[i] == 0 || work->distance[i] >= kJoinTiles) continue;
        float sum = rise[i] * 2; int count = 2;
        const int offset[4] = {-1, 1, -kAxis, kAxis};
        for (int p = 0; p < 4; p++) {
          const int n = i + offset[p];
          if (work->allowed[n] && work->distance[n] < kJoinTiles) { sum += rise[n]; count++; }
        }
        work->temporary[i] = sum / count;
      }
    memcpy(rise, work->temporary, sizeof(work->temporary));
  }
  for (int i = 0; i < kVertices; i++)
    weight[i] = work->allowed[i] ? Smooth(1 - work->distance[i] / kJoinTiles) : 0;
}

static bool ContinuationBoundary(int x, int y, const SimWorldNavigationTownGround *ground,
                                  const SimWorldNavigationMountainScene *scene) {
  if (!JoinVertexAllowed(x, y, ground, scene, true)) return false;
  bool continued = false, inferred = false;
  for (int dy = -1; dy <= 0; dy++)
    for (int dx = -1; dx <= 0; dx++) {
      const int cx = x + dx, cy = y + dy;
      continued |= scene->exterior_cells[cy * kCells + cx] != 0;
      inferred |= !scene->replacement[cy * kCells + cx] && !TownAt(cx, cy);
    }
  return continued && inferred;
}

static void SeedContinuationLimits(const SimWorldNavigationMountainScene *scene,
                                    const SimWorldNavigationTownGround *ground,
                                    float radius_tiles, JoinWork *work,
                                    SimWorldNavigationMountainTransition *out) {
  memset(work->temporary, 0, sizeof(work->temporary));
  /* Native fronts and rear contacts are oblique and usually fall between
   * grid vertices. Sample their closest opaque edge within the boundary
   * vertex's four audited cells; requiring an exact integer-aligned edge
   * finds no usable anchors in the populated map. The containing cells,
   * not just the segment endpoints, must all be protected-clear rock. */
  for (size_t at = 0; at < scene->face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[at];
    if (!face->exterior || face->town < 1 || face->town > kSimTownCount ||
        !(scene->town_mask & ground->enabled_town_mask & (1u << (face->town - 1)))) continue;
    for (int edge = 0; edge < 4; edge++) {
      const int a = edge, b = (edge + 1) & 3;
      const float dx = face->x[b] - face->x[a], dy = face->y[b] - face->y[a];
      const float length_squared = dx * dx + dy * dy;
      for (int y = (int)floorf(fminf(face->y[a], face->y[b]));
           y <= (int)ceilf(fmaxf(face->y[a], face->y[b])); y++)
        for (int x = (int)floorf(fminf(face->x[a], face->x[b]));
             x <= (int)ceilf(fmaxf(face->x[a], face->x[b])); x++) {
          if (x <= 0 || y <= 0 || x >= kCells || y >= kCells ||
              !ContinuationBoundary(x, y, ground, scene)) continue;
          const float t = length_squared > .00000001f
              ? fminf(1, fmaxf(0, ((x - face->x[a]) * dx + (y - face->y[a]) * dy) / length_squared))
              : face->z[a] >= face->z[b] ? 0 : 1;
          const float sample_x = face->x[a] + dx * t, sample_y = face->y[a] + dy * t;
          if (fabsf(sample_x - x) > 1 || fabsf(sample_y - y) > 1) continue;
          if (!EdgeOpaque(scene, face, a, b, t)) continue;
          float normal[3], metric;
          SimWorldNavigationGlobe_SampleAtRadius(radius_tiles, sample_x, sample_y, normal, &metric);
          const int i = y * kAxis + x;
          work->distance[i] = 0; /* Read-only boundary source, never an output vertex. */
          work->temporary[i] = fmaxf(work->temporary[i],
              (face->z[a] + (face->z[b] - face->z[a]) * t) * metric);
        }
    }
  }
  /* The first entirely unowned rock vertex receives a limit. Do not change
   * any corner of a continued cell or bridge a protected cell to reach it.
   * Lower-only application avoids filling beneath raised native geometry. */
  const int offset[4] = {-1, 1, -kAxis, kAxis};
  for (int y = 1; y < kCells; y++)
    for (int x = 1; x < kCells; x++) {
      const int i = y * kAxis + x;
      if (work->allowed[i] || work->distance[i] != 0) continue;
      for (int p = 0; p < 4; p++) {
        const int n = i + offset[p];
        if (!work->allowed[n]) continue;
        if (work->distance[n] != 0) out->continuation_anchor_count++;
        work->distance[n] = 0;
        out->continuation_rise[n] = fmaxf(out->continuation_rise[n], work->temporary[i]);
      }
    }
}

static bool BuildGeometryJoins(const SimWorldNavigationMountainScene *scene,
                               const SimWorldNavigationTownGround *ground,
                               float radius_tiles,
                               SimWorldNavigationMountainTransition *out) {
  JoinWork *work = malloc(sizeof(*work));
  if (!work) return false;
  for (int y = 0; y <= kCells; y++)
    for (int x = 0; x <= kCells; x++) {
      work->allowed[y * kAxis + x] = JoinVertexAllowed(x, y, ground, scene, false);
      work->distance[y * kAxis + x] = kJoinTiles;
    }
  SeedGeometryJoins(scene, radius_tiles, work, out);
  SpreadGeometryConstraints(work, out->join_rise, out->join_weight);
  if (scene->exterior_face_count) {
    for (int y = 0; y <= kCells; y++)
      for (int x = 0; x <= kCells; x++) {
        const int i = y * kAxis + x;
        work->distance[i] = kJoinTiles;
        if (!work->allowed[i]) continue;
        /* Continuation limits are outside all native ownership, including
         * original north/south stamp caps and shared town-border corners. */
        for (int dy = -1; dy <= 0; dy++)
          for (int dx = -1; dx <= 0; dx++)
            if (TownAt(x + dx, y + dy) || scene->replacement[(y + dy) * kCells + x + dx])
              work->allowed[i] = false;
      }
    SeedContinuationLimits(scene, ground, radius_tiles, work, out);
    SpreadGeometryConstraints(work, out->continuation_rise, out->continuation_weight);
  }
  free(work);
  return true;
}

void SimWorldNavigationMountainTransition_Destroy(SimWorldNavigationMountainTransition *out) {
  if (!out) return;
  free(out->patches);
  memset(out, 0, sizeof(*out));
}

bool SimWorldNavigationMountainTransition_Build(
    const SimWorldNavigationMountainScene *scene,
    const SimWorldNavigationTownGround *ground,
    SimWorldNavigationMountainTransition *out) {
  return SimWorldNavigationMountainTransition_BuildAtRadius(
      scene, ground, kSimWorldNavigationGlobeRadiusTiles, out);
}

bool SimWorldNavigationMountainTransition_BuildAtRadius(
    const SimWorldNavigationMountainScene *scene,
    const SimWorldNavigationTownGround *ground, float radius_tiles,
    SimWorldNavigationMountainTransition *out) {
  if (!out) return false;
  SimWorldNavigationMountainTransition_Destroy(out);
  if (!scene || !scene->atlas || !scene->town_mask || !ground || !SimWorldMap_Available()) return false;
  float normal[3];
  if (!SimWorldNavigationGlobe_SampleAtRadius(radius_tiles, 0, 0, normal, NULL)) return false;
  float *weights = calloc(kSimTownCount * kVertices, sizeof(float));
  float *distance = malloc(kCells * kCells * sizeof(float));
  uint32_t ramps[kSimTownCount][kRockShades] = {{0}};
  bool valid = weights && distance;
  for (uint8_t town = 1; valid && town <= kSimTownCount; town++) {
    if (!(scene->town_mask & (1u << (town - 1)))) continue;
    const unsigned variant = (town == 6 ? 2u : 0u) |
        (ground->development_tier[town - 1] >= 2 ? 1u : 0u);
    if (!NativeRamp(scene, variant, ramps[town - 1]) ||
        !isfinite(scene->town_maximum_rise[town - 1]) ||
        scene->town_maximum_rise[town - 1] <= 0) { valid = false; break; }
    DistanceFromTown(scene->replacement, town, distance);
    float *field = weights + (town - 1) * kVertices;
    for (int y = 0; y <= kCells; y++)
      for (int x = 0; x <= kCells; x++) {
        float total = 0;
        int count = 0;
        for (int dy = -1; dy <= 0; dy++)
          for (int dx = -1; dx <= 0; dx++) {
            const int cx = x + dx, cy = y + dy;
            if (cx >= 0 && cy >= 0 && cx < kCells && cy < kCells) {
              total += Smooth(1 - distance[cy * kCells + cx] / kBlendTiles);
              count++;
            }
          }
        field[y * kAxis + x] = count ? total / count : 0;
      }
  }
  if (valid) {
    for (int y = 0; y <= kCells; y++)
      for (int x = 0; x <= kCells; x++) {
        const int i = y * kAxis + x;
        float sum = 0;
        for (int town = 0; town < kSimTownCount; town++) sum += weights[town * kVertices + i];
        const float scale = OutsideFallbackWeight(x, y, scene->town_mask) / fmaxf(1, sum);
        for (int town = 0; town < kSimTownCount; town++) weights[town * kVertices + i] *= scale;
      }
    for (int y = 0; y <= kCells; y++)
      for (int x = 0; x <= kCells; x++) {
        const int i = y * kAxis + x;
        float sum = 0, rise = 0, normal[3], metric;
        SimWorldNavigationGlobe_SampleAtRadius(radius_tiles, (float)x, (float)y, normal, &metric);
        for (int town = 0; town < kSimTownCount; town++) {
          const float w = weights[town * kVertices + i];
          sum += w;
          rise += w * scene->town_maximum_rise[town];
        }
        /* Same physical rise/metric as native meshes, smoothly returning to
         * the original broad-range envelope outside the 12-cell apron. */
        const float target = sum > 0 ? fminf(1, rise / sum * metric /
            SimWorldNavigationTerrain_MaxMountainRise()) : 1;
        out->ridge_scale[i] = 1 + (target - 1) * fminf(1, sum);
      }
    size_t capacity = 0;
    for (int y = 0; y < kCells && valid; y++)
      for (int x = 0; x < kCells; x++) {
        if (TownAt(x, y) || SimWorldMap_MountainCoverage(x, y) <= 0) continue;
        const float *contributors[kSimTownCount];
        int towns[kSimTownCount], contributor_count = 0;
        const int vertex = y * kAxis + x;
        float influence = 0;
        for (int town = 0; town < kSimTownCount; town++) {
          const float *field = weights + town * kVertices;
          /* Bilinear interpolation of four zero corners is identically
           * zero over this cell. Retain the original order of every nonzero
           * contributor; do not quantize, prune small weights or merge ramps. */
          if (field[vertex] == 0 && field[vertex + 1] == 0 &&
              field[vertex + kAxis] == 0 && field[vertex + kAxis + 1] == 0) continue;
          contributors[contributor_count] = field;
          towns[contributor_count++] = town;
          influence += Interpolate(field, x, y, .5f, .5f);
        }
        if (influence <= 0) continue;
        if (out->patch_count == capacity) {
          capacity = capacity ? capacity * 2 : 128;
          if (capacity > kCells * kCells) capacity = kCells * kCells;
          void *patches = realloc(out->patches, capacity * sizeof(*out->patches));
          if (!patches) { valid = false; break; }
          out->patches = patches;
        }
        SimWorldNavigationMountainMaterialPatch *patch = &out->patches[out->patch_count++];
        patch->cell = (uint16_t)(y * kCells + x);
        uint8_t shades[64];
        if (!SimWorldMap_MountainShades(x, y, shades)) { valid = false; break; }
        for (int py = 0; py < kSimTownCellPixels; py++)
          for (int px = 0; px < kSimTownCellPixels; px++) {
            uint32_t *pixel = &patch->pixels[py * kSimTownCellPixels + px];
            *pixel = 0;
            const unsigned shade = shades[(py / 2) * 8 + px / 2];
            if (!shade) continue;
            float sum = 0, colour[3] = {0};
            for (int contributor = 0; contributor < contributor_count; contributor++) {
              const float w = Interpolate(contributors[contributor], x, y,
                  (px + .5f) / kSimTownCellPixels, (py + .5f) / kSimTownCellPixels);
              sum += w;
              const uint32_t c = ramps[towns[contributor]][shade - 1];
              for (int channel = 0; channel < 3; channel++) colour[channel] += w * ((c >> (channel * 8)) & 255u);
            }
            if (sum <= 0) continue;
            *pixel = (uint32_t)(fminf(1, sum) * 255 + .5f) << 24;
            for (int channel = 0; channel < 3; channel++)
              *pixel |= (uint32_t)(colour[channel] / sum + .5f) << (channel * 8);
          }
      }
  }
  if (valid) valid = BuildGeometryJoins(scene, ground, radius_tiles, out);
  free(weights);
  free(distance);
  if (!valid) SimWorldNavigationMountainTransition_Destroy(out);
  return valid;
}

bool SimWorldNavigationMountainTransition_Apply(
    const SimWorldNavigationMountainTransition *transition, uint32_t *pixels, int pitch,
    const uint8_t *cells) {
  if (!transition || !pixels || pitch < kSimWorldNavigationArtPixels) return false;
  for (size_t i = 0; i < transition->patch_count; i++) {
    const SimWorldNavigationMountainMaterialPatch *patch = &transition->patches[i];
    if (cells && !cells[patch->cell]) continue;
    const int x = patch->cell % kCells, y = patch->cell / kCells;
    for (int py = 0; py < kSimTownCellPixels; py++)
      for (int px = 0; px < kSimTownCellPixels; px++) {
        const uint32_t c = patch->pixels[py * kSimTownCellPixels + px], w = c >> 24;
        if (!w) continue;
        uint32_t *out = pixels + ((y * kSimTownCellPixels + py) * pitch + x * kSimTownCellPixels + px);
        uint32_t blended = *out & 0xFF000000u;
        for (int shift = 0; shift < 24; shift += 8)
          blended |= ((((*out >> shift) & 255u) * (255 - w) + ((c >> shift) & 255u) * w + 127) / 255) << shift;
        *out = blended;
      }
  }
  return true;
}
