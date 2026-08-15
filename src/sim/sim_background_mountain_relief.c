#include "sim_background_mountain_relief.h"

#include <string.h>

void SimBackgroundMountainRelief_Resolve(
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelShading shading,
    SimBackgroundMountainRelief *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  /* Every preset shares one authored presentation plane. Changing quality
   * may alter thickness, never the range's silhouette or facing angle. */
  /* Mountains share the building billboard's camera-facing axis, but retain
   * a hillside pitch. A vertical plane turns map-edge ranges into walls; this
   * 26-degree authored slope keeps the SNES perspective while preventing the
   * art from lying flat on the ground. */
  out->face_height_scale = 0.30f;
  out->face_depth_scale = 0.62f;

  switch (detail) {
    case kSimBackgroundVoxelDetail_Low:
      out->side_band_count = 1;
      out->side_alpha = 128;
      out->depth_pixels = 1.0f;
      break;
    case kSimBackgroundVoxelDetail_Balanced:
      out->side_band_count = 2;
      out->side_alpha = 152;
      out->depth_pixels = 1.5f;
      break;
    case kSimBackgroundVoxelDetail_High:
      out->side_band_count = 3;
      out->side_alpha = 178;
      out->depth_pixels = 2.25f;
      break;
    case kSimBackgroundVoxelDetail_Ultra:
      out->side_band_count = 4;
      out->side_alpha = 204;
      out->depth_pixels = 3.0f;
      break;
    case kSimBackgroundVoxelDetail_Count:
      return;
  }

  /* Stronger shading tiers separate the repeated edge bands a little more,
   * without recolouring the authentic top surface. */
  uint8_t darkest = shading == kSimBackgroundVoxelShading_Basic ? 208 :
      shading == kSimBackgroundVoxelShading_AmbientOcclusion ? 190 : 178;
  for (uint8_t band = 0; band < out->side_band_count; band++) {
    unsigned span = 230u - darkest;
    unsigned denominator = out->side_band_count > 1
        ? out->side_band_count - 1u : 1u;
    out->side_brightness[band] = (uint8_t)(
        darkest + span * band / denominator);
  }
}
