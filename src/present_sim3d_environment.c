/* Shared immutable atmosphere style and frame-owned directional light.
 * Kept separate from town effect resources so globe rendering and focused
 * GPU tests use the same definitions without linking the town compositor. */
#include "present_sim3d_internal.h"

#include <math.h>

const float kPi = 3.14159265f;
const SimCloudLayer kSimCloudLayers[kSimCloudLayerCount] = {
  { 4.0f, 0.00f, 0.00f, 1.00f, 0.0060f, 0.0011f },
  { 2.7f, 0.37f, 0.61f, 0.85f, 0.0037f, 0.0008f },
  { 6.3f, 0.72f, 0.19f, 0.70f, 0.0094f, 0.0021f },
};

void SimShadowLight(const FrameSlot *slot, float *light_x, float *light_y) {
  float elevation = (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  float azimuth = (float)slot->sim.light_azimuth_deg * kPi / 180.0f;
  float sine = sinf(elevation);
  /* cot(elevation), clamped so near-horizon light cannot throw a shadow to
   * infinity. This is the original town light formula, unchanged. */
  float shear = sine > 0.05f ? cosf(elevation) / sine : 20.0f;
  if (shear > 4.0f) shear = 4.0f;
  if (shear < 0.0f) shear = 0.0f;
  *light_x = shear * cosf(azimuth);
  *light_y = shear * sinf(azimuth);
}
