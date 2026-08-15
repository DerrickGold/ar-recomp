#include "sim/sim_background_voxel_lighting.h"

#include <stdio.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static SimBackgroundVoxelModelFace VerticalFace(
    SimBackgroundVoxelMaterial material) {
  return (SimBackgroundVoxelModelFace){
    .points = {
      {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 10.0f}, {1.0f, 0.0f, 10.0f},
    },
    .material = material,
    .brightness = 255,
    .occlusion = {255, 255, 255, 255},
  };
}

int main(void) {
  SimBackgroundVoxelModelFace wall = VerticalFace(kSimVoxelMaterial_Wall);
  uint8_t basic = SimBackgroundVoxelLighting_FaceBrightness(
      &wall, kSimBackgroundVoxelShading_Basic, 0, 45);
  SimBackgroundVoxelLightDirection direction;
  SimBackgroundVoxelLighting_ResolveDirection(0, 45, &direction);
  CHECK(basic == SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
      &wall, kSimBackgroundVoxelShading_Basic, &direction));
  uint8_t ao_face = SimBackgroundVoxelLighting_FaceBrightness(
      &wall, kSimBackgroundVoxelShading_AmbientOcclusion, 0, 45);
  CHECK(basic == ao_face);  /* AO is vertex-local, never a second light. */

  SimBackgroundVoxelModelFace leaves =
      VerticalFace(kSimVoxelMaterial_Leaves);
  SimBackgroundVoxelModelFace metal = VerticalFace(kSimVoxelMaterial_Metal);
  uint8_t leaf_light = SimBackgroundVoxelLighting_FaceBrightness(
      &leaves, kSimBackgroundVoxelShading_MaterialAware, 0, 45);
  uint8_t metal_light = SimBackgroundVoxelLighting_FaceBrightness(
      &metal, kSimBackgroundVoxelShading_MaterialAware, 0, 45);
  CHECK(leaf_light != metal_light);

  SimBackgroundVoxelModel model = {
    .min_z = 0.0f,
    .max_z = 10.0f,
  };
  uint8_t basic_bottom = SimBackgroundVoxelLighting_VertexBrightness(
      &wall, &model, 0, basic, kSimBackgroundVoxelShading_Basic);
  uint8_t ao_bottom = SimBackgroundVoxelLighting_VertexBrightness(
      &wall, &model, 0, basic,
      kSimBackgroundVoxelShading_AmbientOcclusion);
  uint8_t ao_top = SimBackgroundVoxelLighting_VertexBrightness(
      &wall, &model, 2, basic,
      kSimBackgroundVoxelShading_AmbientOcclusion);
  uint8_t brightness[4];
  SimBackgroundVoxelLighting_VertexBrightnesses(
      &wall, &model, basic,
      kSimBackgroundVoxelShading_AmbientOcclusion, brightness);
  CHECK(brightness[0] == ao_bottom && brightness[2] == ao_top);
  CHECK(basic_bottom == basic);
  CHECK(ao_bottom < ao_top);
  CHECK(ao_top == basic);

  if (failures) {
    fprintf(stderr, "%d sim background voxel lighting checks failed\n",
            failures);
    return 1;
  }
  puts("sim background voxel lighting checks passed");
  return 0;
}
