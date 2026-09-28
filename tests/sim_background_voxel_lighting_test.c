#include "sim/voxels/sim_background_voxel_lighting.h"
#include "sim/voxels/sim_background_voxel_surface.h"

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
  SimBackgroundVoxelSurfaceNormal normal;
  CHECK(SimBackgroundVoxelSurface_OutwardNormal(&wall, &normal));
  CHECK(normal.x == 0.0f && normal.y == -1.0f && normal.z == 0.0f);

  SimBackgroundVoxelModelFace top = {
    .points = {
      {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
      {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f},
    },
  };
  CHECK(SimBackgroundVoxelSurface_OutwardNormal(&top, &normal));
  CHECK(normal.x == 0.0f && normal.y == 0.0f && normal.z == 1.0f);

  /* Steep casing uses roof winding, not the inward vertical-wall convention.
   * Its upward normal must still receive overhead light (nz is below 0.5). */
  SimBackgroundVoxelModelFace slope = {
    .points = {{1, 1, 0}, {-1, 1, 0}, {0, 0, 2}, {0, 0, 2}},
    .material = kSimVoxelMaterial_WallLight, .brightness = 255,
    .occlusion = {255, 255, 255, 255},
  };
  CHECK(SimBackgroundVoxelSurface_OutwardNormal(&slope, &normal));
  CHECK(normal.y > 0.0f && normal.z > 0.0f && normal.z < 0.5f);
  CHECK(SimBackgroundVoxelLighting_FaceBrightness(
      &slope, kSimBackgroundVoxelShading_MaterialAware, 0, 90) >
        SimBackgroundVoxelLighting_FaceBrightness(
      &wall, kSimBackgroundVoxelShading_MaterialAware, 0, 90));

  SimBackgroundVoxelModelFace degenerate = {0};
  CHECK(!SimBackgroundVoxelSurface_OutwardNormal(&degenerate, &normal));

  /* Real palm surfaces retain downward normals at every LOD. Previously the
   * roof convention flipped every underside up and lit both sides from above.
   * Keep the steep legacy roof regression above alongside this opt-in path. */
  SimBackgroundVoxelObject palm_object = {
    .kind = kSimBackgroundVoxel_Palm, .town = 5, .cell_x = 4, .cell_y = 7,
  };
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel palm;
    SimBackgroundVoxelModel_Build(&palm_object, detail, &palm);
    int undersides = 0;
    for (int i = 0; i < palm.face_count; i++) {
      const SimBackgroundVoxelModelFace *face = &palm.faces[i];
      if (face->material != kSimVoxelMaterial_LeavesDark) continue;
      undersides++;
      CHECK(face->outward_winding);
      CHECK(SimBackgroundVoxelSurface_OutwardNormal(face, &normal));
      CHECK(normal.z < 0.0f);
      CHECK(SimBackgroundVoxelLighting_FaceBrightness(
          face, kSimBackgroundVoxelShading_MaterialAware, 0, 90) < 200);
      /* Reversing the light vertically must now brighten that underside. */
      SimBackgroundVoxelLightDirection below = {0, 0, -1};
      CHECK(SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
          face, kSimBackgroundVoxelShading_MaterialAware, &below) >
          SimBackgroundVoxelLighting_FaceBrightness(
          face, kSimBackgroundVoxelShading_MaterialAware, 0, 90));
    }
    CHECK(undersides >= 8);
  }

  /* A closed bush has a downward lower hemisphere, not an upward roof.
   * Test geometric orientation as well as its overhead lighting response. */
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_Shrub, .town = 1};
    SimBackgroundVoxelModel shrub;
    SimBackgroundVoxelModel_Build(&object, detail, &shrub);
    int lower = 0;
    for (int i = 0; i < shrub.face_count; i++) {
      const SimBackgroundVoxelModelFace *face = &shrub.faces[i];
      if (face->material == kSimVoxelMaterial_Trunk) continue;
      CHECK(face->outward_winding);
      CHECK(SimBackgroundVoxelSurface_OutwardNormal(face, &normal));
      if (normal.z >= -.05f) continue;
      lower++;
      CHECK(SimBackgroundVoxelLighting_FaceBrightness(
          face, kSimBackgroundVoxelShading_MaterialAware, 0, 90) < 200);
    }
    CHECK(lower >= 8);
  }

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
