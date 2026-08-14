#include "sim/sim_background_voxel_models.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static int MaterialFaces(const SimBackgroundVoxelModel *model,
                         SimBackgroundVoxelMaterial material) {
  int count = 0;
  for (uint16_t i = 0; i < model->face_count; i++)
    if (model->faces[i].material == material) count++;
  return count;
}

static bool HorizontalFaceCovers(const SimBackgroundVoxelModel *model,
                                 float x, float y) {
  for (uint16_t i = 0; i < model->face_count; i++) {
    const SimBackgroundVoxelModelFace *face = &model->faces[i];
    float min_x = face->points[0].x, max_x = face->points[0].x;
    float min_y = face->points[0].y, max_y = face->points[0].y;
    bool horizontal = true;
    for (int point = 1; point < 4; point++) {
      if (face->points[point].z != face->points[0].z) horizontal = false;
      if (face->points[point].x < min_x) min_x = face->points[point].x;
      if (face->points[point].x > max_x) max_x = face->points[point].x;
      if (face->points[point].y < min_y) min_y = face->points[point].y;
      if (face->points[point].y > max_y) max_y = face->points[point].y;
    }
    if (horizontal && x > min_x && x < max_x && y > min_y && y < max_y)
      return true;
  }
  return false;
}

static SimBackgroundVoxelModel Build(SimBackgroundVoxelKind kind,
                                     SimBackgroundVoxelDetail detail) {
  SimBackgroundVoxelObject object = {
    .kind = kind,
    .record_slot = 2,
  };
  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModel_Build(&object, detail, &model);
  CHECK(!model.overflow);
  CHECK(model.face_count > 0);
  CHECK(model.face_count <= SimBackgroundVoxelModel_FaceBudget(detail));
  return model;
}

int main(void) {
  SimBackgroundVoxelModel house = Build(
      kSimBackgroundVoxel_House, kSimBackgroundVoxelDetail_Balanced);
  CHECK(house.min_x >= 0.0f && house.max_x <= 16.0f);
  CHECK(house.min_y >= 0.0f && house.max_y <= 16.0f);
  CHECK(house.max_z == 14.0f);
  CHECK(MaterialFaces(&house, kSimVoxelMaterial_Roof) > 0);
  CHECK(MaterialFaces(&house, kSimVoxelMaterial_Dark) > 0);

  SimBackgroundVoxelObject alternate_house_object = {
    .kind = kSimBackgroundVoxel_House,
    .flags = kSimBackgroundVoxel_AlternateFacing,
    .record_slot = 2,
  };
  SimBackgroundVoxelModel alternate_house;
  SimBackgroundVoxelModel_Build(
      &alternate_house_object, kSimBackgroundVoxelDetail_Balanced,
      &alternate_house);
  CHECK(!alternate_house.overflow);
  CHECK(alternate_house.face_count > house.face_count);
  CHECK(alternate_house.min_x >= 0.0f && alternate_house.max_x <= 16.0f);
  CHECK(alternate_house.min_y >= 0.0f && alternate_house.max_y <= 16.0f);
  CHECK(memcmp(alternate_house.faces, house.faces,
               house.face_count * sizeof(house.faces[0])) != 0);

  SimBackgroundVoxelModel cathedral = Build(
      kSimBackgroundVoxel_Cathedral, kSimBackgroundVoxelDetail_Balanced);
  CHECK(cathedral.min_x >= 0.0f && cathedral.max_x <= 32.0f);
  CHECK(cathedral.min_y >= 0.0f && cathedral.max_y <= 32.0f);
  CHECK(cathedral.max_z == 24.0f);
  CHECK(cathedral.min_y >= 8.0f);  /* protected rear land stays visually open */
  CHECK(MaterialFaces(&cathedral, kSimVoxelMaterial_WallLight) >= 25);

  SimBackgroundVoxelModel windmill = Build(
      kSimBackgroundVoxel_Windmill, kSimBackgroundVoxelDetail_Balanced);
  CHECK(windmill.min_x >= 0.0f && windmill.max_x <= 32.0f);
  CHECK(windmill.min_y >= 0.0f && windmill.max_y <= 16.0f);
  CHECK(windmill.max_z <= 32.0f);
  CHECK(MaterialFaces(&windmill, kSimVoxelMaterial_Blade) == 20);

  SimBackgroundVoxelModel factory = Build(
      kSimBackgroundVoxel_Factory, kSimBackgroundVoxelDetail_Balanced);
  CHECK(factory.min_x >= 0.0f && factory.max_x <= 32.0f);
  CHECK(factory.min_y >= 0.0f && factory.max_y <= 32.0f);
  CHECK(factory.max_z == 17.0f);  /* low body plus sparse chimneys */
  CHECK(MaterialFaces(&factory, kSimVoxelMaterial_Roof) > 0);
  CHECK(MaterialFaces(&factory, kSimVoxelMaterial_Metal) > 0);
  CHECK(HorizontalFaceCovers(&factory, 10.0f, 6.0f));   /* upper U arm */
  CHECK(HorizontalFaceCovers(&factory, 26.0f, 16.0f));  /* right spine */
  CHECK(HorizontalFaceCovers(&factory, 10.0f, 26.0f));  /* lower U arm */
  CHECK(!HorizontalFaceCovers(&factory, 10.0f, 16.0f)); /* open courtyard */

  SimBackgroundVoxelObject isolated_object = {
    .kind = kSimBackgroundVoxel_Tree,
    .flags = kSimBackgroundVoxel_IsolatedTree,
    .record_slot = 0xFF,
  };
  SimBackgroundVoxelModel isolated;
  SimBackgroundVoxelModel_Build(
      &isolated_object, kSimBackgroundVoxelDetail_Balanced, &isolated);
  CHECK(!isolated.overflow && isolated.max_z == 15.0f);
  CHECK(isolated.min_x >= 0.0f && isolated.max_x <= 16.0f);
  CHECK(isolated.min_y >= 0.0f && isolated.max_y <= 16.0f);

  SimBackgroundVoxelObject interior_object = {
    .kind = kSimBackgroundVoxel_Tree,
    .tree_edges = kSimBackgroundTreeEdge_North |
        kSimBackgroundTreeEdge_East | kSimBackgroundTreeEdge_South |
        kSimBackgroundTreeEdge_West,
    .record_slot = 0xFF,
  };
  SimBackgroundVoxelModel interior;
  SimBackgroundVoxelModel_Build(
      &interior_object, kSimBackgroundVoxelDetail_Balanced, &interior);
  CHECK(!interior.overflow && interior.max_z == 15.0f);
  CHECK(interior.face_count > isolated.face_count);
  /* Even with all four neighbour bridges, a balanced dense-forest cell keeps
   * at least one quarter of its per-object face budget in reserve. */
  CHECK(interior.face_count <= SimBackgroundVoxelModel_FaceBudget(
      kSimBackgroundVoxelDetail_Balanced) * 3 / 4);

  SimBackgroundVoxelObject construction_object = {
    .kind = kSimBackgroundVoxel_House,
    .flags = kSimBackgroundVoxel_UnderConstruction,
  };
  SimBackgroundVoxelModel construction;
  SimBackgroundVoxelModel_Build(
      &construction_object, kSimBackgroundVoxelDetail_Balanced,
      &construction);
  CHECK(!construction.overflow);
  CHECK(construction.face_count < house.face_count);
  CHECK(MaterialFaces(&construction, kSimVoxelMaterial_Wood) > 0);

  /* Every family respects each performance target, and richer targets are
   * genuinely richer rather than four labels selecting the same geometry. */
  for (int kind = kSimBackgroundVoxel_House;
       kind <= kSimBackgroundVoxel_Tree; kind++) {
    SimBackgroundVoxelModel low = Build(
        (SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_Low);
    SimBackgroundVoxelModel balanced = Build(
        (SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_Balanced);
    SimBackgroundVoxelModel high = Build(
        (SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_High);
    SimBackgroundVoxelModel ultra = Build(
        (SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_Ultra);
    CHECK(low.face_count <= balanced.face_count);
    CHECK(balanced.face_count <= high.face_count);
    CHECK(high.face_count <= ultra.face_count);
    CHECK(low.face_count < ultra.face_count);
  }

  if (failures) {
    fprintf(stderr, "%d sim background voxel model checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel model checks passed");
  return 0;
}
