#include "sim_world_navigation_clouds.h"

#include <math.h>
#include <stddef.h>

static const float kCloudPi = 3.14159265358979323846f;
enum { kLongitudeBlock = 512, kCloudOctaves = 5 };

static float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

static float Hash(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u +
      (uint32_t)z * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) & 65535u) / 65535.0f;
}

typedef struct CloudNoiseCell {
  int x, z;
  bool valid;
  float start[2][2], delta[2][2];
} CloudNoiseCell;

static float Noise(float x, float z, int iy, float fy, CloudNoiseCell *cell) {
  const int ix = (int)floorf(x), iz = (int)floorf(z);
  const float fx = Smooth(x - ix), fz = Smooth(z - iz);
  if (!cell->valid || cell->x != ix || cell->z != iz) {
    for (int dz = 0; dz < 2; dz++)
      for (int dy = 0; dy < 2; dy++) {
        const float a = Hash(ix, iy + dy, iz + dz);
        cell->start[dz][dy] = a;
        cell->delta[dz][dy] = Hash(ix + 1, iy + dy, iz + dz) - a;
      }
    cell->x = ix; cell->z = iz; cell->valid = true;
  }
  float plane[2];
  for (int dz = 0; dz < 2; dz++) {
    float row[2];
    for (int dy = 0; dy < 2; dy++)
      row[dy] = cell->start[dz][dy] + cell->delta[dz][dy] * fx;
    plane[dz] = row[0] + (row[1] - row[0]) * fy;
  }
  return plane[0] + (plane[1] - plane[0]) * fz;
}

static void BakeRow(uint32_t *out, int count, const float *cosine, const float *sine,
                    float radius, float y, float scale) {
  /* Every longitude in an octave has the same lattice Y and interpolation
   * weight. Adjacent texels often share its X/Z cell too. Retain only that
   * cell's eight hashes; never approximate or interpolate the noise field at
   * a lower resolution. Reset it for every latitude, octave and fixed block. */
  float total[kLongitudeBlock] = {0};
  float amplitude = 0.5f, sum = 0, frequency = scale * 1.5f;
  for (int octave = 0; octave < kCloudOctaves; octave++) {
    const float yy = y * frequency + 0.61f + octave * 0.29f;
    const int iy = (int)floorf(yy);
    const float fy = Smooth(yy - iy);
    CloudNoiseCell cell = {0};
    for (int column = 0; column < count; column++) {
      const float x = radius * cosine[column], z = radius * sine[column];
      total[column] += Noise(x * frequency + 0.37f + octave * 0.53f,
          z * frequency + 0.23f + octave * 0.71f, iy, fy, &cell) * amplitude;
    }
    sum += amplitude;
    amplitude *= 0.5f;
    frequency *= 2;
  }
  /* Same density/tint language as the town shroud, extended into 3D. */
  for (int column = 0; column < count; column++) {
    float density = (total[column] / sum - 0.42f) / 0.38f;
    density = Smooth(fminf(1, fmaxf(0, density)));
    const unsigned alpha = (unsigned)(density * 255 + 0.5f);
    const unsigned tint = 236 + (unsigned)(density * 19);
    out[column] = (alpha << 24) | (tint << 16) | (tint << 8) | 255u;
  }
}

bool SimWorldNavigationClouds_Bake(
    uint32_t *out, int pitch, int width, int height, float scale) {
  if (!out || width < 2 || height < 2 || pitch < width ||
      !isfinite(scale) || scale <= 0 || scale > 32) return false;
  /* Poles are single samples. Longitude depends only on the column, not
   * latitude or noise octave. A fixed block bounds stack use for arbitrary
   * bake dimensions without heap allocation, VLAs or platform-specific SIMD. */
  const float zero = 0;
  uint32_t north, south;
  BakeRow(&north, 1, &zero, &zero, 1, 1, scale);
  BakeRow(&south, 1, &zero, &zero, 1, -1, scale);
  for (int x = 0; x < width; x++) {
    out[x] = north;
    out[(size_t)(height - 1) * pitch + x] = south;
  }
  for (int first = 0; first < width;) {
    const int count = width - first < kLongitudeBlock ? width - first : kLongitudeBlock;
    float cosine[kLongitudeBlock], sine[kLongitudeBlock];
    for (int column = 0; column < count; column++) {
      const int x = first + column;
      const float longitude = x == width - 1 ? -kCloudPi
          : 2 * kCloudPi * x / (width - 1) - kCloudPi;
      cosine[column] = cosf(longitude);
      sine[column] = sinf(longitude);
    }
    for (int y = 1; y < height - 1; y++) {
      const float angle = kCloudPi * y / (height - 1);
      const float ny = cosf(angle), radius = sinf(angle);
      BakeRow(out + (size_t)y * pitch + first, count, cosine, sine, radius, ny, scale);
    }
    first += count;
  }
  return true;
}

SimWorldNavigationCloudRotation SimWorldNavigationClouds_Rotation(float u, float v) {
  return (SimWorldNavigationCloudRotation){
    cosf(u * 2 * kCloudPi), sinf(u * 2 * kCloudPi),
    cosf(v * 2 * kCloudPi), sinf(v * 2 * kCloudPi),
  };
}

SimWorldNavigationCloudCoordinate SimWorldNavigationClouds_Coordinate(
    const float n[3], const SimWorldNavigationCloudRotation *r) {
  const float x = n[0] * r->cos_u + n[2] * r->sin_u;
  const float z = n[2] * r->cos_u - n[0] * r->sin_u;
  const float y = n[1] * r->cos_v + z * r->sin_v;
  const float zz = z * r->cos_v - n[1] * r->sin_v;
  return (SimWorldNavigationCloudCoordinate){x, y, zz,
      (atan2f(zz, x) + kCloudPi) / (2 * kCloudPi),
      acosf(fminf(1, fmaxf(-1, y))) / kCloudPi};
}

void SimWorldNavigationClouds_UV(
    const float n[3], const SimWorldNavigationCloudRotation *r, float *u, float *v) {
  const SimWorldNavigationCloudCoordinate coordinate = SimWorldNavigationClouds_Coordinate(n, r);
  *u = coordinate.u; *v = coordinate.v;
}

bool SimWorldNavigationClouds_NeedsSplit(const SimWorldNavigationCloudCoordinate quad[4]) {
  for (int i = 1; i < 4; i++)
    if ((quad[0].x < 0) != (quad[i].x < 0) ||
        (quad[0].z < 0) != (quad[i].z < 0)) return true;
  return false;
}

typedef struct CloudChartVertex {
  double direction[3], weight[3];
} CloudChartVertex;

static int ClipCloudChart(const CloudChartVertex *input, int count,
                         int axis, float sign, CloudChartVertex *out) {
  int used = 0;
  for (int i = 0; i < count; i++) {
    const CloudChartVertex *a = &input[(i + count - 1) % count], *b = &input[i];
    const double da = a->direction[axis] * sign, db = b->direction[axis] * sign;
    if ((da < 0) != (db < 0)) {
      /* Shared edges are visited in opposite orders by adjacent triangles.
       * Always interpolate from the negative axis endpoint, with the same
       * arithmetic, to keep their rasterized intersections bit-identical. */
      const CloudChartVertex *lo = a->direction[axis] < b->direction[axis] ? a : b;
      const CloudChartVertex *hi = lo == a ? b : a;
      const double t = lo->direction[axis] / (lo->direction[axis] - hi->direction[axis]);
      CloudChartVertex *v = &out[used++];
      for (int j = 0; j < 3; j++) {
        v->direction[j] = (1 - t) * lo->direction[j] + t * hi->direction[j];
        v->weight[j] = (1 - t) * lo->weight[j] + t * hi->weight[j];
      }
      v->direction[axis] = 0;
    }
    if (db >= 0) out[used++] = *b;
  }
  return used;
}

int SimWorldNavigationClouds_SplitTriangle(
    const SimWorldNavigationCloudCoordinate triangle[3],
    SimWorldNavigationCloudPatch out[kSimWorldNavigationCloudMaxPatches]) {
  CloudChartVertex original[3] = {0};
  for (int i = 0; i < 3; i++) {
    original[i].direction[0] = triangle[i].x;
    original[i].direction[1] = triangle[i].y;
    original[i].direction[2] = triangle[i].z;
    original[i].weight[i] = 1;
  }
  int used = 0;
  for (int chart = 0; chart < 4; chart++) {
    if ((chart == 0 || chart == 3) &&
        triangle[0].x == 0 && triangle[1].x == 0 && triangle[2].x == 0) continue;
    if (chart < 2 && triangle[0].z == 0 && triangle[1].z == 0 && triangle[2].z == 0) continue;
    /* A triangle clipped by two planes has at most five corners. */
    CloudChartVertex half[4], polygon[5];
    const int first = ClipCloudChart(original, 3, 0, chart == 0 || chart == 3 ? -1 : 1, half);
    const int count = ClipCloudChart(half, first, 2, chart < 2 ? -1 : 1, polygon);
    if (count < 3) continue;
    float u[5], v[5];
    for (int i = 0; i < count; i++) {
      const double *n = polygon[i].direction;
      const double radius = hypot(n[0], n[2]);
      /* atan2 at the pole has no unique longitude. Each quarter uses its
       * midpoint; all pole texels are identical in the spherical atlas. */
      u[i] = radius < 1e-7f ? (chart + .5f) * .25f :
          (float)((atan2(n[2], n[0]) + kCloudPi) / (2 * kCloudPi));
      if (n[2] == 0 && n[0] < 0) u[i] = chart == 0 ? 0 : 1;
      v[i] = (float)(atan2(radius, n[1]) / kCloudPi);
    }
    for (int start = 1; start < count - 1; start += 2) {
      const int corners[4] = {0, start, start + 1, start + 2 < count ? start + 2 : start + 1};
      SimWorldNavigationCloudPatch *patch = &out[used++];
      for (int p = 0; p < 4; p++) {
        const int at = corners[p];
        for (int j = 0; j < 3; j++) patch->weight[p][j] = polygon[at].weight[j];
        patch->u[p] = u[at]; patch->v[p] = v[at];
      }
    }
  }
  return used;
}

void SimWorldNavigationClouds_Unwrap(float u[4]) {
  float minimum = u[0], maximum = u[0];
  for (int i = 1; i < 4; i++) {
    minimum = fminf(minimum, u[i]);
    maximum = fmaxf(maximum, u[i]);
  }
  if (maximum - minimum > 0.5f)
    for (int i = 0; i < 4; i++) if (u[i] < 0.5f) u[i] += 1;
}
