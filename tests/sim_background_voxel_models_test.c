#include "sim/sim_background_voxel_models.h"
#include "sim/sim_background_bridge.h"
#include "sim/sim_background_voxel_region.h"

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

static void MaterialZBounds(const SimBackgroundVoxelModel *model,
                            SimBackgroundVoxelMaterial material,
                            float *min_z, float *max_z) {
  *min_z = 1000000.0f;
  *max_z = -1000000.0f;
  for (uint16_t face = 0; face < model->face_count; face++) {
    if (model->faces[face].material != material) continue;
    for (int point = 0; point < 4; point++) {
      float z = model->faces[face].points[point].z;
      if (z < *min_z) *min_z = z;
      if (z > *max_z) *max_z = z;
    }
  }
}

/* Widest X extent of any geometry within half a pixel of the given height. */
static float TopWidthAt(const SimBackgroundVoxelModel *model, float z) {
  float min_x = 1000000.0f, max_x = -1000000.0f;
  for (uint16_t face = 0; face < model->face_count; face++)
    for (int point = 0; point < 4; point++) {
      const SimBackgroundVoxelModelPoint *at =
          &model->faces[face].points[point];
      if (at->z < z - 0.5f || at->z > z + 0.5f) continue;
      if (at->x < min_x) min_x = at->x;
      if (at->x > max_x) max_x = at->x;
    }
  return max_x > min_x ? max_x - min_x : 0.0f;
}

static bool MaterialHasSlopedFace(
    const SimBackgroundVoxelModel *model,
    SimBackgroundVoxelMaterial material) {
  for (uint16_t face = 0; face < model->face_count; face++) {
    if (model->faces[face].material != material) continue;
    float min_x = model->faces[face].points[0].x;
    float max_x = min_x;
    float min_y = model->faces[face].points[0].y;
    float max_y = min_y;
    float min_z = model->faces[face].points[0].z;
    float max_z = min_z;
    for (int point = 1; point < 4; point++) {
      const SimBackgroundVoxelModelPoint *at =
          &model->faces[face].points[point];
      if (at->x < min_x) min_x = at->x;
      if (at->x > max_x) max_x = at->x;
      if (at->y < min_y) min_y = at->y;
      if (at->y > max_y) max_y = at->y;
      if (at->z < min_z) min_z = at->z;
      if (at->z > max_z) max_z = at->z;
    }
    if (max_x - min_x > 0.01f && max_y - min_y > 0.01f &&
        max_z - min_z > 0.01f)
      return true;
  }
  return false;
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

static bool SameBounds(const SimBackgroundVoxelModel *a,
                       const SimBackgroundVoxelModel *b) {
  return a->min_x == b->min_x && a->min_y == b->min_y &&
      a->min_z == b->min_z && a->max_x == b->max_x &&
      a->max_y == b->max_y && a->max_z == b->max_z;
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
  if (kind == kSimBackgroundVoxel_Bridge) {
    object.bridge_axis = kSimBackgroundBridgeAxis_EastWest;
    object.bridge_bank_a_x = 0;
    object.bridge_bank_b_x = 2;
  }
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
  uint64_t progression_hash[kSimBackgroundTownCount]
      [kSimBackgroundDevelopmentLevelCount];
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0;
         level < kSimBackgroundDevelopmentLevelCount; level++) {
      SimBackgroundVoxelModel regional = BuildRegionalHouse(town, level);
      progression_hash[town - 1][level] = ModelHash(&regional);
    }
  for (int town = 0; town < kSimBackgroundTownCount; town++) {
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
  /* Kasandora's canonical progression remains readable by silhouette: a low
   * round yurt, a taller white tent, then a flat-roofed adobe dwelling. */
  SimBackgroundVoxelModel kasandora_yurt = BuildRegionalHouse(3, 0);
  SimBackgroundVoxelModel kasandora_tent = BuildRegionalHouse(3, 1);
  SimBackgroundVoxelModel kasandora_adobe = BuildRegionalHouse(3, 2);
  CHECK(kasandora_yurt.max_z < kasandora_tent.max_z);
  CHECK(kasandora_tent.max_z < kasandora_adobe.max_z);
  CHECK(kasandora_tent.max_z == 10.0f);
  CHECK(TopWidthAt(&kasandora_tent, 3.2f) >= 14.0f);
  CHECK(progression_hash[2][1] != progression_hash[0][0]);

  /* Aitos' developed stone house is a flat-roofed masonry terrace. Its roof
   * must cover the centre at one level and remain far below a gable peak. */
  SimBackgroundVoxelModel aitos_stone = BuildRegionalHouse(4, 2);
  CHECK(HorizontalFaceCovers(&aitos_stone, 8.0f, 8.0f));
  CHECK(aitos_stone.max_z <= 10.5f);
  CHECK(TopWidthAt(&aitos_stone, 9.5f) >= 14.0f);
  CHECK(!MaterialHasSlopedFace(&aitos_stone, kSimVoxelMaterial_Roof));
  CHECK(!MaterialHasSlopedFace(&aitos_stone, kSimVoxelMaterial_RoofLight));
  SimBackgroundVoxelObject aitos_house = {
    .kind = kSimBackgroundVoxel_House,
    .town = 4,
    .development_level = 2,
  };
  for (int detail = kSimBackgroundVoxelDetail_Low;
       detail < kSimBackgroundVoxelDetail_Count; detail++) {
    for (int style = kSimBackgroundVoxelStyle_Basic;
         style < kSimBackgroundVoxelStyle_Count; style++) {
      SimBackgroundVoxelModel styled_aitos;
      SimBackgroundVoxelModel_BuildStyled(
          &aitos_house, (SimBackgroundVoxelDetail)detail,
          (SimBackgroundVoxelStyle)style, &styled_aitos);
      CHECK(HorizontalFaceCovers(&styled_aitos, 8.0f, 8.0f));
      CHECK(!MaterialHasSlopedFace(
          &styled_aitos, kSimVoxelMaterial_Roof));
      CHECK(!MaterialHasSlopedFace(
          &styled_aitos, kSimVoxelMaterial_RoofLight));
    }
  }

  /* Marahna deliberately shares only the yurt, then branches into its raised
   * tropical hut and a lower, solid log cabin. */
  CHECK(progression_hash[2][0] == progression_hash[4][0]);
  CHECK(progression_hash[2][1] != progression_hash[4][1]);
  CHECK(progression_hash[2][2] != progression_hash[4][2]);

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

  /* Optional seeded facade detail is transformed with an alternate-facing
   * house, but it cannot enlarge that house or alter its roofline. */
  SimBackgroundVoxelObject alternate_varied_object = {
    .kind = kSimBackgroundVoxel_House,
    .flags = kSimBackgroundVoxel_AlternateFacing,
    .record_slot = 1,
  };
  SimBackgroundVoxelModel alternate_architectural, alternate_varied;
  SimBackgroundVoxelModel_BuildStyled(
      &alternate_varied_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Architectural, &alternate_architectural);
  SimBackgroundVoxelModel_BuildStyled(
      &alternate_varied_object, kSimBackgroundVoxelDetail_High,
      kSimBackgroundVoxelStyle_Varied, &alternate_varied);
  CHECK(SameBounds(&alternate_architectural, &alternate_varied));
  CHECK(ModelHash(&alternate_architectural) != ModelHash(&alternate_varied));

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
  CHECK(windmill.min_y >= 0.0f && windmill.max_y <= 16.8f);
  CHECK(windmill.max_z <= 32.0f);
  CHECK(MaterialFaces(&windmill, kSimVoxelMaterial_Blade) == 20);

  /* Out where the blades sweep, nothing but the rotor may stand in the rotor's
   * front face. The hub cap is the one surface meant to cover the blades and
   * it stays within 3.0 of the hub; between there and the tips, any face at
   * the blade front can only take pixels away from a blade.
   *
   * That is the invariant two earlier rounds of moving the mill's frame back
   * never tested, because the surface eating the blades was the rotor's OWN
   * spar, drawn in Wood. At 0.56 model units it measures under one screen
   * pixel everywhere it is drawn, and a sub-pixel quad claims whole pixels
   * rather than thinning out -- in the mill's brown it read as the frame
   * showing through a severed blade. The spar is in the blade's own ramp now,
   * so it is exempt here by material, exactly as the blade faces are. */
  const float rotor_x = 16.0f, rotor_z = 21.0f;
  const float rotor_back = 15.8f;      /* kWindmillBladePlane */
  const float hub_cap_radius = 3.0f;
  const float tip_radius = 11.2f;      /* outer 10.0 plus the half width */
  for (int detail = kSimBackgroundVoxelDetail_Low;
       detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelObject spinning = {
      .kind = kSimBackgroundVoxel_Windmill,
      .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 1,
    };
    for (int style = kSimBackgroundVoxelStyle_Basic;
         style < kSimBackgroundVoxelStyle_Count; style++) {
      for (int phase = 0; phase < 3; phase++) {
        for (int slot = 0; slot < 2; slot++) {
          spinning.animation_phase = (uint8_t)phase;
          spinning.record_slot = (uint8_t)slot;
          SimBackgroundVoxelModel turning;
          SimBackgroundVoxelModel_BuildStyled(
              &spinning, (SimBackgroundVoxelDetail)detail,
              (SimBackgroundVoxelStyle)style, &turning);
          for (uint16_t face = 0; face < turning.face_count; face++) {
            if (turning.faces[face].material == kSimVoxelMaterial_Blade)
              continue;
            for (int point = 0; point < 4; point++) {
              const SimBackgroundVoxelModelPoint *at =
                  &turning.faces[face].points[point];
              float dx = at->x - rotor_x, dz = at->z - rotor_z;
              float radius_sq = dx * dx + dz * dz;
              if (radius_sq <= hub_cap_radius * hub_cap_radius) continue;
              if (radius_sq > tip_radius * tip_radius) continue;
              CHECK(at->y < rotor_back);
            }
          }
        }
      }
    }
  }

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
    .record_slot = kSimBackgroundVoxelNoRecordSlot,
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
    .record_slot = kSimBackgroundVoxelNoRecordSlot,
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
    .record_slot = kSimBackgroundVoxelNoRecordSlot,
  };
  SimBackgroundVoxelModel palm;
  SimBackgroundVoxelModel_Build(
      &palm_object, kSimBackgroundVoxelDetail_Balanced, &palm);
  CHECK(!palm.overflow && palm.face_count > 0);
  CHECK(palm.min_x >= 0.0f && palm.max_x <= 16.0f);
  CHECK(palm.min_y >= 0.0f && palm.max_y <= 16.0f);
  CHECK(MaterialFaces(&palm, kSimVoxelMaterial_Trunk) > 0);
  CHECK(MaterialFaces(&palm, kSimVoxelMaterial_Leaves) > 0);
  CHECK(MaterialHasSlopedFace(&palm, kSimVoxelMaterial_Leaves));
  CHECK(ModelHash(&palm) != ModelHash(&isolated));

  SimBackgroundVoxelObject shrub_object = {
    .kind = kSimBackgroundVoxel_Shrub,
    .flags = kSimBackgroundVoxel_IsolatedTree,
    .cell_x = 5,
    .cell_y = 9,
    .record_slot = kSimBackgroundVoxelNoRecordSlot,
  };
  SimBackgroundVoxelModel shrub;
  SimBackgroundVoxelModel_Build(
      &shrub_object, kSimBackgroundVoxelDetail_Balanced, &shrub);
  CHECK(!shrub.overflow && shrub.face_count > 0);
  CHECK(shrub.min_x >= 0.0f && shrub.max_x <= 16.0f);
  CHECK(shrub.min_y >= 0.0f && shrub.max_y <= 16.0f);
  /* The clearable bush is shorter and rounder than the permanent evergreen it
   * used to be drawn as, and shares no geometry with it. */
  CHECK(shrub.max_z < isolated.max_z);
  CHECK(ModelHash(&shrub) != ModelHash(&isolated));
  CHECK(MaterialFaces(&shrub, kSimVoxelMaterial_Leaves) > 0);

  /* Every landmark lives inside its own 2x2 plot: 32x32 town pixels. */
  SimBackgroundVoxelModel story_tree = Build(
      kSimBackgroundVoxel_StoryTree, kSimBackgroundVoxelDetail_Balanced);
  CHECK(story_tree.min_x >= 0.0f && story_tree.max_x <= 32.0f);
  CHECK(story_tree.min_y >= 0.0f && story_tree.max_y <= 32.0f);
  CHECK(story_tree.max_z <= 30.0f);
  CHECK(MaterialFaces(&story_tree, kSimVoxelMaterial_Snow) > 0);
  CHECK(ModelHash(&story_tree) != ModelHash(&snow_tree));

  SimBackgroundVoxelModel bloodpool_castle = Build(
      kSimBackgroundVoxel_BloodpoolCastle,
      kSimBackgroundVoxelDetail_Balanced);
  CHECK(bloodpool_castle.min_x >= 0.0f && bloodpool_castle.max_x <= 32.0f);
  CHECK(bloodpool_castle.min_y >= 0.0f && bloodpool_castle.max_y <= 32.0f);
  CHECK(bloodpool_castle.max_z <= 32.0f);
  CHECK(MaterialFaces(&bloodpool_castle, kSimVoxelMaterial_Gold) > 0);

  SimBackgroundVoxelModel marahna_temple = Build(
      kSimBackgroundVoxel_MarahnaTemple,
      kSimBackgroundVoxelDetail_Balanced);
  CHECK(marahna_temple.min_x >= 0.0f && marahna_temple.max_x <= 32.0f);
  CHECK(marahna_temple.min_y >= 0.0f && marahna_temple.max_y <= 32.0f);
  CHECK(marahna_temple.max_z <= 24.0f);
  CHECK(MaterialFaces(&marahna_temple, kSimVoxelMaterial_Gold) > 0);
  CHECK(ModelHash(&marahna_temple) != ModelHash(&bloodpool_castle));

  SimBackgroundVoxelModel pyramid = Build(
      kSimBackgroundVoxel_Pyramid, kSimBackgroundVoxelDetail_Balanced);
  CHECK(pyramid.min_x >= 0.0f && pyramid.max_x <= 32.0f);
  CHECK(pyramid.min_y >= 0.0f && pyramid.max_y <= 32.0f);
  CHECK(pyramid.max_z <= 28.0f);
  /* A pyramid is a pyramid: its top must be far narrower than its base. */
  CHECK(TopWidthAt(&pyramid, pyramid.max_z) <
        TopWidthAt(&pyramid, 0.0f) * 0.4f);
  CHECK(ModelHash(&pyramid) != ModelHash(&bloodpool_castle));

  SimBackgroundVoxelObject bridge_object = {
    .kind = kSimBackgroundVoxel_Bridge,
    .bridge_axis = kSimBackgroundBridgeAxis_EastWest,
    .bridge_bank_a_x = 5,
    .bridge_bank_b_x = 8,
  };
  SimBackgroundVoxelModel bridge;
  SimBackgroundVoxelModel_Build(
      &bridge_object, kSimBackgroundVoxelDetail_High, &bridge);
  CHECK(!bridge.overflow);
  CHECK(bridge.min_x >= -0.1f && bridge.max_x <= 34.1f);
  CHECK(bridge.min_y >= -0.1f && bridge.max_y <= 10.1f);
  CHECK(bridge.min_z < 0.0f);
  CHECK(bridge.max_z >= SimBackgroundBridge_AuthoredHeight() - 0.05f &&
        bridge.max_z <= SimBackgroundBridge_AuthoredHeight());
  CHECK(MaterialFaces(&bridge, kSimVoxelMaterial_Paving) > 0);
  CHECK(MaterialFaces(&bridge, kSimVoxelMaterial_WallLight) > 0);
  CHECK(MaterialFaces(&bridge, kSimVoxelMaterial_Trim) > 0);
  CHECK(MaterialFaces(&bridge, kSimVoxelMaterial_Dark) > 0);
  CHECK(MaterialFaces(&bridge, kSimVoxelMaterial_Wood) == 0);
  float paving_min_z, paving_max_z, rail_min_z, rail_max_z;
  MaterialZBounds(&bridge, kSimVoxelMaterial_Paving,
                  &paving_min_z, &paving_max_z);
  MaterialZBounds(&bridge, kSimVoxelMaterial_WallLight,
                  &rail_min_z, &rail_max_z);
  CHECK(paving_min_z > 0.0f && paving_min_z == paving_max_z);
  /* Parapets are mortised into the slab below the walking surface.  Merely
   * sharing a coplanar z=0 edge produced a detached railing at oblique pitch. */
  CHECK(rail_min_z < paving_min_z);
  CHECK(rail_max_z > paving_max_z);

  /* Bloodpool has perpendicular crossings which terminate on adjacent sides
   * of one bank. Water-opening bounds leave both intact but keep their stone
   * slabs out of the shared land cell; bank-centre spans overlapped here. */
  SimBackgroundVoxelObject bloodpool_east_west = {
    .kind = kSimBackgroundVoxel_Bridge,
    .cell_x = 17, .cell_y = 22,
    .bridge_axis = kSimBackgroundBridgeAxis_EastWest,
    .bridge_bank_a_x = 16, .bridge_bank_a_y = 22,
    .bridge_bank_b_x = 18, .bridge_bank_b_y = 22,
  };
  SimBackgroundVoxelObject bloodpool_north_south = {
    .kind = kSimBackgroundVoxel_Bridge,
    .cell_x = 18, .cell_y = 21,
    .bridge_axis = kSimBackgroundBridgeAxis_NorthSouth,
    .bridge_bank_a_x = 18, .bridge_bank_a_y = 20,
    .bridge_bank_b_x = 18, .bridge_bank_b_y = 22,
  };
  SimBackgroundBridgeBounds ew =
      SimBackgroundBridge_ResolveBounds(&bloodpool_east_west);
  SimBackgroundBridgeBounds ns =
      SimBackgroundBridge_ResolveBounds(&bloodpool_north_south);
  CHECK(ew.origin_x + ew.width < ns.origin_x);
  CHECK(ns.origin_y + ns.depth < ew.origin_y);

  /* Invalid bridge metadata fails closed instead of compiling a misleading
   * north-south fallback with arbitrary dimensions. */
  SimBackgroundVoxelObject invalid_bridge = {
    .kind = kSimBackgroundVoxel_Bridge,
    .bridge_axis = kSimBackgroundBridgeAxis_None,
  };
  SimBackgroundVoxelModel invalid_bridge_model;
  SimBackgroundVoxelModel_Build(
      &invalid_bridge, kSimBackgroundVoxelDetail_High,
      &invalid_bridge_model);
  CHECK(invalid_bridge_model.face_count == 0);
  SimBackgroundBridgeBounds null_bounds =
      SimBackgroundBridge_ResolveBounds(NULL);
  CHECK(null_bounds.width == 0.0f && null_bounds.depth == 0.0f);

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
    /* Every step of the quality setting must buy something, or the level is a
     * label the player can select for no effect. */
    CHECK(low.face_count < balanced.face_count);
    CHECK(balanced.face_count < high.face_count);
    CHECK(high.face_count < ultra.face_count);
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
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0;
         level < kSimBackgroundDevelopmentLevelCount; level++)
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

  /* Houses are the most numerous object in a developed town, so a regional
   * family that stops gaining geometry short of Ultra is where the quality
   * setting most visibly does nothing. Sixteen of the eighteen identities
   * were flat between High and Ultra. */
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0;
         level < kSimBackgroundDevelopmentLevelCount; level++) {
      SimBackgroundVoxelObject object = {
        .kind = kSimBackgroundVoxel_House,
        .town = (uint8_t)town,
        .development_level = (uint8_t)level,
        .cell_x = (uint8_t)(town * 3),
        .cell_y = (uint8_t)(level * 5),
        .record_slot = (uint8_t)(town * 3 + level),
      };
      uint16_t previous = 0;
      for (int detail = kSimBackgroundVoxelDetail_Low;
           detail < kSimBackgroundVoxelDetail_Count; detail++) {
        SimBackgroundVoxelModel step;
        SimBackgroundVoxelModel_BuildStyled(
            &object, (SimBackgroundVoxelDetail)detail,
            kSimBackgroundVoxelStyle_Varied, &step);
        CHECK(!step.overflow && step.face_count > previous);
        previous = step.face_count;
      }
    }

  /* Varied house styling is now facade-only. Across every regional identity,
   * High and Ultra may add visible surface detail but never a new footprint,
   * eave, ridge, or height beyond the Architectural model. */
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0;
         level < kSimBackgroundDevelopmentLevelCount; level++)
      for (int detail = kSimBackgroundVoxelDetail_High;
           detail <= kSimBackgroundVoxelDetail_Ultra; detail++) {
        SimBackgroundVoxelObject object = {
          .kind = kSimBackgroundVoxel_House,
          .town = (uint8_t)town,
          .development_level = (uint8_t)level,
          .cell_x = (uint8_t)(town * 3),
          .cell_y = (uint8_t)(level * 5),
          .record_slot = (uint8_t)(town * 3 + level),
        };
        SimBackgroundVoxelModel architectural, varied;
        SimBackgroundVoxelModel_BuildStyled(
            &object, (SimBackgroundVoxelDetail)detail,
            kSimBackgroundVoxelStyle_Architectural, &architectural);
        SimBackgroundVoxelModel_BuildStyled(
            &object, (SimBackgroundVoxelDetail)detail,
            kSimBackgroundVoxelStyle_Varied, &varied);
        CHECK(SameBounds(&architectural, &varied));
        CHECK(ModelHash(&architectural) != ModelHash(&varied));
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
