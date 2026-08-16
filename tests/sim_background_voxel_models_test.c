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

static void MaterialXBounds(const SimBackgroundVoxelModel *model,
                            SimBackgroundVoxelMaterial material,
                            float *min_x, float *max_x) {
  *min_x = 1000000.0f;
  *max_x = -1000000.0f;
  for (uint16_t face = 0; face < model->face_count; face++) {
    if (model->faces[face].material != material) continue;
    for (int point = 0; point < 4; point++) {
      float x = model->faces[face].points[point].x;
      if (x < *min_x) *min_x = x;
      if (x > *max_x) *max_x = x;
    }
  }
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

static float RegionMaxZ(const SimBackgroundVoxelModel *model,
                        float x0, float y0, float x1, float y1) {
  float max_z = 0.0f;
  for (uint16_t face = 0; face < model->face_count; face++)
    for (int point = 0; point < 4; point++) {
      const SimBackgroundVoxelModelPoint *p =
          &model->faces[face].points[point];
      if (p->x >= x0 && p->x <= x1 && p->y >= y0 && p->y <= y1 &&
          p->z > max_z)
        max_z = p->z;
    }
  return max_z;
}

static uint64_t ModelHash(const SimBackgroundVoxelModel *model) {
  const uint8_t *bytes = (const uint8_t *)model->faces;
  size_t byte_count =
      model->face_count * sizeof(model->faces[0]);
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < byte_count; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  hash ^= model->face_count;
  return hash;
}

static int UniqueVariedModels(SimBackgroundVoxelKind kind,
                              SimBackgroundVoxelDetail detail) {
  uint64_t hashes[8] = {0};
  int unique = 0;
  for (int seed = 0; seed < 64; seed++) {
    SimBackgroundVoxelObject object = {
      .kind = kind,
      .cell_x = seed & 7,
      .cell_y = seed >> 3,
      .record_slot = (uint8_t)(seed % 7),
      .group = (uint8_t)(seed % 3),
    };
    SimBackgroundVoxelModel model;
    SimBackgroundVoxelModel_BuildStyled(
        &object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(!model.overflow);
    CHECK(model.face_count <= SimBackgroundVoxelModel_FaceBudget(detail));
    uint64_t hash = ModelHash(&model);
    bool known = false;
    for (int i = 0; i < unique; i++) known |= hashes[i] == hash;
    if (!known && unique < (int)(sizeof(hashes) / sizeof(hashes[0])))
      hashes[unique++] = hash;
  }
  return unique;
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

static SimBackgroundVoxelModel BuildRegionalHouse(uint8_t town,
                                                   uint8_t level) {
  SimBackgroundVoxelObject object = {
    .kind = kSimBackgroundVoxel_House,
    .town = town,
    .development_level = level,
    .record_slot = 1,
  };
  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModel_BuildStyled(
      &object, kSimBackgroundVoxelDetail_Balanced,
      kSimBackgroundVoxelStyle_Basic, &model);
  CHECK(!model.overflow && model.face_count > 0);
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

  /* The ROM selects eight architectural families through its exact 6x3
   * town/civilization table. Each town's three-stage progression must remain
   * visually distinct even where another town deliberately shares a family. */
  uint64_t progression_hash[6][3];
  for (int town = 1; town <= 6; town++)
    for (int level = 0; level < 3; level++) {
      SimBackgroundVoxelModel regional = BuildRegionalHouse(town, level);
      progression_hash[town - 1][level] = ModelHash(&regional);
    }
  for (int town = 0; town < 6; town++) {
    CHECK(progression_hash[town][0] != progression_hash[town][1]);
    CHECK(progression_hash[town][1] != progression_hash[town][2]);
    CHECK(progression_hash[town][0] != progression_hash[town][2]);
  }
  /* Tent and timber reuse in the source game is intentional, not a missing
   * regional override. */
  CHECK(progression_hash[0][0] == progression_hash[5][0]);
  CHECK(progression_hash[0][1] == progression_hash[1][1]);
  CHECK(progression_hash[0][1] == progression_hash[3][1]);
  CHECK(progression_hash[0][1] == progression_hash[5][1]);
  /* Kasandora's explicitly requested progression grows from canvas to a low
   * stone dwelling and then a taller developed stone house. */
  SimBackgroundVoxelModel kasandora_tent = BuildRegionalHouse(3, 0);
  SimBackgroundVoxelModel kasandora_early = BuildRegionalHouse(3, 1);
  SimBackgroundVoxelModel kasandora_developed = BuildRegionalHouse(3, 2);
  CHECK(kasandora_tent.max_z < kasandora_early.max_z);
  CHECK(kasandora_early.max_z < kasandora_developed.max_z);

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

  /* Seed 1 selects the porch. On alternate-facing houses its posts must be
   * transformed with the main facade instead of staying at the standard-house
   * coordinates and landing to the left of the door. */
  SimBackgroundVoxelObject alternate_porch_object = {
    .kind = kSimBackgroundVoxel_House,
    .flags = kSimBackgroundVoxel_AlternateFacing,
    .record_slot = 1,
  };
  SimBackgroundVoxelModel alternate_porch;
  SimBackgroundVoxelModel_BuildStyled(
      &alternate_porch_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &alternate_porch);
  float porch_min_x, porch_max_x;
  MaterialXBounds(&alternate_porch, kSimVoxelMaterial_Wood,
                  &porch_min_x, &porch_max_x);
  CHECK(porch_min_x > 8.7f && porch_max_x < 12.0f);

  SimBackgroundVoxelModel cathedral = Build(
      kSimBackgroundVoxel_Cathedral, kSimBackgroundVoxelDetail_Balanced);
  CHECK(cathedral.min_x >= 0.0f && cathedral.max_x <= 32.0f);
  CHECK(cathedral.min_y >= 0.0f && cathedral.max_y <= 32.0f);
  CHECK(cathedral.max_z == 24.0f);
  CHECK(cathedral.min_y >= 8.0f);  /* protected rear land stays visually open */
  /* Surface compilation removes column caps buried in the facade/base. */
  CHECK(MaterialFaces(&cathedral, kSimVoxelMaterial_WallLight) >= 15);

  SimBackgroundVoxelObject cathedral_object = {
    .kind = kSimBackgroundVoxel_Cathedral,
    .record_slot = 2,
  };
  SimBackgroundVoxelModel decorated_cathedral;
  SimBackgroundVoxelModel_BuildStyled(
      &cathedral_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Trim, &decorated_cathedral);
  CHECK(!decorated_cathedral.overflow);
  CHECK(MaterialFaces(&decorated_cathedral, kSimVoxelMaterial_Gold) > 0);
  CHECK(MaterialFaces(&decorated_cathedral, kSimVoxelMaterial_Glass) > 0);
  CHECK(RegionMaxZ(&decorated_cathedral, 2.0f, 24.0f, 9.0f, 31.5f)
        <= 16.4f);
  CHECK(RegionMaxZ(&decorated_cathedral, 23.0f, 24.0f, 30.0f, 31.5f)
        <= 16.4f);

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
  CHECK(RegionMaxZ(&factory, 1.0f, 1.0f, 20.0f, 8.5f) ==
        RegionMaxZ(&factory, 1.0f, 23.5f, 20.0f, 31.0f));

  /* Styling is a separate cost boundary from the density target. The factory
   * yard appears only in Architectural+, and Varied is deterministic for the
   * same object identity. */
  SimBackgroundVoxelObject styled_factory_object = {
    .kind = kSimBackgroundVoxel_Factory,
    .cell_x = 7,
    .cell_y = 11,
    .record_slot = 2,
  };
  SimBackgroundVoxelModel trimmed_factory, architectural_factory;
  SimBackgroundVoxelModel varied_factory;
  SimBackgroundVoxelModel repeated_varied_factory;
  SimBackgroundVoxelModel_BuildStyled(
      &styled_factory_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Trim, &trimmed_factory);
  SimBackgroundVoxelModel_BuildStyled(
      &styled_factory_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Architectural, &architectural_factory);
  SimBackgroundVoxelModel_BuildStyled(
      &styled_factory_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &varied_factory);
  SimBackgroundVoxelModel_BuildStyled(
      &styled_factory_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &repeated_varied_factory);
  CHECK(!architectural_factory.overflow && !varied_factory.overflow);
  CHECK(architectural_factory.authored_face_count >
        architectural_factory.face_count);
  CHECK(architectural_factory.box_count > 0);
  /* The courtyard must expose the underlying biome tile at every style level;
   * architectural detail may add fixtures, but never a replacement floor. */
  CHECK(MaterialFaces(&trimmed_factory, kSimVoxelMaterial_Paving) == 0);
  CHECK(MaterialFaces(&architectural_factory, kSimVoxelMaterial_Paving) == 0);
  CHECK(architectural_factory.face_count > trimmed_factory.face_count);
  CHECK(varied_factory.face_count > architectural_factory.face_count);
  CHECK(varied_factory.face_count == repeated_varied_factory.face_count);
  CHECK(memcmp(varied_factory.faces, repeated_varied_factory.faces,
               varied_factory.face_count * sizeof(varied_factory.faces[0]))
        == 0);
  int occluded_vertices = 0;
  for (uint16_t face = 0; face < varied_factory.face_count; face++)
    for (int point = 0; point < 4; point++)
      if (varied_factory.faces[face].occlusion[point] < 255)
        occluded_vertices++;
  CHECK(occluded_vertices > 0);

  SimBackgroundVoxelModel low_basic, low_varied;
  SimBackgroundVoxelModel_BuildStyled(
      &styled_factory_object, kSimBackgroundVoxelDetail_Low,
      kSimBackgroundVoxelStyle_Basic, &low_basic);
  SimBackgroundVoxelModel_BuildStyled(
      &styled_factory_object, kSimBackgroundVoxelDetail_Low,
      kSimBackgroundVoxelStyle_Varied, &low_varied);
  CHECK(low_basic.face_count == low_varied.face_count);

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
  /* Adjacency affects extraction/grouping, never the authored crown. Forest
   * interiors must not acquire rectangular connector bars on their sides. */
  CHECK(interior.face_count == isolated.face_count);
  CHECK(memcmp(interior.faces, isolated.faces,
               interior.face_count * sizeof(interior.faces[0])) == 0);
  /* A balanced dense-forest cell keeps at least one quarter of its per-object
   * face budget in reserve. */
  CHECK(interior.face_count <= SimBackgroundVoxelModel_FaceBudget(
      kSimBackgroundVoxelDetail_Balanced) * 3 / 4);

  SimBackgroundVoxelObject snow_tree_object = isolated_object;
  snow_tree_object.town = 6;
  SimBackgroundVoxelModel snow_tree;
  SimBackgroundVoxelModel_Build(
      &snow_tree_object, kSimBackgroundVoxelDetail_Balanced, &snow_tree);
  CHECK(!snow_tree.overflow && snow_tree.max_z == 16.0f);
  CHECK(ModelHash(&snow_tree) != ModelHash(&isolated));

  SimBackgroundVoxelObject palm_object = {
    .kind = kSimBackgroundVoxel_Palm,
    .town = 5,
    .cell_x = 4,
    .cell_y = 7,
    .group = 2,
    .record_slot = 0xFF,
  };
  SimBackgroundVoxelModel palm;
  SimBackgroundVoxelModel_Build(
      &palm_object, kSimBackgroundVoxelDetail_Balanced, &palm);
  CHECK(!palm.overflow && palm.face_count > 0);
  CHECK(palm.min_x >= 0.0f && palm.max_x <= 16.0f);
  CHECK(palm.min_y >= 0.0f && palm.max_y <= 16.0f);
  CHECK(MaterialFaces(&palm, kSimVoxelMaterial_Trunk) > 0);
  CHECK(MaterialFaces(&palm, kSimVoxelMaterial_Leaves) > 0);
  CHECK(ModelHash(&palm) != ModelHash(&isolated));

  SimBackgroundVoxelObject construction_object = {
    .kind = kSimBackgroundVoxel_House,
    .flags = kSimBackgroundVoxel_UnderConstruction,
  };
  SimBackgroundVoxelModel construction;
  SimBackgroundVoxelModel_Build(
      &construction_object, kSimBackgroundVoxelDetail_Balanced,
      &construction);
  CHECK(!construction.overflow);
  /* Clean roof surfaces use fewer faces than the deliberately skeletal frame;
   * construction identity is material-based, not a face-count heuristic. */
  CHECK(construction.face_count != house.face_count);
  CHECK(MaterialFaces(&construction, kSimVoxelMaterial_Wood) > 0);

  /* Every family respects each performance target, and richer targets are
   * genuinely richer rather than four labels selecting the same geometry. */
  for (int kind = kSimBackgroundVoxel_House;
       kind < kSimBackgroundVoxelKindCount; kind++) {
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

  /* Every density/style boundary is a real hard budget. This exercises more
   * than the representative fixtures above so a newly authored variant cannot
   * overflow only for one deterministic town coordinate. */
  for (int kind = kSimBackgroundVoxel_House;
       kind < kSimBackgroundVoxelKindCount; kind++) {
    for (int detail = kSimBackgroundVoxelDetail_Low;
         detail < kSimBackgroundVoxelDetail_Count; detail++) {
      for (int style = kSimBackgroundVoxelStyle_Basic;
           style < kSimBackgroundVoxelStyle_Count; style++) {
        for (int seed = 0; seed < 4; seed++) {
          SimBackgroundVoxelObject object = {
            .kind = (uint8_t)kind,
            .cell_x = (uint8_t)seed,
            .cell_y = (uint8_t)(seed * 3),
            .record_slot = (uint8_t)(seed + 1),
            .group = (uint8_t)(seed & 1),
          };
          SimBackgroundVoxelModel model;
          SimBackgroundVoxelModel_BuildStyled(
              &object, (SimBackgroundVoxelDetail)detail,
              (SimBackgroundVoxelStyle)style, &model);
          CHECK(!model.overflow);
          CHECK(model.face_count <=
                SimBackgroundVoxelModel_FaceBudget(
                    (SimBackgroundVoxelDetail)detail));
        }
      }
    }
  }

  /* Regional families must honor the same configurable quality boundaries as
   * the original Fillmore model. Exercise all 18 ROM-selected identities at
   * every density/style combination rather than validating only one town. */
  for (int town = 1; town <= 6; town++)
    for (int level = 0; level < 3; level++)
      for (int detail = kSimBackgroundVoxelDetail_Low;
           detail < kSimBackgroundVoxelDetail_Count; detail++)
        for (int style = kSimBackgroundVoxelStyle_Basic;
             style < kSimBackgroundVoxelStyle_Count; style++) {
          SimBackgroundVoxelObject object = {
            .kind = kSimBackgroundVoxel_House,
            .town = (uint8_t)town,
            .development_level = (uint8_t)level,
            .cell_x = (uint8_t)(town * 3),
            .cell_y = (uint8_t)(level * 5),
            .record_slot = (uint8_t)(town * 3 + level),
          };
          SimBackgroundVoxelModel regional;
          SimBackgroundVoxelModel_BuildStyled(
              &object, (SimBackgroundVoxelDetail)detail,
              (SimBackgroundVoxelStyle)style, &regional);
          CHECK(!regional.overflow && regional.face_count > 0);
          CHECK(regional.face_count <=
                SimBackgroundVoxelModel_FaceBudget(
                    (SimBackgroundVoxelDetail)detail));
        }

  CHECK(UniqueVariedModels(kSimBackgroundVoxel_House,
                           kSimBackgroundVoxelDetail_High) >= 4);
  CHECK(UniqueVariedModels(kSimBackgroundVoxel_House,
                           kSimBackgroundVoxelDetail_Ultra) >= 6);
  CHECK(UniqueVariedModels(kSimBackgroundVoxel_Factory,
                           kSimBackgroundVoxelDetail_High) >= 3);
  CHECK(UniqueVariedModels(kSimBackgroundVoxel_Tree,
                           kSimBackgroundVoxelDetail_High) >= 4);

  if (failures) {
    fprintf(stderr, "%d sim background voxel model checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel model checks passed");
  return 0;
}
