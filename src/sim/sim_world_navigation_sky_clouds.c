#include "sim_world_navigation_sky_clouds.h"

#include <math.h>
#include <stddef.h>

static float Saturate(float x) { return fminf(1, fmaxf(0, x)); }
static float Smooth(float x) { x = Saturate(x); return x * x * (3 - 2 * x); }

static float Noise(float x, float y, float z) {
  const int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
  const float fx = Smooth(x - ix), fy = Smooth(y - iy), fz = Smooth(z - iz);
  float result = 0;
  for (int dz = 0; dz < 2; dz++)
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++) {
        uint32_t hash = (uint32_t)(ix + dx) * 374761393u +
            (uint32_t)(iy + dy) * 668265263u + (uint32_t)(iz + dz) * 2246822519u;
        hash = (hash ^ (hash >> 13)) * 1274126177u;
        result += ((hash ^ (hash >> 16)) & 65535u) / 65535.0f *
            (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
      }
  return result;
}

typedef struct Billow { float x, y, rx, ry; } Billow;
_Static_assert(kSimSkyCloudWidth - 1 <= UINT8_MAX && kSimSkyCloudHeight - 1 <= UINT8_MAX,
    "slice-local support bounds must fit their portable coordinates");
static const Billow kShapes[kSimSkyCloudBanks][6] = {
  {{.15f,.65f,.13f,.16f}, {.28f,.53f,.18f,.28f}, {.44f,.42f,.20f,.34f},
   {.61f,.50f,.18f,.27f}, {.76f,.60f,.15f,.19f}, {.87f,.68f,.10f,.11f}},
  {{.13f,.68f,.10f,.12f}, {.25f,.60f,.17f,.20f}, {.41f,.53f,.20f,.28f},
   {.59f,.39f,.19f,.34f}, {.73f,.54f,.17f,.24f}, {.87f,.65f,.10f,.15f}},
  {{.13f,.64f,.10f,.14f}, {.27f,.50f,.18f,.27f}, {.45f,.48f,.20f,.30f},
   {.63f,.55f,.17f,.23f}, {.77f,.51f,.14f,.22f}, {.88f,.66f,.09f,.12f}},
  {{.13f,.68f,.11f,.12f}, {.28f,.57f,.18f,.21f}, {.44f,.36f,.18f,.31f},
   {.60f,.43f,.20f,.32f}, {.75f,.59f,.16f,.18f}, {.87f,.67f,.10f,.12f}},
};

static float Density(int bank, float x, float y, float z) {
  if (x <= 0 || x >= 1 || y <= 0 || y >= 1 || z <= 0 || z >= 1) return 0;
  float density = 0;
  for (int i = 0; i < 6; i++) {
    const Billow *p = &kShapes[bank][i];
    const float centre_z = .46f + ((i + bank) % 3) * .06f;
    const float dx = (x - p->x) / p->rx, dy = (y - p->y) / p->ry;
    const float dz = (z - centre_z) / (p->rx * 1.8f);
    density = fmaxf(density, 1 - dx * dx - dy * dy - dz * dz);
  }
  if (density <= 0) return 0;
  /* Coherent 3D erosion breaks up the ellipsoid outlines and lights smaller
   * billows; identical spatial density is used by extinction and sun probes. */
  const float erosion = .55f * Noise(x * 19 + bank * 3, y * 9, z * 9) +
      .18f * Noise(x * 43, y * 23 + bank * 5, z * 21);
  return Smooth((density - erosion * .65f) * 2.4f) *
      (1 - Smooth((y - .67f) / .20f));
}

static size_t Texel(int bank, int slice, int x, int y, int pitch) {
  return (size_t)(bank * kSimSkyCloudRows * kSimSkyCloudHeight +
      slice / kSimSkyCloudColumns * kSimSkyCloudHeight + y) * pitch +
      slice % kSimSkyCloudColumns * kSimSkyCloudWidth + x;
}

bool SimWorldNavigationSkyClouds_Bounds(const uint32_t *pixels, int pitch,
    int bank, int slice, SimSkyCloudBounds *bounds) {
  if (!pixels || !bounds || pitch < kSimSkyCloudAtlasWidth ||
      (size_t)pitch > SIZE_MAX / sizeof(*pixels) / kSimSkyCloudAtlasHeight ||
      bank < 0 || bank >= kSimSkyCloudBanks || slice < 0 || slice > kSimSkyCloudSlices)
    return false;
  int x0 = kSimSkyCloudWidth, y0 = kSimSkyCloudHeight, x1 = -1, y1 = -1;
  for (int y = 0; y < kSimSkyCloudHeight; ++y)
    for (int x = 0; x < kSimSkyCloudWidth; ++x) {
      if (!(pixels[Texel(bank, slice, x, y, pitch)] >> 24)) continue;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  *bounds = x1 < 0 ? (SimSkyCloudBounds){0} : (SimSkyCloudBounds){
    x0 > 0 ? x0 - 1 : 0, y0 > 0 ? y0 - 1 : 0,
    x1 + 1 < kSimSkyCloudWidth ? x1 + 1 : kSimSkyCloudWidth - 1,
    y1 + 1 < kSimSkyCloudHeight ? y1 + 1 : kSimSkyCloudHeight - 1};
  return true;
}

bool SimWorldNavigationSkyClouds_Bake(uint32_t *out, int pitch,
                                     const float light[3], bool lighting) {
  if (!out || !light || pitch < kSimSkyCloudAtlasWidth ||
      (size_t)pitch > SIZE_MAX / sizeof(*out) / kSimSkyCloudAtlasHeight ||
      !isfinite(light[0]) || !isfinite(light[1]) || !isfinite(light[2])) return false;
  const float length = hypotf(hypotf(light[0], light[1]), light[2]);
  if (!isfinite(length) || (lighting && length < .0001f)) return false;
  const float step[3] = {lighting ? light[0] / length * .1f : 0,
      lighting ? light[1] / length * .1f : 0, lighting ? light[2] / length * .1f : 0};
  for (int y = 0; y < kSimSkyCloudAtlasHeight; y++)
    for (int x = 0; x < kSimSkyCloudAtlasWidth; x++)
      out[(size_t)y * pitch + x] = 0x00ffffffu;
  for (int bank = 0; bank < kSimSkyCloudBanks; bank++)
    for (int y = 1; y < kSimSkyCloudHeight - 1; y++)
      for (int x = 1; x < kSimSkyCloudWidth - 1; x++) {
        const float u = (x - .5f) / (kSimSkyCloudWidth - 2);
        const float v = (y - .5f) / (kSimSkyCloudHeight - 2);
        float accumulated[4] = {0};
        /* Back-to-front integration also bakes the single-plane fallback.
         * Lighting uses six bounded density probes toward the sun, once per
         * atlas/light key, never per frame or screen pixel. */
        for (int z = kSimSkyCloudSlices - 1; z >= 0; z--) {
          const float w = (z + .5f) / kSimSkyCloudSlices;
          const float density = Density(bank, u, v, w);
          if (density <= 0) continue;
          const float alpha = 1 - expf(-density * 8 / kSimSkyCloudSlices);
          float optical_depth = 0;
          if (lighting)
            for (int sample = 1; sample <= 6; sample++)
              optical_depth += Density(bank, u + step[0] * sample,
                  v + step[1] * sample, w + step[2] * sample) * .55f;
          const float sun = expf(-optical_depth);
          const float rgb[3] = {lighting ? .58f + .42f * sun : 1,
              lighting ? .70f + .29f * sun : 1, lighting ? .85f + .13f * sun : 1};
          const unsigned a = (unsigned)(alpha * 255 + .5f);
          if (a) out[Texel(bank, z, x, y, pitch)] = (a << 24) |
                ((unsigned)(rgb[0] * 255 + .5f) << 16) |
                ((unsigned)(rgb[1] * 255 + .5f) << 8) | (unsigned)(rgb[2] * 255 + .5f);
          for (int c = 0; c < 3; c++)
            accumulated[c] = rgb[c] * alpha + accumulated[c] * (1 - alpha);
          accumulated[3] = alpha + accumulated[3] * (1 - alpha);
        }
        if (accumulated[3] * 255 >= .5f) {
          const float inverse = 1 / accumulated[3];
          out[Texel(bank, kSimSkyCloudSlices, x, y, pitch)] =
              ((unsigned)(accumulated[3] * 255 + .5f) << 24) |
              ((unsigned)(Saturate(accumulated[0] * inverse) * 255 + .5f) << 16) |
              ((unsigned)(Saturate(accumulated[1] * inverse) * 255 + .5f) << 8) |
              (unsigned)(Saturate(accumulated[2] * inverse) * 255 + .5f);
        }
      }
  return true;
}
