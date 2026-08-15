#include "sim_background_voxel_lighting.h"

#include <math.h>

static void MaterialResponse(SimBackgroundVoxelMaterial material,
                             float *ambient, float *direct) {
  *ambient = 0.63f;
  *direct = 0.37f;
  switch (material) {
    case kSimVoxelMaterial_Leaves:
    case kSimVoxelMaterial_LeavesLight:
    case kSimVoxelMaterial_LeavesDark:
      *ambient = 0.72f;
      *direct = 0.28f;
      break;
    case kSimVoxelMaterial_Roof:
    case kSimVoxelMaterial_RoofLight:
      *ambient = 0.57f;
      *direct = 0.43f;
      break;
    case kSimVoxelMaterial_Metal:
    case kSimVoxelMaterial_Blade:
    case kSimVoxelMaterial_Gold:
    case kSimVoxelMaterial_Glass:
      *ambient = 0.49f;
      *direct = 0.51f;
      break;
    case kSimVoxelMaterial_Snow:
      *ambient = 0.76f;
      *direct = 0.24f;
      break;
    case kSimVoxelMaterial_Dark:
    case kSimVoxelMaterial_Wood:
    case kSimVoxelMaterial_Contact:
      *ambient = 0.69f;
      *direct = 0.31f;
      break;
    case kSimVoxelMaterial_Paving:
      *ambient = 0.60f;
      *direct = 0.40f;
      break;
    case kSimVoxelMaterial_Wall:
    case kSimVoxelMaterial_WallLight:
    case kSimVoxelMaterial_Trim:
    case kSimVoxelMaterial_Trunk:
    case kSimVoxelMaterial_Count:
      break;
  }
}

uint8_t SimBackgroundVoxelLighting_FaceBrightness(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelShading shading,
    uint16_t light_azimuth_deg,
    uint8_t light_elevation_deg) {
  if (!face) return 0;
  const SimBackgroundVoxelModelPoint *a = &face->points[0];
  const SimBackgroundVoxelModelPoint *b = &face->points[1];
  const SimBackgroundVoxelModelPoint *d = &face->points[3];
  float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
  float vx = d->x - a->x, vy = d->y - a->y, vz = d->z - a->z;
  float nx = uy * vz - uz * vy;
  float ny = uz * vx - ux * vz;
  float nz = ux * vy - uy * vx;
  float length = sqrtf(nx * nx + ny * ny + nz * nz);
  if (length < 0.0001f) return face->brightness;
  nx /= length;
  ny /= length;
  nz /= length;
  /* AddBox authors side faces from the inside winding and top faces from the
   * outside winding. Correct that convention before applying world light. */
  if (fabsf(nz) < 0.5f) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  } else if (nz < 0.0f) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  }

  const float kPi = 3.14159265f;
  float azimuth = light_azimuth_deg * kPi / 180.0f;
  float elevation = light_elevation_deg * kPi / 180.0f;
  float horizontal = cosf(elevation);
  /* The setting names the direction a shadow is thrown, so the light itself
   * comes from the opposite horizontal direction. */
  float light_x = -cosf(azimuth) * horizontal;
  float light_y = -sinf(azimuth) * horizontal;
  float light_z = sinf(elevation);
  float diffuse = nx * light_x + ny * light_y + nz * light_z;
  if (diffuse < 0.0f) diffuse = 0.0f;
  if (diffuse > 1.0f) diffuse = 1.0f;
  float authored = 0.86f + 0.14f * face->brightness / 255.0f;
  float ambient = 0.63f, direct = 0.37f;
  if (shading == kSimBackgroundVoxelShading_MaterialAware)
    MaterialResponse((SimBackgroundVoxelMaterial)face->material,
                     &ambient, &direct);
  float lit = authored * (ambient + direct * diffuse);
  if (lit > 1.0f) lit = 1.0f;
  return (uint8_t)(lit * 255.0f + 0.5f);
}

uint8_t SimBackgroundVoxelLighting_VertexBrightness(
    const SimBackgroundVoxelModelFace *face,
    const SimBackgroundVoxelModel *model,
    int point,
    uint8_t directional_brightness,
    SimBackgroundVoxelShading shading) {
  if (!face || !model || point < 0 || point >= 4)
    return directional_brightness;
  if (shading < kSimBackgroundVoxelShading_AmbientOcclusion)
    return directional_brightness;
  float face_min_z = face->points[0].z;
  float face_max_z = face->points[0].z;
  for (int i = 1; i < 4; i++) {
    if (face->points[i].z < face_min_z) face_min_z = face->points[i].z;
    if (face->points[i].z > face_max_z) face_max_z = face->points[i].z;
  }
  float factor = 1.0f;
  if (face_max_z - face_min_z > 0.05f) {
    float height = face->points[point].z - model->min_z;
    float range = model->max_z - model->min_z;
    float normalized = range > 0.05f ? height / range : 1.0f;
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    /* Contact darkening is strongest at the ground and smoothly releases up
     * walls, trunks and courtyard faces. Four vertex values preserve batching
     * and let the renderer interpolate the gradient without extra geometry. */
    factor = 0.76f + 0.24f * sqrtf(normalized);
  } else if (face_max_z < model->max_z * 0.68f) {
    /* Low horizontal ledges sit underneath other mass and receive less sky. */
    factor = 0.91f;
  }
  factor *= face->occlusion[point] / 255.0f;
  float shaded = directional_brightness * factor;
  if (shaded < 0.0f) shaded = 0.0f;
  if (shaded > 255.0f) shaded = 255.0f;
  return (uint8_t)(shaded + 0.5f);
}
