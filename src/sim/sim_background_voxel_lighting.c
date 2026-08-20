#include "sim_background_voxel_lighting.h"

#include <math.h>

#include "sim_background_voxel_surface.h"

typedef struct SimBackgroundMaterialLightResponse {
  float ambient, direct;
} SimBackgroundMaterialLightResponse;

static const SimBackgroundMaterialLightResponse kDefaultMaterialResponse = {
  0.72f, 0.28f,
};
static const float kAuthoredBrightnessFloor = 0.86f;
static const float kAuthoredBrightnessRange = 0.14f;
static const float kLightingGeometryEpsilon = 0.05f;
static const float kGroundContactFloor = 0.84f;
static const float kGroundContactRange = 0.16f;
static const float kLowLedgeHeightFraction = 0.68f;
static const float kLowLedgeLightFactor = 0.95f;

static SimBackgroundMaterialLightResponse MaterialResponse(
    SimBackgroundVoxelMaterial material) {
  switch (material) {
    case kSimVoxelMaterial_Leaves:
    case kSimVoxelMaterial_LeavesLight:
    case kSimVoxelMaterial_LeavesDark:
      return (SimBackgroundMaterialLightResponse){0.78f, 0.22f};
    case kSimVoxelMaterial_Roof:
    case kSimVoxelMaterial_RoofLight:
      return (SimBackgroundMaterialLightResponse){0.68f, 0.32f};
    case kSimVoxelMaterial_Metal:
    case kSimVoxelMaterial_Blade:
    case kSimVoxelMaterial_Gold:
    case kSimVoxelMaterial_Glass:
      return (SimBackgroundMaterialLightResponse){0.62f, 0.38f};
    case kSimVoxelMaterial_Snow:
      return (SimBackgroundMaterialLightResponse){0.82f, 0.18f};
    case kSimVoxelMaterial_Dark:
    case kSimVoxelMaterial_Wood:
    case kSimVoxelMaterial_Contact:
      return (SimBackgroundMaterialLightResponse){0.76f, 0.24f};
    case kSimVoxelMaterial_Paving:
    case kSimVoxelMaterial_Foundation:
      return (SimBackgroundMaterialLightResponse){0.74f, 0.26f};
    case kSimVoxelMaterial_Wall:
    case kSimVoxelMaterial_WallLight:
    case kSimVoxelMaterial_Trim:
    case kSimVoxelMaterial_Trunk:
    case kSimVoxelMaterial_Count:
      return kDefaultMaterialResponse;
  }
  return kDefaultMaterialResponse;
}

void SimBackgroundVoxelLighting_ResolveDirection(
    uint16_t light_azimuth_deg,
    uint8_t light_elevation_deg,
    SimBackgroundVoxelLightDirection *out) {
  if (!out) return;
  const float kPi = 3.14159265f;
  float azimuth = light_azimuth_deg * kPi / 180.0f;
  float elevation = light_elevation_deg * kPi / 180.0f;
  float horizontal = cosf(elevation);
  /* The setting names the direction a shadow is thrown, so the light itself
   * comes from the opposite horizontal direction. */
  *out = (SimBackgroundVoxelLightDirection){
    -cosf(azimuth) * horizontal,
    -sinf(azimuth) * horizontal,
    sinf(elevation),
  };
}

uint8_t SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelShading shading,
    const SimBackgroundVoxelLightDirection *light) {
  if (!face || !light) return 0;
  SimBackgroundVoxelSurfaceNormal normal;
  if (!SimBackgroundVoxelSurface_OutwardNormal(face, &normal))
    return face->brightness;

  float diffuse = normal.x * light->x + normal.y * light->y +
      normal.z * light->z;
  if (diffuse < 0.0f) diffuse = 0.0f;
  if (diffuse > 1.0f) diffuse = 1.0f;
  float authored = kAuthoredBrightnessFloor +
      kAuthoredBrightnessRange * face->brightness / (float)UINT8_MAX;
  SimBackgroundMaterialLightResponse response = kDefaultMaterialResponse;
  if (shading == kSimBackgroundVoxelShading_MaterialAware)
    response = MaterialResponse(
        (SimBackgroundVoxelMaterial)face->material);
  float lit = authored *
      (response.ambient + response.direct * diffuse);
  if (lit > 1.0f) lit = 1.0f;
  return (uint8_t)(lit * UINT8_MAX + 0.5f);
}

uint8_t SimBackgroundVoxelLighting_FaceBrightness(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelShading shading,
    uint16_t light_azimuth_deg,
    uint8_t light_elevation_deg) {
  SimBackgroundVoxelLightDirection light;
  SimBackgroundVoxelLighting_ResolveDirection(
      light_azimuth_deg, light_elevation_deg, &light);
  return SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
      face, shading, &light);
}

void SimBackgroundVoxelLighting_VertexBrightnesses(
    const SimBackgroundVoxelModelFace *face,
    const SimBackgroundVoxelModel *model,
    uint8_t directional_brightness,
    SimBackgroundVoxelShading shading,
    uint8_t out[4]) {
  if (!out) return;
  for (int point = 0; point < 4; point++)
    out[point] = directional_brightness;
  if (!face || !model ||
      shading < kSimBackgroundVoxelShading_AmbientOcclusion)
    return;
  float face_min_z = face->points[0].z;
  float face_max_z = face->points[0].z;
  for (int i = 1; i < 4; i++) {
    if (face->points[i].z < face_min_z) face_min_z = face->points[i].z;
    if (face->points[i].z > face_max_z) face_max_z = face->points[i].z;
  }
  for (int point = 0; point < 4; point++) {
    float factor = 1.0f;
    if (face_max_z - face_min_z > kLightingGeometryEpsilon) {
      float height = face->points[point].z - model->min_z;
      float range = model->max_z - model->min_z;
      float normalized = range > kLightingGeometryEpsilon
          ? height / range : 1.0f;
      if (normalized < 0.0f) normalized = 0.0f;
      if (normalized > 1.0f) normalized = 1.0f;
      /* Contact darkening is strongest at the ground and smoothly releases up
       * walls, trunks and courtyard faces. Four vertex values preserve batching
       * and let the renderer interpolate the gradient without extra geometry. */
      factor = kGroundContactFloor +
          kGroundContactRange * sqrtf(normalized);
    } else if (face_max_z < model->max_z * kLowLedgeHeightFraction) {
      /* Low horizontal ledges sit underneath other mass and receive less sky. */
      factor = kLowLedgeLightFactor;
    }
    factor *= face->occlusion[point] / (float)UINT8_MAX;
    float shaded = directional_brightness * factor;
    if (shaded < 0.0f) shaded = 0.0f;
    if (shaded > UINT8_MAX) shaded = UINT8_MAX;
    out[point] = (uint8_t)(shaded + 0.5f);
  }
}

uint8_t SimBackgroundVoxelLighting_VertexBrightness(
    const SimBackgroundVoxelModelFace *face,
    const SimBackgroundVoxelModel *model,
    int point,
    uint8_t directional_brightness,
    SimBackgroundVoxelShading shading) {
  if (point < 0 || point >= 4) return directional_brightness;
  uint8_t brightness[4];
  SimBackgroundVoxelLighting_VertexBrightnesses(
      face, model, directional_brightness, shading, brightness);
  return brightness[point];
}
