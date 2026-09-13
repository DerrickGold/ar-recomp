#ifndef SIM_WORLD_NAVIGATION_SKY_CLOUDS_H
#define SIM_WORLD_NAVIGATION_SKY_CLOUDS_H

#include <stdbool.h>
#include <stdint.h>

enum {
  kSimSkyCloudWidth = 96, kSimSkyCloudHeight = 48,
  kSimSkyCloudSlices = 16, kSimSkyCloudBanks = 4,
  kSimSkyCloudColumns = 4, kSimSkyCloudRows = 5,
  kSimSkyCloudAtlasWidth = kSimSkyCloudWidth * kSimSkyCloudColumns,
  kSimSkyCloudAtlasHeight = kSimSkyCloudHeight * kSimSkyCloudRows * kSimSkyCloudBanks,
};

/* Lit 3D density volumes, stored as sixteen front-to-back XY slices per
 * bank. Tile sixteen is their precomposited low-cost fallback. Transparent
 * gutters prevent interpolation across slices. ARGB integer output; pitch
 * in pixels. Light points toward the sun in volume-local XYZ (Y down, Z
 * away from the viewer). No renderer, frame state or retained allocation. */
bool SimWorldNavigationSkyClouds_Bake(uint32_t *out, int pitch,
                                     const float light[3], bool lighting);

/* Conservative bilinear support in slice-local texel-centre coordinates.
 * Includes a one-texel margin around nonzero alpha, clamped to the original
 * slice. Baked gutters are transparent; no alpha threshold or density
 * approximation is used. x0==x1 denotes an empty slice. Compute
 * once after a bake, not during presentation. No mip/anisotropic footprint is
 * implied. Invalid input leaves bounds untouched; pitch is in pixels. */
typedef struct SimSkyCloudBounds { uint8_t x0, y0, x1, y1; } SimSkyCloudBounds;
bool SimWorldNavigationSkyClouds_Bounds(const uint32_t *pixels, int pitch,
    int bank, int slice, SimSkyCloudBounds *bounds);

#endif
