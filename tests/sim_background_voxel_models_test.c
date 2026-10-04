#include "sim/voxels/sim_background_voxel_models.h"
#include "sim/voxels/sim_background_bridge.h"
#include "sim/voxels/sim_background_voxel_region.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                         \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static int MaterialFaces(const SimBackgroundVoxelModel *model,
                         SimBackgroundVoxelMaterial material) {
  int count = 0;
  for (uint16_t i = 0; i < model->face_count; i++)
    if (model->faces[i].material == material) count++;
  return count;
}

static void MaterialZBounds(const SimBackgroundVoxelModel *model,
                            SimBackgroundVoxelMaterial material, float *min_z, float *max_z) {
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
      const SimBackgroundVoxelModelPoint *at = &model->faces[face].points[point];
      if (at->z < z - 0.5f || at->z > z + 0.5f) continue;
      if (at->x < min_x) min_x = at->x;
      if (at->x > max_x) max_x = at->x;
    }
  return max_x > min_x ? max_x - min_x : 0.0f;
}

static bool SamePoint(SimBackgroundVoxelModelPoint a, SimBackgroundVoxelModelPoint b) {
  return fabsf(a.x - b.x) < 0.00001f && fabsf(a.y - b.y) < 0.00001f &&
         fabsf(a.z - b.z) < 0.00001f;
}

/* Shared edges close every frond bend and the stem/crown junction. Only the
 * ground ring and narrow frond roots buried inside the crown may be open. */
static void CheckPalmConnections(const SimBackgroundVoxelModel *model) {
  struct Edge {
    SimBackgroundVoxelModelPoint a, b;
    int uses;
  } edges[kSimBackgroundVoxelModelMaxFaces * 4];
  int count = 0;
  for (int face = 0; face < model->face_count; face++) {
    const SimBackgroundVoxelModelFace *f = &model->faces[face];
    for (int at = 0; at < 4; at++) {
      SimBackgroundVoxelModelPoint a = f->points[at], b = f->points[(at + 1) % 4];
      if (SamePoint(a, b)) continue;  /* Collapsed edge of an authored triangle. */
      int found = -1;
      for (int edge = 0; edge < count; edge++)
        if ((SamePoint(a, edges[edge].a) && SamePoint(b, edges[edge].b)) ||
            (SamePoint(a, edges[edge].b) && SamePoint(b, edges[edge].a))) {
          found = edge;
          break;
        }
      if (found >= 0) edges[found].uses++;
      else edges[count++] = (struct Edge){a, b, 1};
    }
  }
  for (int edge = 0; edge < count; edge++) {
    CHECK(edges[edge].uses == 1 || edges[edge].uses == 2);
    if (edges[edge].uses == 2) continue;
    SimBackgroundVoxelModelPoint a = edges[edge].a, b = edges[edge].b;
    bool ground = fabsf(a.z) < 0.00001f && fabsf(b.z) < 0.00001f;
    bool crown_root = a.z > 11.9f && a.z < 12.1f && b.z > 11.9f && b.z < 12.1f &&
        a.x > 6.8f && a.x < 9.2f && b.x > 6.8f && b.x < 9.2f &&
        a.y > 6.8f && a.y < 9.2f && b.y > 6.8f && b.y < 9.2f;
    CHECK(ground || crown_root);
  }
}

static int PalmTips(const SimBackgroundVoxelModel *model,
                    SimBackgroundVoxelModelPoint out[8]) {
  int count = 0;
  for (int face = 0; face < model->face_count; face++) {
    const SimBackgroundVoxelModelFace *f = &model->faces[face];
    if (f->material != kSimVoxelMaterial_LeavesDark ||
        !SamePoint(f->points[1], f->points[2])) continue;
    if (count < 8) out[count] = f->points[1];
    count++;
  }
  return count;
}

static void CheckPalmDetailContinuity(void) {
  for (int seed = 0; seed < 8; seed++) {
    SimBackgroundVoxelObject object = {
      .kind = kSimBackgroundVoxel_Palm, .town = 5,
      .cell_x = seed, .cell_y = seed * 7, .group = seed,
    };
    SimBackgroundVoxelModelPoint low_tips[8] = {{0}};
    for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
      SimBackgroundVoxelModel model;
      SimBackgroundVoxelModel_BuildStyled(
          &object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(!model.overflow);
      CHECK(model.authored_face_count <= SimBackgroundVoxelModel_FaceBudget(detail));
      CHECK(model.min_x >= 0 && model.max_x <= 16);
      CHECK(model.min_y >= 0 && model.max_y <= 16);
      CHECK(model.max_z <= 15.5f);
      CHECK(TopWidthAt(&model, 0) < 3.0f);
      CheckPalmConnections(&model);
      SimBackgroundVoxelModelPoint tips[8] = {{0}};
      CHECK(PalmTips(&model, tips) == 8);
      if (detail == kSimBackgroundVoxelDetail_Low)
        memcpy(low_tips, tips, sizeof(tips));
      else
        for (int tip = 0; tip < 8; tip++) CHECK(SamePoint(low_tips[tip], tips[tip]));
    }
  }
}

static bool MaterialHasSlopedFace(const SimBackgroundVoxelModel *model,
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
      const SimBackgroundVoxelModelPoint *at = &model->faces[face].points[point];
      if (at->x < min_x) min_x = at->x;
      if (at->x > max_x) max_x = at->x;
      if (at->y < min_y) min_y = at->y;
      if (at->y > max_y) max_y = at->y;
      if (at->z < min_z) min_z = at->z;
      if (at->z > max_z) max_z = at->z;
    }
    if (max_x - min_x > 0.01f && max_y - min_y > 0.01f && max_z - min_z > 0.01f) return true;
  }
  return false;
}

static bool HorizontalFaceCovers(const SimBackgroundVoxelModel *model, float x, float y) {
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
    if (horizontal && x > min_x && x < max_x && y > min_y && y < max_y) return true;
  }
  return false;
}

static float RegionMaxZ(const SimBackgroundVoxelModel *model, float x0, float y0, float x1,
                        float y1) {
  float max_z = 0.0f;
  for (uint16_t face = 0; face < model->face_count; face++)
    for (int point = 0; point < 4; point++) {
      const SimBackgroundVoxelModelPoint *p = &model->faces[face].points[point];
      if (p->x >= x0 && p->x <= x1 && p->y >= y0 && p->y <= y1 && p->z > max_z) max_z = p->z;
    }
  return max_z;
}

static uint64_t ModelHash(const SimBackgroundVoxelModel *model) {
  const uint8_t *bytes = (const uint8_t *)model->faces;
  size_t byte_count = model->face_count * sizeof(model->faces[0]);
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < byte_count; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  hash ^= model->face_count;
  return hash;
}

static bool SameBounds(const SimBackgroundVoxelModel *a, const SimBackgroundVoxelModel *b) {
  return a->min_x == b->min_x && a->min_y == b->min_y && a->min_z == b->min_z &&
         a->max_x == b->max_x && a->max_y == b->max_y && a->max_z == b->max_z;
}

static void CheckMeasuredBounds(const SimBackgroundVoxelObject *object,
                                SimBackgroundVoxelDetail detail, SimBackgroundVoxelStyle style,
                                const SimBackgroundVoxelModel *model) {
  SimBackgroundVoxelModelBounds bounds;
  const bool valid = SimBackgroundVoxelModel_MeasureBounds(object, detail, style, &bounds);
  CHECK(valid || !model->face_count);
  if (!model->face_count) return;
  CHECK(bounds.min_x <= model->min_x && bounds.min_y <= model->min_y &&
        bounds.min_z <= model->min_z);
  CHECK(bounds.max_x >= model->max_x && bounds.max_y >= model->max_y &&
        bounds.max_z >= model->max_z);
}

static int UniqueVariedModels(SimBackgroundVoxelKind kind, SimBackgroundVoxelDetail detail) {
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
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(!model.overflow);
    CHECK(model.face_count <= SimBackgroundVoxelModel_ObjectFaceBudget(&object, detail));
    uint64_t hash = ModelHash(&model);
    bool known = false;
    for (int i = 0; i < unique; i++)
      known |= hashes[i] == hash;
    if (!known && unique < (int)(sizeof(hashes) / sizeof(hashes[0]))) hashes[unique++] = hash;
  }
  return unique;
}

static SimBackgroundVoxelModel Build(SimBackgroundVoxelKind kind, SimBackgroundVoxelDetail detail) {
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
  CHECK(model.face_count <= SimBackgroundVoxelModel_ObjectFaceBudget(&object, detail));
  return model;
}

static SimBackgroundVoxelModel BuildRegionalHouse(uint8_t town, uint8_t level) {
  SimBackgroundVoxelObject object = {
      .kind = kSimBackgroundVoxel_House,
      .town = town,
      .development_level = level,
      .record_slot = 1,
  };
  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModel_BuildStyled(&object, kSimBackgroundVoxelDetail_Balanced,
                                      kSimBackgroundVoxelStyle_Basic, &model);
  CHECK(!model.overflow && model.face_count > 0);
  return model;
}

/* Bounds on an axis-aligned face in the two remaining coordinates. */
static bool FaceRectangle(const SimBackgroundVoxelModelFace *face, int axis,
                           float plane, float bounds[4]) {
  bounds[0] = bounds[2] = 1000000.0f;
  bounds[1] = bounds[3] = -1000000.0f;
  for (int i = 0; i < 4; i++) {
    const SimBackgroundVoxelModelPoint p = face->points[i];
    const float v[] = {p.x, p.y, p.z};
    if (v[axis] != plane) return false;
    for (int k = 0; k < 2; k++) {
      float value = v[(axis + k + 1) % 3];
      if (value < bounds[k * 2]) bounds[k * 2] = value;
      if (value > bounds[k * 2 + 1]) bounds[k * 2 + 1] = value;
    }
  }
  return true;
}

static bool MaterialOverlap(const SimBackgroundVoxelModel *model,
                             int material_a, int material_b, int axis, float plane) {
  for (int a = 0; a < model->face_count; a++) {
    float ra[4];
    if (model->faces[a].material != material_a ||
        !FaceRectangle(&model->faces[a], axis, plane, ra)) continue;
    for (int b = 0; b < model->face_count; b++) {
      if (a == b) continue;
      float rb[4];
      if (model->faces[b].material != material_b ||
          !FaceRectangle(&model->faces[b], axis, plane, rb)) continue;
      if (ra[0] < rb[1] && rb[0] < ra[1] &&
          ra[2] < rb[3] && rb[2] < ra[3]) return true;
    }
  }
  return false;
}

static bool GroundedFacadeAt(const SimBackgroundVoxelModel *model,
                             float x, float y, float height) {
  for (int face = 0; face < model->face_count; face++) {
    float bounds[4];
    if (model->faces[face].material == kSimVoxelMaterial_WallLight &&
        FaceRectangle(&model->faces[face], 1, y, bounds) &&
        bounds[0] == 0.0f && bounds[1] >= height &&
        bounds[2] < x && bounds[3] > x) return true;
  }
  return false;
}

static bool ContactAt(const SimBackgroundVoxelModelContact *contacts,
                       int count, float x, float y, float apron) {
  for (int part = 0; part < count; part++) {
    SimBackgroundVoxelModelContact c = contacts[part];
    if (c.x0 - apron <= x && c.x1 + apron >= x &&
        c.y0 - apron <= y && c.y1 + apron >= y) return true;
  }
  return false;
}

/* Vertical ray through triangulated quads: catches an open roof and verifies
 * that a courtyard really exposes the ground instead of a hidden foundation. */
static float ProjectedSurfaceAt(const SimBackgroundVoxelModel *model,
                                 float x, float y, bool from_front) {
  float highest = -1.0f;
  for (int f = 0; f < model->face_count; f++) {
    const SimBackgroundVoxelModelPoint *p = model->faces[f].points;
    for (int t = 0; t < 2; t++) {
      SimBackgroundVoxelModelPoint a = p[0], b = p[t + 1], c = p[t + 2];
      if (from_front) {
        float ay = a.y, by = b.y, cy = c.y;
        a.y = a.z;
        a.z = ay;
        b.y = b.z;
        b.z = by;
        c.y = c.z;
        c.z = cy;
      }
      float determinant = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
      if (fabsf(determinant) < 0.0001f) continue;
      float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / determinant;
      float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / determinant;
      if (u < -0.0001f || v < -0.0001f || u + v > 1.0001f) continue;
      float z = u * a.z + v * b.z + (1.0f - u - v) * c.z;
      if (z > highest) highest = z;
    }
  }
  return highest;
}

static float SurfaceHeightAt(const SimBackgroundVoxelModel *model, float x, float y) {
  return ProjectedSurfaceAt(model, x, y, false);
}

static float SurfaceFrontAt(const SimBackgroundVoxelModel *model, float x, float z) {
  return ProjectedSurfaceAt(model, x, z, true);
}

static void CheckAuditRegressions(void) {
  /* Cover every shape profile in every climate and style, including the two
   * real audit inputs that overflowed only after root trim was appended. */
  for (int town = 1; town <= 6; town++)
    for (int seed = 0; seed < 8; seed++)
      for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++)
        for (int style = 0; style < kSimBackgroundVoxelStyle_Count; style++) {
          SimBackgroundVoxelObject tree = {
            .kind = kSimBackgroundVoxel_Tree, .town = (uint8_t)town,
            .cell_x = (uint8_t)seed, .cell_y = (uint8_t)((seed * 7) % 32),
            .group = (uint16_t)seed, .record_slot = (uint8_t)seed,
          };
          SimBackgroundVoxelModel model;
          SimBackgroundVoxelModel_BuildStyled(&tree, detail, style, &model);
          CHECK(!model.overflow && model.face_count > 0);
          CHECK(model.authored_face_count <= SimBackgroundVoxelModel_FaceBudget(detail));
        }

  SimBackgroundVoxelModel model;
  SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_BloodpoolCastle, .town = 2};
  CHECK(SimBackgroundVoxelModel_FaceBudget(kSimBackgroundVoxelDetail_Low) == 64);
  CHECK(SimBackgroundVoxelModel_FaceBudget(kSimBackgroundVoxelDetail_Balanced) == 160);
  CHECK(SimBackgroundVoxelModel_ObjectFaceBudget(&object, kSimBackgroundVoxelDetail_Low) == 128);
  CHECK(SimBackgroundVoxelModel_ObjectFaceBudget(
      &object, kSimBackgroundVoxelDetail_Balanced) == 160);
  SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
  int count = SimBackgroundVoxelModel_Contacts(&object, contacts);
  CHECK(count == 11);
  for (int part = 0; part < count; part++) {
    const SimBackgroundVoxelModelContact c = contacts[part];
    /* Include the renderer's 0.4-unit foundation apron in the ground check. */
    CHECK(!(c.x0 - 0.4f < 8.0f && c.x1 + 0.4f > 8.0f &&
            c.y0 - 0.4f < 18.0f && c.y1 + 0.4f > 18.0f));
    CHECK(!(c.x0 - 0.4f < 24.0f && c.x1 + 0.4f > 24.0f &&
            c.y0 - 0.4f < 18.0f && c.y1 + 0.4f > 18.0f));
  }
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(!model.overflow);
    SimBackgroundVoxelModel detailed_castle;
    SimBackgroundVoxelModel_BuildStyled(&object, kSimBackgroundVoxelDetail_Ultra,
                                       kSimBackgroundVoxelStyle_Varied, &detailed_castle);
    CHECK(SameBounds(&model, &detailed_castle));
    CHECK(SurfaceHeightAt(&model, 12.0f, 12.0f) > 20.0f);
    CHECK(SurfaceHeightAt(&model, 14.5f, 13.0f) == 26.0f);
    CHECK(SurfaceHeightAt(&model, 17.5f, 13.0f) == 26.0f);
    CHECK(GroundedFacadeAt(&model, 16.0f, 22.0f, 20.0f));
    CHECK(SurfaceHeightAt(&model, 11.0f, 9.0f) == 32.0f);
    CHECK(SurfaceHeightAt(&model, 21.0f, 9.0f) == 32.0f);
    for (int side = 0; side < 2; side++) {
      float x = side ? 27.5f : 4.5f;
      CHECK(SurfaceHeightAt(&model, x, 6.5f) == 28.0f);
      CHECK(SurfaceHeightAt(&model, x, 29.5f) == 20.0f);
      CHECK(GroundedFacadeAt(&model, x, 32.0f, 13.0f));
      CHECK(ContactAt(contacts, count, x, 31.9f, 0.0f));
    }
    CHECK(SurfaceHeightAt(&model, 8.0f, 18.0f) < 0.0f);
    CHECK(SurfaceHeightAt(&model, 24.0f, 18.0f) < 0.0f);
    CHECK(fabsf(SurfaceHeightAt(&model, 16.0f, 30.5f) - 15.2f) < 0.001f);
    /* The gate and corner piers stand in front of recessed curtain panels.
     * A ray through the arch reaches the keep, not a painted solid wall. */
    CHECK(fabsf(SurfaceFrontAt(&model, 12.0f, 6.0f) - 32.0f) < 0.001f);
    CHECK(fabsf(SurfaceFrontAt(&model, 20.0f, 6.0f) - 32.0f) < 0.001f);
    CHECK(fabsf(SurfaceFrontAt(&model, 9.0f, 6.0f) - 29.5f) < 0.001f);
    CHECK(fabsf(SurfaceFrontAt(&model, 23.0f, 6.0f) - 29.5f) < 0.001f);
    CHECK(SurfaceFrontAt(&model, 16.0f, 3.0f) < 26.5f);
    CHECK(SurfaceFrontAt(&model, 16.0f, 7.0f) < 26.5f);
    CHECK(fabsf(SurfaceFrontAt(&model, 16.0f, 8.0f) - 32.0f) < 0.001f);
    CHECK(fabsf(SurfaceFrontAt(&model, 13.6f, 6.0f) - 32.0f) < 0.001f);
    for (int side = 0; side < 2; side++) {
      float bay_x = side ? 23.0f : 9.0f;
      CHECK(SurfaceHeightAt(&model, bay_x, 31.0f) < 0.0f);
      CHECK(!ContactAt(contacts, count, bay_x, 31.0f, 0.4f));
    }
    CHECK(MaterialFaces(&model, kSimVoxelMaterial_Glass) == 0);
    CHECK(!MaterialOverlap(&model, kSimVoxelMaterial_Dark, kSimVoxelMaterial_Glass, 1, 31.05f));
  }
  object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_Windmill, .town = 1};
  CHECK(SimBackgroundVoxelModel_ObjectFaceBudget(&object, kSimBackgroundVoxelDetail_Low) == 64);
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    int round_walls = 0;
    for (int face = 0; face < model.face_count; face++) {
      const SimBackgroundVoxelModelFace *f = &model.faces[face];
      if (f->material == kSimVoxelMaterial_Wall &&
          f->points[0].z == (detail == kSimBackgroundVoxelDetail_Low ? 0.0f : 2.0f) && f->points[2].z == 22.0f &&
          f->points[0].x != f->points[1].x &&
          f->points[0].y != f->points[1].y) round_walls++;
    }
    CHECK(round_walls == 8);
    CHECK(SurfaceHeightAt(&model, 16.0f, 15.5f) > 5.8f);
  }
  object.flags = kSimBackgroundVoxel_UnderConstruction;
  CHECK(SimBackgroundVoxelModel_Contacts(&object, contacts) >= 2);
  for (int phase = 0; phase < 3; phase++)
    for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
      object.animation_phase = (uint8_t)phase;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(!model.overflow);
      CHECK(SurfaceHeightAt(&model, 16.0f, 7.0f) > 6.0f);
    }
  object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_Factory, .town = 1};
  SimBackgroundVoxelModel_BuildStyled(&object, kSimBackgroundVoxelDetail_Ultra,
                                     kSimBackgroundVoxelStyle_Varied, &model);
  CHECK(!MaterialOverlap(&model, kSimVoxelMaterial_Wall, kSimVoxelMaterial_WallLight, 1, 30.5f));
  CHECK(!MaterialOverlap(&model, kSimVoxelMaterial_Wall, kSimVoxelMaterial_WallLight, 1, 1.5f));
  CHECK(!MaterialOverlap(&model, kSimVoxelMaterial_Roof, kSimVoxelMaterial_Trim, 0, 20.8f));

  object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_MarahnaTemple, .town = 5};
  CHECK(SimBackgroundVoxelModel_ObjectFaceBudget(&object, kSimBackgroundVoxelDetail_Low) == 144);
  CHECK(SimBackgroundVoxelModel_ObjectFaceBudget(
      &object, kSimBackgroundVoxelDetail_Balanced) == 256);
  count = SimBackgroundVoxelModel_Contacts(&object, contacts);
  CHECK(count == 12 && count <= kSimBackgroundVoxelModelMaxContacts);
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(!model.overflow);
    CHECK(RegionMaxZ(&model, 0, 0, 10, 21) >= 18.0f);
    CHECK(RegionMaxZ(&model, 22, 0, 32, 21) >= 18.0f);
    CHECK(RegionMaxZ(&model, 11, 0, 21, 24) == 24.0f);
    CHECK(GroundedFacadeAt(&model, 11.4f, 22.5f, 10.0f));
    CHECK(GroundedFacadeAt(&model, 20.5f, 22.5f, 10.0f));
    for (int side = 0; side < 2; side++) {
      float center = side ? 26.5f : 5.5f;
      CHECK(SurfaceHeightAt(&model, center + 2.5f, 12.0f) >
            SurfaceHeightAt(&model, center + 3.5f, 12.0f));
      /* The belly projects beyond the ground radius before tapering inward. */
      CHECK(SurfaceHeightAt(&model, center + 4.7f, 12.0f) > 3.0f);
      CHECK(SurfaceHeightAt(&model, center + 4.7f, 12.0f) < 7.0f);
      float garden = side ? 26.0f : 6.0f;
      CHECK(SurfaceHeightAt(&model, garden, 20.0f) < 0.0f);
      CHECK(!ContactAt(contacts, count, garden, 20.0f, 0.4f));
      CHECK(SurfaceHeightAt(&model, garden, 25.5f) == 9.5f);
      CHECK(ContactAt(contacts, count, garden, 25.5f, 0.0f));
      /* Continuous outside/front wall runs, including the old gap
       * between the mound and garden border; keep their foundations narrow. */
      const float support[][2] = {
        {side ? 30.5f : 1.5f, 18.0f},
        {side ? 30.5f : 1.5f, 30.5f},
        {garden, 30.5f},
      };
      for (int part = 0; part < 3; part++) {
        CHECK(fabsf(SurfaceHeightAt(&model, support[part][0], support[part][1]) - 2.8f) < 0.001f);
        CHECK(ContactAt(contacts, count, support[part][0], support[part][1], 0.0f));
      }
      float gate_x = side ? 21.0f : 11.0f;
      CHECK(fabsf(SurfaceHeightAt(&model, gate_x, 30.5f) - 5.2f) < 0.001f);
      CHECK(ContactAt(contacts, count, gate_x, 30.5f, 0.0f));
      CHECK(SurfaceHeightAt(&model, gate_x, 25.5f) < 0.0f);
      CHECK(!ContactAt(contacts, count, gate_x, 25.5f, 0.4f));
    }
    int ground_slopes = 0;
    for (int face = 0; face < model.face_count; face++) {
      const SimBackgroundVoxelModelFace *f = &model.faces[face];
      if (f->material == kSimVoxelMaterial_Gold && f->points[0].z == 0.0f &&
          f->points[1].z == 0.0f && f->points[2].z > 0.0f &&
          f->points[2].z < 6.0f) ground_slopes++;
      if (f->material == kSimVoxelMaterial_Dark || f->material == kSimVoxelMaterial_Glass)
        for (int vertex = 0; vertex < 4; vertex++)
          CHECK(f->points[vertex].x >= 10.5f && f->points[vertex].x <= 21.5f);
    }
    CHECK(ground_slopes == 16);
  }

  object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_Pyramid, .town = 3};
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel plain;
    object.flags = 0;
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &plain);
    object.flags = kSimBackgroundVoxel_PyramidEye;
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(!model.overflow && !plain.overflow);
    CHECK(SameBounds(&model, &plain));
    CHECK(MaterialFaces(&plain, kSimVoxelMaterial_Glass) == 0);
    CHECK(MaterialFaces(&model, kSimVoxelMaterial_Glass) > 0);
    CHECK(ModelHash(&model) != ModelHash(&plain));
    /* Every stone vertex lies on one of four continuous casing slopes. No
     * brick course introduces a horizontal ledge or a stepped silhouette. */
    for (int face = 0; face < plain.face_count; face++)
      for (int vertex = 0; vertex < 4; vertex++) {
        SimBackgroundVoxelModelPoint p = plain.faces[face].points[vertex];
        float edge = fmaxf(fabsf(p.x - 16.0f), fabsf(p.y - 16.0f));
        CHECK(fabsf(edge - 15.5f * (1.0f - p.z / 28.0f)) < 0.19f);
      }

  }

  object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House, .town = 6,
                                     .development_level = 2};
  for (int alternate = 0; alternate < 2; alternate++)
    for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++)
      for (int style = 0; style < kSimBackgroundVoxelStyle_Count; style++) {
        object.flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0;
        SimBackgroundVoxelModel_BuildStyled(&object, detail, style, &model);
        CHECK(!model.overflow);
        CHECK(!MaterialHasSlopedFace(&model, kSimVoxelMaterial_Roof));
        CHECK(!MaterialHasSlopedFace(&model, kSimVoxelMaterial_RoofLight));
      }

  object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House, .town = 5,
    .development_level = 2, .visual_state = kSimStructureVisualState_Finished,
    .visual_metatile = 0x3A};
  CHECK(SimBackgroundVoxelRegion_ObjectHouseStyle(&object) ==
        kSimBackgroundHouseStyle_MarahnaStilt);
  SimBackgroundVoxelModel_BuildStyled(&object, kSimBackgroundVoxelDetail_High,
                                     kSimBackgroundVoxelStyle_Basic, &model);
  CHECK(model.max_z == 12.5f);
  object.visual_metatile = 0x0A;
  CHECK(SimBackgroundVoxelRegion_ObjectHouseStyle(&object) ==
        kSimBackgroundHouseStyle_MarahnaLogCabin);
  SimBackgroundVoxelModel_BuildStyled(&object, kSimBackgroundVoxelDetail_High,
                                     kSimBackgroundVoxelStyle_Basic, &model);
  CHECK(model.max_z == 12.0f);
}

static void CheckEnvironmentModels(void) {
  static const uint8_t tiles[] = {0x61, 0x62, 0x63, 0x69, 0x6A, 0x6B};
  for (int at = 0; at < 6; at++) {
    SimBackgroundVoxelObject object = {
      .kind = (uint8_t)SimBackgroundVoxelRegion_RockKind(tiles[at]),
      .town = 4, .visual_metatile = tiles[at],
    };
    for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
      SimBackgroundVoxelModel model;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(!model.overflow && model.face_count <= 64);
      CHECK(model.min_z == 0.0f && model.max_z > 1.0f);
      CHECK(model.min_x >= 0.0f && model.max_x <= 16.0f);
      CHECK(model.min_y >= 0.0f && model.max_y <= 16.0f);
      CHECK(model.max_z <= SimBackgroundVoxelRegion_AuthoredHeight(&object));
      SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
      CHECK(SimBackgroundVoxelModel_Contacts(&object, contacts) == 0);
      CHECK(!SimBackgroundVoxelModel_CastsShadow(&object));
      if (tiles[at] == 0x62) CHECK(SurfaceHeightAt(&model, 3.0f, 3.0f) < 0.0f);
    }
  }
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_Shrub, .town = 1};
    SimBackgroundVoxelModel model;
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(MaterialHasSlopedFace(&model, kSimVoxelMaterial_Leaves));
    CHECK(SurfaceHeightAt(&model, 8.0f, 8.0f) > SurfaceHeightAt(&model, 12.0f, 8.0f));
    float low, high;
    MaterialZBounds(&model, kSimVoxelMaterial_Trunk, &low, &high);
    CHECK(low == 0.0f && high <= 3.0f);
  }
}

/* Continuous crowns must be closed at every LOD, including the collapsed
 * pole triangles. Open branch ends are deliberately buried and excluded. */
static void CheckClosedCrown(const SimBackgroundVoxelModel *model) {
  for (int f = 0; f < model->face_count; f++) {
    const SimBackgroundVoxelModelFace *face = &model->faces[f];
    if (face->material == kSimVoxelMaterial_Trunk) continue;
    for (int e = 0; e < 4; e++) {
      SimBackgroundVoxelModelPoint a = face->points[e], b = face->points[(e + 1) & 3];
      if (SamePoint(a, b)) continue;
      int uses = 0;
      for (int other = 0; other < model->face_count; other++) {
        const SimBackgroundVoxelModelFace *candidate = &model->faces[other];
        if (candidate->material == kSimVoxelMaterial_Trunk) continue;
        for (int edge = 0; edge < 4; edge++) {
          SimBackgroundVoxelModelPoint c = candidate->points[edge];
          SimBackgroundVoxelModelPoint d = candidate->points[(edge + 1) & 3];
          if ((SamePoint(a, c) && SamePoint(b, d)) ||
              (SamePoint(a, d) && SamePoint(b, c))) uses++;
        }
      }
      CHECK(uses == 2);
    }
  }
}

static int CanopyCoverage(const SimBackgroundVoxelModel *model) {
  int covered = 0;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      if (SurfaceHeightAt(model, x + .5f, y + .5f) > 3) covered++;
  return covered;
}

static int CanopyBorderCoverage(const SimBackgroundVoxelModel *model,
                                bool vertical, float border) {
  int covered = 0;
  for (int sample = 0; sample < 32; sample++) {
    float along = 8 + ((sample + .5f) * .5f - 8) / .90f;
    if (SurfaceHeightAt(model, vertical ? border : along,
                        vertical ? along : border) > 3) covered++;
  }
  return covered;
}

static void CheckForestClusters(void) {
  const int families[] = {kSimBackgroundVoxel_Tree, kSimBackgroundVoxel_BroadTree};
  for (int family = 0; family < 2; family++)
    for (int town = 1; town <= 6; town++)
      for (int seed = 0; seed < 16; seed++)
        for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
          SimBackgroundVoxelObject object = {.kind = families[family], .town = town,
              .cell_x = seed, .tree_edges = seed % 15 + 1};
          SimBackgroundVoxelModel model, repeat;
          SimBackgroundVoxelModel_BuildStyled(
              &object, detail, kSimBackgroundVoxelStyle_Varied, &model);
          CHECK(!model.overflow && model.min_z == 0);
          CHECK(model.authored_face_count <= SimBackgroundVoxelModel_FaceBudget(detail));
          CHECK(model.max_z == SimBackgroundVoxelRegion_AuthoredHeight(&object));
          uint8_t edges = object.tree_edges;
          CHECK(model.min_x >= (edges & kSimBackgroundTreeEdge_West ? -3.2f : 0));
          CHECK(model.max_x <= (edges & kSimBackgroundTreeEdge_East ? 19.2f : 16));
          CHECK(model.min_y >= (edges & kSimBackgroundTreeEdge_North ? -3.2f : 0));
          CHECK(model.max_y <= (edges & kSimBackgroundTreeEdge_South ? 19.2f : 16));
          /* Joined crowns reach their neighbor after the presentation's .90
           * footprint scale. Unjoined edges still fit on their own source plot. */
          if (edges & kSimBackgroundTreeEdge_East)
            CHECK(CanopyBorderCoverage(&model, true, 8 + 8 / .90f) >= 4);
          if (edges & kSimBackgroundTreeEdge_West)
            CHECK(CanopyBorderCoverage(&model, true, 8 - 8 / .90f) >= 4);
          if (edges & kSimBackgroundTreeEdge_North)
            CHECK(CanopyBorderCoverage(&model, false, 8 - 8 / .90f) >= 4);
          if (edges & kSimBackgroundTreeEdge_South)
            CHECK(CanopyBorderCoverage(&model, false, 8 + 8 / .90f) >= 4);
          SimBackgroundVoxelModel_BuildStyled(
              &object, detail, kSimBackgroundVoxelStyle_Varied, &repeat);
          CHECK(ModelHash(&model) == ModelHash(&repeat));
          SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
          int count = SimBackgroundVoxelModel_Contacts(&object, contacts);
          bool marahna = object.kind == kSimBackgroundVoxel_BroadTree && town == 5;
          CHECK(count == (marahna ? 2 : 4));
          if (marahna) CHECK(!ContactAt(contacts, count, 0, 0, 0));
          else CHECK(!ContactAt(contacts, count, 8, 8, 0));
          for (int contact = 0; contact < count; contact++) {
            SimBackgroundVoxelModelContact c = contacts[contact];
            if (marahna) {
              CHECK(c.x1 - c.x0 >= 3.2f - .001f);
              CHECK(c.y1 - c.y0 >= 3.2f - .001f);
              float min_x = c.x1, min_y = c.y1, max_x = c.x0, max_y = c.y0;
              for (int face = 0; face < model.face_count; face++) {
                if (model.faces[face].material != kSimVoxelMaterial_Trunk) continue;
                for (int vertex = 0; vertex < 4; vertex++) {
                  SimBackgroundVoxelModelPoint p = model.faces[face].points[vertex];
                  if (p.z != 0 || p.x < c.x0 || p.x > c.x1 ||
                      p.y < c.y0 || p.y > c.y1) continue;
                  min_x = fminf(min_x, p.x);
                  min_y = fminf(min_y, p.y);
                  max_x = fmaxf(max_x, p.x);
                  max_y = fmaxf(max_y, p.y);
                }
              }
              /* Both actual trunk feet stay thick, even at Low detail. */
              CHECK(max_x - min_x > 2.4f && max_y - min_y > 2.6f);
            }
            CHECK(SurfaceHeightAt(&model, (c.x0 + c.x1) * .5f,
                                  (c.y0 + c.y1) * .5f) > (marahna ? 13.1f : 9));
          }
          if (!seed) {
            CheckClosedCrown(&model);
            SimBackgroundVoxelModelBounds bounds;
            CHECK(SimBackgroundVoxelModel_MeasureBounds(
                &object, detail, kSimBackgroundVoxelStyle_Varied, &bounds));
            CHECK(bounds.min_x <= model.min_x && bounds.max_x >= model.max_x);
            CHECK(bounds.min_y <= model.min_y && bounds.max_y >= model.max_y);
            object.tree_edges = 0;
            object.flags = kSimBackgroundVoxel_IsolatedTree;
            SimBackgroundVoxelModel_BuildStyled(
                &object, detail, kSimBackgroundVoxelStyle_Varied, &repeat);
            CHECK(CanopyCoverage(&model) > CanopyCoverage(&repeat) + 15);
            CHECK(ModelHash(&model) != ModelHash(&repeat));
          }
        }
}

static void CheckRecognitionPolish(void) {
  const int foliage[] = {kSimBackgroundVoxel_Tree, kSimBackgroundVoxel_BroadTree,
                         kSimBackgroundVoxel_StoryTree, kSimBackgroundVoxel_Shrub};
  for (int at = 0; at < 4; at++)
    for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
      SimBackgroundVoxelObject object = {.kind = foliage[at], .town = 6};
      SimBackgroundVoxelModel model;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(!model.overflow && model.min_z == 0);
      CHECK(model.authored_face_count <= SimBackgroundVoxelModel_ObjectFaceBudget(&object, detail));
      CheckClosedCrown(&model);
      if (object.kind == kSimBackgroundVoxel_StoryTree) {
        CHECK(model.max_z > 28 && model.max_z < 32);
        CHECK(model.max_x - model.min_x > 25);
        CHECK(model.max_x <= 32 && model.min_x >= 0);
        CHECK(model.max_y <= 32 && model.min_y >= 0);
        CHECK(SimBackgroundVoxelModel_ObjectFaceBudget(&object, detail) >= 128);
      }
    }
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel model;
    SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_Factory, .town = 1};
    SimBackgroundVoxelModel_Build(&object, detail, &model);
    CHECK(SurfaceHeightAt(&model, 25, 11) >= 16.7f);
    CHECK(SurfaceHeightAt(&model, 25, 19) >= 16.7f);
    object.flags = kSimBackgroundVoxel_UnderConstruction;
    SimBackgroundVoxelModel_Build(&object, detail, &model);
    CHECK(!model.overflow && SurfaceHeightAt(&model, 10, 16) < 0);
    CHECK(MaterialHasSlopedFace(&model, kSimVoxelMaterial_Wood));
    object.kind = kSimBackgroundVoxel_House;
    for (int phase = 0; phase < 2; phase++) {
      object.animation_phase = phase;
      SimBackgroundVoxelModel_Build(&object, detail, &model);
      CHECK(!model.overflow && MaterialHasSlopedFace(&model, kSimVoxelMaterial_Wood));
    }
    object.kind = kSimBackgroundVoxel_Windmill;
    float previous_height = 0;
    for (int phase = 0; phase < 3; phase++) {
      object.animation_phase = phase;
      SimBackgroundVoxelModel_Build(&object, detail, &model);
      CHECK(!model.overflow && model.max_z > previous_height);
      previous_height = model.max_z;
      if (phase == 2) {
        CHECK(MaterialHasSlopedFace(&model, kSimVoxelMaterial_Roof));
        CHECK(SurfaceHeightAt(&model, 16, 15.5f) > 5.8f);
      }
    }
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House,
        .town = 1, .development_level = 2};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    /* Window lintels and jambs meet; overlapping caps used to tie in D32. */
    CHECK(!MaterialOverlap(&model, kSimVoxelMaterial_Trim, kSimVoxelMaterial_Trim, 2, 8.4f));
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House,
        .town = 5, .development_level = 1};
    for (int alternate = 0; alternate < 2; alternate++) {
      object.flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(!model.overflow);
      float min, max;
      MaterialZBounds(&model, kSimVoxelMaterial_Dark, &min, &max);
      CHECK(min >= 5.3f); /* No invented opening below the raised floor. */
      SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
      int count = SimBackgroundVoxelModel_Contacts(&object, contacts);
      CHECK(count == 4);
      CHECK(!ContactAt(contacts, count, 8, 12, .4f));
    }
    object.development_level = 2;
    object.flags = 0;
    SimBackgroundVoxelModel_Build(&object, detail, &model);
    CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) >= 6);
  }
}

static void CheckFoliageShadows(void) {
  SimBackgroundVoxelModelPoint hull[kSimBackgroundVoxelFoliageShadowMaxPoints];
  SimBackgroundVoxelObject tree = {.kind = kSimBackgroundVoxel_Tree, .town = 1};
  CHECK(!SimBackgroundVoxelModel_CastsShadow(NULL));
  CHECK(SimBackgroundVoxelModel_CastsShadow(&tree));
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(NULL, 0, 0, hull) == 0);
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(&tree, NAN, 0, hull) == 0);
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(&tree, 0, 0, NULL) == 0);
  SimBackgroundVoxelObject repeat = tree;
  repeat.cell_x += 16;
  repeat.cell_y += 16;
  repeat.group += 16;
  SimBackgroundVoxelModelPoint same[kSimBackgroundVoxelFoliageShadowMaxPoints];
  CHECK(SimBackgroundVoxelModel_FoliageShadowVariant(&tree) ==
        SimBackgroundVoxelModel_FoliageShadowVariant(&repeat));
  int n = SimBackgroundVoxelModel_FoliageShadowHull(&tree, .7f, -.3f, hull);
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(&repeat, .7f, -.3f, same) == n);
  CHECK(!memcmp(hull, same, n * sizeof(*hull)));
  SimBackgroundVoxelObject shrub = {.kind = kSimBackgroundVoxel_Shrub, .town = 1};
  CHECK(SimBackgroundVoxelModel_CastsShadow(&shrub));
  CHECK(SimBackgroundVoxelModel_FoliageShadowVariant(&shrub) !=
        SimBackgroundVoxelModel_FoliageShadowVariant(&tree));
  repeat = shrub;
  repeat.town = 6;
  repeat.cell_x = 7;
  repeat.cell_y = 13;
  repeat.group = 9;
  CHECK(SimBackgroundVoxelModel_FoliageShadowVariant(&shrub) ==
        SimBackgroundVoxelModel_FoliageShadowVariant(&repeat));
  n = SimBackgroundVoxelModel_FoliageShadowHull(&shrub, .7f, -.3f, hull);
  CHECK(n >= 8);
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(&repeat, .7f, -.3f, same) == n);
  CHECK(!memcmp(hull, same, n * sizeof(*hull)));
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(&shrub, 0, INFINITY, hull) == 0);
  repeat.kind = kSimBackgroundVoxel_House;
  CHECK(!SimBackgroundVoxelModel_UsesFoliageShadow(&repeat));
  CHECK(SimBackgroundVoxelModel_FoliageShadowHull(&repeat, 0, 0, hull) == 0);
  /* The renderer reuses one outline for every object with the same key: equal
   * keys must build identical outlines, and no two families may share one. */
  {
    enum { kObjects = 96 };
    static const SimBackgroundVoxelKind families[] = {
      kSimBackgroundVoxel_Tree, kSimBackgroundVoxel_Shrub, kSimBackgroundVoxel_Palm,
      kSimBackgroundVoxel_BroadTree, kSimBackgroundVoxel_StoryTree,
      kSimBackgroundVoxel_Tree, kSimBackgroundVoxel_BroadTree,
      kSimBackgroundVoxel_BroadTree,
    };
    enum { kFamilies = sizeof(families) / sizeof(families[0]) };
    static uint16_t keys[kFamilies][kObjects];
    static int counts[kFamilies][kObjects];
    static SimBackgroundVoxelModelPoint outlines[kFamilies][kObjects]
        [kSimBackgroundVoxelFoliageShadowMaxPoints];
    int shared = 0;
    for (int family = 0; family < kFamilies; family++)
      for (int at = 0; at < kObjects; at++) {
        SimBackgroundVoxelObject object = {.kind = families[family], .town = 5,
            .cell_x = at % 32, .cell_y = (at * 7) % 32, .group = at / 3};
        if (family == 7) object.town = 3;
        if (family >= 5) object.tree_edges = at & 1 ? 15 : kSimBackgroundTreeEdge_East;
        keys[family][at] = SimBackgroundVoxelModel_FoliageShadowVariant(&object);
        counts[family][at] = SimBackgroundVoxelModel_FoliageShadowHull(
            &object, .3f, -.5f, outlines[family][at]);
        CHECK(counts[family][at] >= 8);
        for (int other = 0; other < family; other++)
          for (int before = 0; before < kObjects; before++)
            CHECK(keys[other][before] != keys[family][at]);
        for (int before = 0; before < at; before++) {
          if (keys[family][before] != keys[family][at]) continue;
          shared++;
          CHECK(counts[family][before] == counts[family][at]);
          CHECK(!memcmp(outlines[family][before], outlines[family][at],
                        counts[family][at] * sizeof(hull[0])));
        }
      }
    CHECK(shared > kObjects); /* The property was exercised, not vacuous. */
  }
  /* Curved crowns must still fit the outline budget at grazing light angles. */
  for (int slope = 0; slope <= 32; slope++)
    for (int direction = 0; direction < 24; direction++) {
      float angle = direction * 6.2831853f / 24;
      CHECK(SimBackgroundVoxelModel_FoliageShadowHull(
          &shrub, slope * .25f * cosf(angle), slope * .25f * sinf(angle), hull) >= 8);
    }
  static const float casts[][2] = {{0, 0}, {.6f, -.4f}, {-.6f, .4f}, {2, 1}, {-2, -1}};
  static const SimBackgroundVoxelKind kinds[] = {
    kSimBackgroundVoxel_Tree, kSimBackgroundVoxel_Shrub, kSimBackgroundVoxel_Palm,
    kSimBackgroundVoxel_BroadTree, kSimBackgroundVoxel_StoryTree,
    kSimBackgroundVoxel_Tree, kSimBackgroundVoxel_BroadTree,
  };
  for (size_t kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); kind++)
    for (int town = 1; town <= 6; town++)
      for (int seed = 0; seed < 4; seed++)
        for (size_t cast = 0; cast < sizeof(casts) / sizeof(casts[0]); cast++) {
          SimBackgroundVoxelObject object = {.kind = kinds[kind], .town = town,
              .cell_x = seed, .cell_y = seed * 3};
          bool forest = kind >= 5;
          if (forest) {
            static const uint8_t edges[] = {1, 2, 6, 15};
            object.tree_edges = edges[seed];
          }
          CHECK(SimBackgroundVoxelModel_UsesFoliageShadow(&object));
          /* The ancient tree owns a two-cell plot; the rest one cell. */
          const float plot = kinds[kind] == kSimBackgroundVoxel_StoryTree ? 32 : 16;
          float dx = casts[cast][0], dy = casts[cast][1];
          int count = SimBackgroundVoxelModel_FoliageShadowHull(&object, dx, dy, hull);
          CHECK(count >= 8 && count <= kSimBackgroundVoxelFoliageShadowMaxPoints);
          float twice_area = 0;
          float min_x = plot, min_y = plot, max_x = 0, max_y = 0;
          for (int i = 0; i < count; i++) {
            SimBackgroundVoxelModelPoint a = hull[i], b = hull[(i + 1) % count];
            CHECK(isfinite(a.x) && isfinite(a.y) && a.z == 0);
            twice_area += a.x * b.y - b.x * a.y;
            min_x = fminf(min_x, a.x);
            max_x = fmaxf(max_x, a.x);
            min_y = fminf(min_y, a.y);
            max_y = fmaxf(max_y, a.y);
            if (!cast) {
              float x0 = forest
                  ? (object.tree_edges & kSimBackgroundTreeEdge_West ? -3.2f : 0) : .5f;
              float x1 = forest
                  ? (object.tree_edges & kSimBackgroundTreeEdge_East ? 19.2f : 16) : plot - .5f;
              float y0 = forest
                  ? (object.tree_edges & kSimBackgroundTreeEdge_North ? -3.2f : 0) : .5f;
              float y1 = forest
                  ? (object.tree_edges & kSimBackgroundTreeEdge_South ? 19.2f : 16) : plot - .5f;
              CHECK(a.x > x0 && a.x < x1 && a.y > y0 && a.y < y1);
            }
          }
          CHECK(twice_area > 120);
          if (!cast && !forest) {
            /* A round canopy, never the plot's square: well under the plot,
             * and cutting clearly into its own bounding box's corners. */
            CHECK(twice_area < 1.13f * plot * plot);
            CHECK(twice_area < 2 * .9f * (max_x - min_x) * (max_y - min_y));
          }
          /* The inexpensive outline must enclose the light-projected model at
           * every LOD, to within the subpixel error of its twenty-four-sided rings. */
          for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
            SimBackgroundVoxelModel model;
            SimBackgroundVoxelModel_BuildStyled(
                &object, detail, kSimBackgroundVoxelStyle_Varied, &model);
            for (int face = 0; face < model.face_count; face++)
              for (int vertex = 0; vertex < 4; vertex++) {
                SimBackgroundVoxelModelPoint p = model.faces[face].points[vertex];
                p.x += p.z * dx;
                p.y += p.z * dy;
                for (int edge = 0; edge < count; edge++) {
                  SimBackgroundVoxelModelPoint a = hull[edge], b = hull[(edge + 1) % count];
                  float ex = b.x - a.x, ey = b.y - a.y;
                  float distance = (ex * (p.y - a.y) - ey * (p.x - a.x)) / hypotf(ex, ey);
                  CHECK(distance > -.4f);
                }
              }
          }
        }
}

static void CheckReviewFollowup(void) {
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
    SimBackgroundVoxelModel model;
    SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_House, .town = 2,
        .development_level = 2};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(SurfaceFrontAt(&model, 1.3f, 6) < 0); /* No unsupported left extension. */
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House, .town = 4,
        .flags = kSimBackgroundVoxel_AlternateFacing};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) == 0);
    CHECK(model.max_z > 12 && model.max_z <= 12.81f);
    float crown_left = 100, crown_right = -100;
    int crown_bands = 0;
    for (int f = 0; f < model.face_count; f++) {
      const SimBackgroundVoxelModelFace *face = &model.faces[f];
      if (face->material == kSimVoxelMaterial_Trim && face->points[0].z > 9)
        crown_bands++;
      for (int v = 0; v < 4; v++)
        if (face->points[v].z > 11) {
          crown_left = fminf(crown_left, face->points[v].x);
          crown_right = fmaxf(crown_right, face->points[v].x);
        }
    }
    CHECK(crown_right - crown_left > 10); /* Broad crest survives even Low detail. */
    CHECK(crown_bands == 5);
    CHECK(SurfaceHeightAt(&model, 8, 8) > 11); /* Closed crest through the center. */
    for (int alternate = 0; alternate < 2; alternate++) {
      object.flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) == 0);
      CHECK(MaterialHasSlopedFace(&model, kSimVoxelMaterial_Roof));
      CHECK(SurfaceFrontAt(&model, 8, 5.6f) > SurfaceFrontAt(&model, 8, 2) + .3f);
      SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
      int count = SimBackgroundVoxelModel_Contacts(&object, contacts);
      CHECK(count == 8);
      for (int part = 0; part < count; part++) {
        float x = (contacts[part].x0 + contacts[part].x1) * .5f - 8;
        float y = (contacts[part].y0 + contacts[part].y1) * .5f - 8;
        CHECK(x*x + y*y < 5.7f*5.7f); /* Contacts follow the wall, not the eave. */
      }
    }
    object.town = 3;
    object.development_level = 1;
    for (int alternate = 0; alternate < 2; alternate++) {
      object.flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      int standing_walls = 0;
      for (int f = 0; f < model.face_count; f++) {
        const SimBackgroundVoxelModelFace *face = &model.faces[f];
        if (face->material != kSimVoxelMaterial_Wall && face->material != kSimVoxelMaterial_WallLight)
          continue;
        if (face->points[0].z == 0 && face->points[2].z == 5.7f &&
            face->points[0].x == face->points[2].x) standing_walls++;
      }
      CHECK(standing_walls == 4); /* Two cloth panels on each upright side. */
    }
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House, .town = 4,
        .development_level = 2, .flags = kSimBackgroundVoxel_AlternateFacing};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    int chimney_openings = 0;
    for (int f = 0; f < model.face_count; f++)
      if (model.faces[f].material == kSimVoxelMaterial_Dark && model.faces[f].points[0].z > 12) {
        for (int v = 1; v < 4; v++)
          CHECK(model.faces[f].points[v].z == model.faces[f].points[0].z);
        chimney_openings++;
      }
    CHECK(chimney_openings == 1); /* The flue survives solid-face cleanup. */
    object.town = 5;
    object.development_level = 1;
    for (int alternate = 0; alternate < 2; alternate++) {
      object.flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
      CHECK(SurfaceHeightAt(&model, 3, 16.1f) < SurfaceHeightAt(&model, 5, 16.1f));
      CHECK(SurfaceHeightAt(&model, 5, 16.1f) < SurfaceHeightAt(&model, 7, 16.1f));
      CHECK(fabsf(SurfaceHeightAt(&model, 8.5f, 16.1f) - 5.3f) < .001f);
    }
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_Windmill, .town = 1};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    float inner_min = 100, inner_max = -100, tip_min = 100, tip_max = -100;
    CHECK(MaterialFaces(&model, kSimVoxelMaterial_BladeStripe) >= 8);
    for (int f = 0; f < model.face_count; f++)
      if (model.faces[f].material == kSimVoxelMaterial_Blade)
        for (int v = 0; v < 4; v++) {
          SimBackgroundVoxelModelPoint p = model.faces[f].points[v];
          if (fabsf(p.x - 18.2f) < .001f) {inner_min = fminf(inner_min,p.z); inner_max = fmaxf(inner_max,p.z);}
          if (fabsf(p.x - 26) < .001f) {tip_min = fminf(tip_min,p.z); tip_max = fmaxf(tip_max,p.z);}
        }
    CHECK(tip_max - tip_min > 3 * (inner_max - inner_min));
    CHECK((tip_max + tip_min) * .5f > 21.5f); /* Swept rather than straight tips. */
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_Factory, .town = 1};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    int stacks = 0, dormers = 0;
    for (int f = 0; f < model.face_count; f++) {
      const SimBackgroundVoxelModelFace *face = &model.faces[f];
      if (face->material == kSimVoxelMaterial_Roof)
        CHECK(face->points[0].z != face->points[2].z || face->points[1].z != face->points[3].z);
      if (face->material != kSimVoxelMaterial_Dark) continue;
      if (face->points[0].z > 16) {
        for (int v = 0; v < 4; v++) CHECK(face->points[v].x > 24 && face->points[v].x < 28);
        stacks++;
      } else if (face->points[0].z >= 10) dormers++;
    }
    CHECK(stacks == 2 && dormers == 3);
    CHECK(fabsf(SurfaceFrontAt(&model, 20, 6.5f) - 31) < .001f);
    int walls = 0;
    for (int f = 0; f < model.face_count; f++) {
      const SimBackgroundVoxelModelFace *face = &model.faces[f];
      if (face->points[0].z == 0 && face->points[2].z == 9 && face->outward_winding) walls++;
    }
    CHECK(walls == 8); /* Exterior walls must survive occupancy-based culling. */
    object = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_StoryTree, .town = 6};
    SimBackgroundVoxelModel_BuildStyled(&object, detail, kSimBackgroundVoxelStyle_Varied, &model);
    CHECK(SurfaceHeightAt(&model, 16, 23) > 24); /* Central snow bulb fills the old fork gap. */
  }
}

static void CheckHousesMeetGroundWithoutSlabs(void) {
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0; level < kSimBackgroundDevelopmentLevelCount; level++)
      for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++)
        for (int style = 0; style < kSimBackgroundVoxelStyle_Count; style++)
          for (int alternate = 0; alternate < 2; alternate++)
            for (int stage = 0; stage < 3; stage++) {
              SimBackgroundVoxelObject object = {
                .kind = kSimBackgroundVoxel_House, .town = town,
                .development_level = level, .animation_phase = stage ? stage - 1 : 0,
                .flags = (alternate ? kSimBackgroundVoxel_AlternateFacing : 0) |
                    (stage ? kSimBackgroundVoxel_UnderConstruction : 0),
              };
              SimBackgroundVoxelModel model;
              SimBackgroundVoxelModel_BuildStyled(&object, detail, style, &model);
              CHECK(!model.overflow);
              /* A broad, low box is a pedestal. Narrow steps and the raised
               * floor of a stilt house are structural, and remain intact. */
              bool slab = false;
              for (int box = 0; box < model.box_count; box++) {
                const SimBackgroundVoxelModelBox *b = &model.boxes[box];
                slab |= b->z0 == 0 && b->z1 <= 2.1f &&
                    b->x1 - b->x0 > 4 && b->y1 - b->y0 > 4;
              }
              CHECK(!slab);
              CHECK(model.min_z == 0);
              const bool stilts = !stage &&
                  SimBackgroundVoxelRegion_ObjectHouseStyle(&object) ==
                      kSimBackgroundHouseStyle_MarahnaStilt;
              if (level > 0) {
                float wall_min, wall_max;
                MaterialZBounds(&model, kSimVoxelMaterial_Wall, &wall_min, &wall_max);
                if (stage != 1 || level != 1 || town != 3)
                  CHECK(wall_min == (stilts ? 5.3f : 0));
              }
              if (stilts) {
                SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
                CHECK(SimBackgroundVoxelModel_Contacts(&object, contacts) == 4);
                CHECK(SurfaceHeightAt(&model, 8, 12) >= 5.3f);
              }
            }
}

static void CheckAnimalPen(void) {
  const SimBackgroundVoxelObject object = {
      .kind = kSimBackgroundVoxel_AnimalPen, .town = 4,
      .source_cells_w = 2, .source_cells_h = 2,
      .footprint_cells_w = 2, .footprint_cells_d = 2,
  };
  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++)
    for (int style = 0; style < kSimBackgroundVoxelStyle_Count; style++) {
      SimBackgroundVoxelModel model;
      SimBackgroundVoxelModel_BuildStyled(&object, detail, style, &model);
      CHECK(!model.overflow && model.box_count == 0);
      CHECK(model.min_z == 0 && model.max_z == 2.0f);
      CHECK(model.min_x > 0 && model.max_x < 32);
      CHECK(model.min_y > 0 && model.max_y < 32);
      CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) == 22 * 8);
      CHECK(MaterialFaces(&model, kSimVoxelMaterial_Trim) == 22 * 3);
      CHECK(SurfaceHeightAt(&model, 16, 16) < 0); /* Exposed grass. */
      for (int column = 0; column < 8; column++) {
        float x = 1.5f + column * 4;
        CHECK(SurfaceHeightAt(&model, x, 4) == 2);
        /* The flat cap reaches the four cardinal edges, while every corner
         * of the old square cross-section is now outside the timber. */
        for (int sign = -1; sign <= 1; sign += 2) {
          CHECK(fabsf(SurfaceHeightAt(&model, x + sign * .6f, 4) - 2) < .0001f);
          CHECK(fabsf(SurfaceHeightAt(&model, x, 4 + sign * .6f) - 2) < .0001f);
          CHECK(SurfaceHeightAt(&model, x + sign * .6f, 4.6f) < 0);
          CHECK(SurfaceHeightAt(&model, x + sign * .6f, 3.4f) < 0);
        }
        CHECK(SurfaceHeightAt(&model, x, 28) ==
              (column == 4 || column == 5 ? -1 : 2));
        if (column < 7) CHECK(SurfaceHeightAt(&model, x + 2, 4) < 0);
      }
      for (int row = 0; row < 4; row++) {
        CHECK(SurfaceHeightAt(&model, 1.5f, 10 + row * 4) == 2);
        CHECK(SurfaceHeightAt(&model, 29.5f, 10 + row * 4) == 2);
      }
    }
  SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
  CHECK(SimBackgroundVoxelModel_Contacts(&object, contacts) == 0);
  CHECK(!SimBackgroundVoxelModel_CastsShadow(&object));
}

int main(void) {
  CheckAnimalPen();
  CheckHousesMeetGroundWithoutSlabs();
  CheckAuditRegressions();
  CheckRecognitionPolish();
  CheckEnvironmentModels();
  CheckFoliageShadows();
  CheckForestClusters();
  CheckReviewFollowup();
  CHECK(SimBackgroundVoxelModel_HeightBound(NULL, kSimBackgroundVoxelDetail_Ultra,
                                            kSimBackgroundVoxelStyle_Varied) == 0.0f);
  const SimBackgroundVoxelObject unknown = {.kind = UINT8_MAX};
  CHECK(SimBackgroundVoxelModel_HeightBound(&unknown, kSimBackgroundVoxelDetail_Ultra,
                                            kSimBackgroundVoxelStyle_Varied) == 0.0f);
  SimBackgroundVoxelModelBounds invalid = {1, 2, 3, 4, 5, 6};
  CHECK(!SimBackgroundVoxelModel_MeasureBounds(&unknown, kSimBackgroundVoxelDetail_Ultra,
                                               kSimBackgroundVoxelStyle_Varied, &invalid));
  const SimBackgroundVoxelModelBounds zero = {0};
  CHECK(!memcmp(&invalid, &zero, sizeof(zero)));
  CHECK(!SimBackgroundVoxelModel_MeasureBounds(&unknown, kSimBackgroundVoxelDetail_Ultra,
                                               kSimBackgroundVoxelStyle_Varied, NULL));
  SimBackgroundVoxelModel house =
      Build(kSimBackgroundVoxel_House, kSimBackgroundVoxelDetail_Balanced);
  CHECK(house.min_x >= 0.0f && house.max_x <= 16.0f);
  CHECK(house.min_y >= 0.0f && house.max_y <= 16.0f);
  CHECK(house.max_z == 15.6f); /* Native chimney rises above the gable. */
  CHECK(MaterialFaces(&house, kSimVoxelMaterial_Roof) > 0);
  CHECK(MaterialFaces(&house, kSimVoxelMaterial_Dark) > 0);

  /* The ROM selects eight architectural families through its exact 6x3
   * town/civilization table. Each town's three-stage progression must remain
   * visually distinct even where another town deliberately shares a family. */
  uint64_t progression_hash[kSimBackgroundTownCount][kSimBackgroundDevelopmentLevelCount];
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0; level < kSimBackgroundDevelopmentLevelCount; level++) {
      SimBackgroundVoxelModel regional = BuildRegionalHouse(town, level);
      progression_hash[town - 1][level] = ModelHash(&regional);
    }
  for (int town = 0; town < kSimBackgroundTownCount; town++) {
    CHECK(progression_hash[town][0] != progression_hash[town][1]);
    CHECK(progression_hash[town][1] != progression_hash[town][2]);
    CHECK(progression_hash[town][0] != progression_hash[town][2]);
  }
  /* Yurt and timber reuse in the source game is intentional, not a missing
   * regional override. */
  for (int town = 1; town < kSimBackgroundTownCount; town++)
    CHECK(progression_hash[0][0] == progression_hash[town][0]);
  CHECK(progression_hash[0][1] == progression_hash[1][1]);
  CHECK(progression_hash[0][1] == progression_hash[3][1]);
  CHECK(progression_hash[0][1] == progression_hash[5][1]);
  /* Straw huts have rounded reed walls and a thatched roof, while the canvas
   * family retains its standing pavilion walls and fabric roof. */
  SimBackgroundVoxelModel kasandora_yurt = BuildRegionalHouse(3, 0);
  SimBackgroundVoxelModel kasandora_tent = BuildRegionalHouse(3, 1);
  CHECK(kasandora_yurt.box_count == 0);
  CHECK(kasandora_tent.box_count == 0);
  CHECK(kasandora_yurt.min_z == 0 && kasandora_tent.min_z == 0);
  CHECK(MaterialHasSlopedFace(&kasandora_yurt, kSimVoxelMaterial_Wall));
  CHECK(MaterialHasSlopedFace(&kasandora_tent, kSimVoxelMaterial_Roof));
  CHECK(MaterialFaces(&kasandora_yurt, kSimVoxelMaterial_Dark) == 1);
  CHECK(MaterialFaces(&kasandora_tent, kSimVoxelMaterial_Dark) == 1);
  CHECK(kasandora_yurt.max_z > 14 && kasandora_yurt.max_z <= 14.61f);
  CHECK(kasandora_tent.max_z >= 11.4f && kasandora_tent.max_z <= 11.5f);
  CHECK(progression_hash[2][1] != progression_hash[0][0]);

  for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++)
    for (int style = 0; style < kSimBackgroundVoxelStyle_Count; style++)
      for (int alternate = 0; alternate < 2; alternate++) {
        uint64_t stage_hash[3][2];
        for (int family = 0; family < 3; family++)
          for (int phase = 0; phase < 2; phase++) {
            SimBackgroundVoxelObject shelter = {
                .kind = kSimBackgroundVoxel_House, .town = family == 1 ? 3 : 4,
                .development_level = family, .animation_phase = phase,
                .flags = kSimBackgroundVoxel_UnderConstruction |
                    (alternate ? kSimBackgroundVoxel_AlternateFacing : 0),
            };
            SimBackgroundVoxelModel stage;
            SimBackgroundVoxelModel_BuildStyled(&shelter, detail, style, &stage);
            CHECK(!stage.overflow);
            CHECK(stage.face_count <= stage.face_budget);
            CHECK(stage.max_z <= SimBackgroundVoxelRegion_AuthoredHeight(&shelter));
            CHECK(MaterialHasSlopedFace(&stage, kSimVoxelMaterial_Wood));
            if (family < 2) {
              CHECK(stage.box_count == 0);
              CHECK(MaterialFaces(&stage, kSimVoxelMaterial_Dark) == 0);
              CHECK((MaterialFaces(&stage, kSimVoxelMaterial_Wall) +
                     MaterialFaces(&stage, kSimVoxelMaterial_WallLight) +
                     MaterialFaces(&stage, kSimVoxelMaterial_RoofLight) > 0) == (phase == 1));
            }
            stage_hash[family][phase] = ModelHash(&stage);
          }
        for (int family = 0; family < 3; family++)
          CHECK(stage_hash[family][0] != stage_hash[family][1]);
        for (int phase = 0; phase < 2; phase++) {
          CHECK(stage_hash[0][phase] != stage_hash[1][phase]);
          CHECK(stage_hash[1][phase] != stage_hash[2][phase]);
        }
        /* Window inlays stay on the actual facade, including the side wing;
         * there must be no volumetric dark window boxes sticking through it. */
        for (int town = 3; town <= 6; town++)
          for (int level = 1; level < 3; level++) {
            if (town == 5 || (town == 3 && level == 1)) continue;
            SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_House,
                .town = town, .development_level = level,
                .flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0};
            SimBackgroundVoxelModel clean;
            SimBackgroundVoxelModel_BuildStyled(&object, detail, style, &clean);
            CHECK(!clean.overflow);
            for (int f = 0; f < clean.face_count; f++) {
              if (clean.faces[f].material != kSimVoxelMaterial_Dark) continue;
              if (town == 4 && level == 2 && alternate && clean.faces[f].points[0].z > 12) continue;
              for (int v = 1; v < 4; v++)
                CHECK(fabsf(clean.faces[f].points[v].y - clean.faces[f].points[0].y) < .001f);
            }
          }
      }

  /* Alternate-facing sprites are authored variants, not duplicates. Their
   * crown, canopy or roof-access details remain present at every LOD. */
  for (int town = 1; town <= 6; town++)
    for (int level = 0; level < 3; level++)
      for (int detail = 0; detail < kSimBackgroundVoxelDetail_Count; detail++) {
        SimBackgroundVoxelObject object = {.kind = kSimBackgroundVoxel_House,
            .town = town, .development_level = level};
        SimBackgroundVoxelModel front, alternate;
        SimBackgroundVoxelModel_Build(&object, detail, &front);
        object.flags = kSimBackgroundVoxel_AlternateFacing;
        SimBackgroundVoxelModel_Build(&object, detail, &alternate);
        CHECK(!front.overflow && !alternate.overflow);
        CHECK(ModelHash(&front) != ModelHash(&alternate));
        if ((town == 3 || town == 6) && level == 2) {
          CHECK(alternate.max_z > front.max_z);
          CHECK(alternate.max_z <= SimBackgroundVoxelRegion_AuthoredHeight(&object));
        }
        if (town == 3 && level == 1) {
          SimBackgroundVoxelModelContact contacts[kSimBackgroundVoxelModelMaxContacts];
          CHECK(SimBackgroundVoxelModel_Contacts(&object, contacts) == 4);
        }
      }

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
  for (int detail = kSimBackgroundVoxelDetail_Low; detail < kSimBackgroundVoxelDetail_Count;
       detail++) {
    for (int style = kSimBackgroundVoxelStyle_Basic; style < kSimBackgroundVoxelStyle_Count;
         style++) {
      SimBackgroundVoxelModel styled_aitos;
      SimBackgroundVoxelModel_BuildStyled(&aitos_house, (SimBackgroundVoxelDetail)detail,
                                          (SimBackgroundVoxelStyle)style, &styled_aitos);
      CHECK(HorizontalFaceCovers(&styled_aitos, 8.0f, 8.0f));
      CHECK(!MaterialHasSlopedFace(&styled_aitos, kSimVoxelMaterial_Roof));
      CHECK(!MaterialHasSlopedFace(&styled_aitos, kSimVoxelMaterial_RoofLight));
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
  SimBackgroundVoxelModel_Build(&alternate_house_object, kSimBackgroundVoxelDetail_Balanced,
                                &alternate_house);
  CHECK(!alternate_house.overflow);
  CHECK(alternate_house.face_count > house.face_count);
  CHECK(alternate_house.min_x >= 0.0f && alternate_house.max_x <= 16.0f);
  CHECK(alternate_house.min_y >= 0.0f && alternate_house.max_y <= 16.0f);
  CHECK(memcmp(alternate_house.faces, house.faces, house.face_count * sizeof(house.faces[0])) != 0);

  /* Optional seeded facade detail is transformed with an alternate-facing
   * house, but it cannot enlarge that house or alter its roofline. */
  SimBackgroundVoxelObject alternate_varied_object = {
      .kind = kSimBackgroundVoxel_House,
      .flags = kSimBackgroundVoxel_AlternateFacing,
      .record_slot = 1,
  };
  SimBackgroundVoxelModel alternate_architectural, alternate_varied;
  SimBackgroundVoxelModel_BuildStyled(&alternate_varied_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Architectural,
                                      &alternate_architectural);
  SimBackgroundVoxelModel_BuildStyled(&alternate_varied_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Varied, &alternate_varied);
  CHECK(SameBounds(&alternate_architectural, &alternate_varied));
  CHECK(ModelHash(&alternate_architectural) != ModelHash(&alternate_varied));

  SimBackgroundVoxelModel cathedral =
      Build(kSimBackgroundVoxel_Cathedral, kSimBackgroundVoxelDetail_Balanced);
  CHECK(cathedral.min_x >= 0.0f && cathedral.max_x <= 32.0f);
  CHECK(cathedral.min_y >= 0.0f && cathedral.max_y <= 32.0f);
  CHECK(cathedral.max_z == 24.0f);
  CHECK(cathedral.min_y >= 8.0f); /* protected rear land stays visually open */
  /* Surface compilation removes column caps buried in the facade/base. */
  CHECK(MaterialFaces(&cathedral, kSimVoxelMaterial_WallLight) >= 15);

  SimBackgroundVoxelObject cathedral_object = {
      .kind = kSimBackgroundVoxel_Cathedral,
      .record_slot = 2,
  };
  SimBackgroundVoxelModel decorated_cathedral;
  SimBackgroundVoxelModel_BuildStyled(&cathedral_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Trim, &decorated_cathedral);
  CHECK(!decorated_cathedral.overflow);
  CHECK(MaterialFaces(&decorated_cathedral, kSimVoxelMaterial_Gold) > 0);
  /* Relief must be mounted on, and contained by, the main triangular gable. */
  for (int f = 0; f < decorated_cathedral.face_count; f++) {
    const SimBackgroundVoxelModelFace *face = &decorated_cathedral.faces[f];
    if (face->material != kSimVoxelMaterial_Gold) continue;
    for (int p = 0; p < 4; p++) {
      SimBackgroundVoxelModelPoint at = face->points[p];
      CHECK(at.y >= 30.5f && at.y <= 30.55f);
      CHECK(at.z >= 16.0f && fabsf(at.x - 16.0f) <= (24.0f - at.z) * 1.75f);
    }
  }
  CHECK(RegionMaxZ(&decorated_cathedral, 2.0f, 24.0f, 9.0f, 31.5f) <= 16.4f);
  CHECK(RegionMaxZ(&decorated_cathedral, 23.0f, 24.0f, 30.0f, 31.5f) <= 16.4f);

  SimBackgroundVoxelModel windmill =
      Build(kSimBackgroundVoxel_Windmill, kSimBackgroundVoxelDetail_Balanced);
  CHECK(windmill.min_x >= 0.0f && windmill.max_x <= 32.0f);
  CHECK(windmill.min_y >= 0.0f && windmill.max_y <= 16.8f);
  CHECK(windmill.max_z <= 32.0f);
  CHECK(MaterialFaces(&windmill, kSimVoxelMaterial_Blade) >= 20 &&
        MaterialFaces(&windmill, kSimVoxelMaterial_BladeStripe) == 16);

  /* Only the sails and their attached purple seams occupy the swept rotor
   * volume. The tower and its trim stay behind it; the central hub may cover
   * the blade roots. */
  const float rotor_x = 16.0f, rotor_z = 21.0f;
  const float rotor_back = 15.8f; /* kWindmillBladePlane */
  const float hub_cap_radius = 3.0f;
  const float tip_radius = 11.2f; /* outer 10.0 plus the half width */
  for (int detail = kSimBackgroundVoxelDetail_Low; detail < kSimBackgroundVoxelDetail_Count;
       detail++) {
    SimBackgroundVoxelObject spinning = {
        .kind = kSimBackgroundVoxel_Windmill,
        .source_cells_w = 2,
        .source_cells_h = 2,
        .footprint_cells_w = 2,
        .footprint_cells_d = 1,
    };
    for (int style = kSimBackgroundVoxelStyle_Basic; style < kSimBackgroundVoxelStyle_Count;
         style++) {
      for (int phase = 0; phase < 3; phase++) {
        for (int slot = 0; slot < 2; slot++) {
          spinning.animation_phase = (uint8_t)phase;
          spinning.record_slot = (uint8_t)slot;
          SimBackgroundVoxelModel turning;
          SimBackgroundVoxelModel_BuildStyled(&spinning, (SimBackgroundVoxelDetail)detail,
                                              (SimBackgroundVoxelStyle)style, &turning);
          for (uint16_t face = 0; face < turning.face_count; face++) {
            if (turning.faces[face].material == kSimVoxelMaterial_Blade ||
                turning.faces[face].material == kSimVoxelMaterial_BladeStripe) continue;
            for (int point = 0; point < 4; point++) {
              const SimBackgroundVoxelModelPoint *at = &turning.faces[face].points[point];
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

  SimBackgroundVoxelModel factory =
      Build(kSimBackgroundVoxel_Factory, kSimBackgroundVoxelDetail_Balanced);
  CHECK(factory.min_x >= 0.0f && factory.max_x <= 32.0f);
  CHECK(factory.min_y >= 0.0f && factory.max_y <= 32.0f);
  CHECK(factory.max_z == 17.0f); /* low body plus sparse chimneys */
  CHECK(MaterialFaces(&factory, kSimVoxelMaterial_Roof) > 0);
  CHECK(MaterialFaces(&factory, kSimVoxelMaterial_WallLight) > 0);
  CHECK(SurfaceHeightAt(&factory, 10.0f, 6.0f) >= 12.0f);   /* upper U arm */
  CHECK(SurfaceHeightAt(&factory, 26.0f, 16.0f) >= 12.0f);  /* right spine */
  CHECK(SurfaceHeightAt(&factory, 10.0f, 26.0f) >= 12.0f);  /* lower U arm */
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
  SimBackgroundVoxelModel_BuildStyled(&styled_factory_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Trim, &trimmed_factory);
  SimBackgroundVoxelModel_BuildStyled(&styled_factory_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Architectural,
                                      &architectural_factory);
  SimBackgroundVoxelModel_BuildStyled(&styled_factory_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Varied, &varied_factory);
  SimBackgroundVoxelModel_BuildStyled(&styled_factory_object, kSimBackgroundVoxelDetail_High,
                                      kSimBackgroundVoxelStyle_Varied, &repeated_varied_factory);
  CHECK(!architectural_factory.overflow && !varied_factory.overflow);
  CHECK(architectural_factory.authored_face_count > architectural_factory.face_count);
  CHECK(architectural_factory.box_count > 0);
  /* The courtyard must expose the underlying biome tile at every style level;
   * architectural detail may add fixtures, but never a replacement floor. */
  CHECK(MaterialFaces(&trimmed_factory, kSimVoxelMaterial_Paving) == 0);
  CHECK(MaterialFaces(&architectural_factory, kSimVoxelMaterial_Paving) == 0);
  CHECK(architectural_factory.face_count > trimmed_factory.face_count);
  CHECK(varied_factory.face_count == architectural_factory.face_count);
  CHECK(ModelHash(&varied_factory) != ModelHash(&architectural_factory));
  CHECK(varied_factory.face_count == repeated_varied_factory.face_count);
  CHECK(memcmp(varied_factory.faces, repeated_varied_factory.faces,
               varied_factory.face_count * sizeof(varied_factory.faces[0])) == 0);
  int occluded_vertices = 0;
  for (uint16_t face = 0; face < varied_factory.face_count; face++)
    for (int point = 0; point < 4; point++)
      if (varied_factory.faces[face].occlusion[point] < 255) occluded_vertices++;
  CHECK(occluded_vertices > 0);

  SimBackgroundVoxelModel low_basic, low_varied;
  SimBackgroundVoxelModel_BuildStyled(&styled_factory_object, kSimBackgroundVoxelDetail_Low,
                                      kSimBackgroundVoxelStyle_Basic, &low_basic);
  SimBackgroundVoxelModel_BuildStyled(&styled_factory_object, kSimBackgroundVoxelDetail_Low,
                                      kSimBackgroundVoxelStyle_Varied, &low_varied);
  CHECK(low_basic.face_count == low_varied.face_count);

  SimBackgroundVoxelObject isolated_object = {
      .kind = kSimBackgroundVoxel_Tree,
      .flags = kSimBackgroundVoxel_IsolatedTree,
      .record_slot = kSimBackgroundVoxelNoRecordSlot,
  };
  SimBackgroundVoxelModel isolated;
  SimBackgroundVoxelModel_Build(&isolated_object, kSimBackgroundVoxelDetail_Balanced, &isolated);
  CHECK(!isolated.overflow && isolated.max_z == 18.0f);
  CHECK(isolated.min_x >= 0.0f && isolated.max_x <= 16.0f);
  CHECK(isolated.min_y >= 0.0f && isolated.max_y <= 16.0f);

  SimBackgroundVoxelObject interior_object = {
      .kind = kSimBackgroundVoxel_Tree,
      .tree_edges = kSimBackgroundTreeEdge_North | kSimBackgroundTreeEdge_East |
                    kSimBackgroundTreeEdge_South | kSimBackgroundTreeEdge_West,
      .record_slot = kSimBackgroundVoxelNoRecordSlot,
  };
  SimBackgroundVoxelModel interior;
  SimBackgroundVoxelModel_Build(&interior_object, kSimBackgroundVoxelDetail_Balanced, &interior);
  CHECK(!interior.overflow && interior.max_z == 18.0f);
  CHECK(ModelHash(&interior) != ModelHash(&isolated));
  CHECK(CanopyCoverage(&interior) > CanopyCoverage(&isolated) + 15);
  CHECK(interior.face_count <=
        SimBackgroundVoxelModel_FaceBudget(kSimBackgroundVoxelDetail_Balanced));

  SimBackgroundVoxelObject snow_tree_object = isolated_object;
  snow_tree_object.town = 6;
  SimBackgroundVoxelModel snow_tree;
  SimBackgroundVoxelModel_Build(&snow_tree_object, kSimBackgroundVoxelDetail_Balanced, &snow_tree);
  CHECK(!snow_tree.overflow && snow_tree.max_z == 19.0f);
  CHECK(ModelHash(&snow_tree) != ModelHash(&isolated));

  CheckPalmDetailContinuity();

  SimBackgroundVoxelObject palm_object = {
      .kind = kSimBackgroundVoxel_Palm,
      .town = 5,
      .cell_x = 4,
      .cell_y = 7,
      .group = 2,
      .record_slot = kSimBackgroundVoxelNoRecordSlot,
  };
  SimBackgroundVoxelModel palm;
  SimBackgroundVoxelModel_Build(&palm_object, kSimBackgroundVoxelDetail_Balanced, &palm);
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
  SimBackgroundVoxelModel_Build(&shrub_object, kSimBackgroundVoxelDetail_Balanced, &shrub);
  CHECK(!shrub.overflow && shrub.face_count > 0);
  CHECK(shrub.min_x >= 0.0f && shrub.max_x <= 16.0f);
  CHECK(shrub.min_y >= 0.0f && shrub.max_y <= 16.0f);
  /* The clearable bush is shorter and rounder than the permanent evergreen it
   * used to be drawn as, and shares no geometry with it. */
  CHECK(shrub.max_z < isolated.max_z);
  CHECK(ModelHash(&shrub) != ModelHash(&isolated));
  CHECK(MaterialFaces(&shrub, kSimVoxelMaterial_Leaves) > 0);

  /* Landmark bodies occupy a 2x2 plot; measured bounds include roof overhangs. */
  SimBackgroundVoxelModel story_tree =
      Build(kSimBackgroundVoxel_StoryTree, kSimBackgroundVoxelDetail_Balanced);
  CHECK(story_tree.min_x >= 0.0f && story_tree.max_x <= 32.0f);
  CHECK(story_tree.min_y >= 0.0f && story_tree.max_y <= 32.0f);
  CHECK(story_tree.max_z <= 30.0f);
  CHECK(MaterialFaces(&story_tree, kSimVoxelMaterial_Snow) > 0);
  CHECK(ModelHash(&story_tree) != ModelHash(&snow_tree));

  SimBackgroundVoxelModel bloodpool_castle =
      Build(kSimBackgroundVoxel_BloodpoolCastle, kSimBackgroundVoxelDetail_Balanced);
  CHECK(bloodpool_castle.min_x >= 0.0f && bloodpool_castle.max_x <= 32.0f);
  /* Grounded front shafts reach the plot edge; their closed caps overhang it.
   * MeasureBounds coverage above includes these roof tips in retained scenes. */
  CHECK(bloodpool_castle.min_y >= 0.0f && bloodpool_castle.max_y <= 33.2f);
  CHECK(bloodpool_castle.max_z <= 32.0f);
  CHECK(MaterialFaces(&bloodpool_castle, kSimVoxelMaterial_Gold) > 0);

  SimBackgroundVoxelModel marahna_temple =
      Build(kSimBackgroundVoxel_MarahnaTemple, kSimBackgroundVoxelDetail_Balanced);
  CHECK(marahna_temple.min_x >= 0.0f && marahna_temple.max_x <= 32.0f);
  CHECK(marahna_temple.min_y >= 0.0f && marahna_temple.max_y <= 32.0f);
  CHECK(marahna_temple.max_z <= 24.0f);
  CHECK(MaterialFaces(&marahna_temple, kSimVoxelMaterial_Gold) > 0);
  CHECK(ModelHash(&marahna_temple) != ModelHash(&bloodpool_castle));

  SimBackgroundVoxelModel pyramid =
      Build(kSimBackgroundVoxel_Pyramid, kSimBackgroundVoxelDetail_Balanced);
  CHECK(pyramid.min_x >= 0.0f && pyramid.max_x <= 32.0f);
  CHECK(pyramid.min_y >= 0.0f && pyramid.max_y <= 32.0f);
  CHECK(pyramid.max_z <= 28.0f);
  /* A pyramid is a pyramid: its top must be far narrower than its base. */
  CHECK(TopWidthAt(&pyramid, pyramid.max_z) < TopWidthAt(&pyramid, 0.0f) * 0.4f);
  CHECK(ModelHash(&pyramid) != ModelHash(&bloodpool_castle));

  SimBackgroundVoxelObject bridge_object = {
      .kind = kSimBackgroundVoxel_Bridge,
      .bridge_axis = kSimBackgroundBridgeAxis_EastWest,
      .bridge_bank_a_x = 5,
      .bridge_bank_b_x = 8,
  };
  SimBackgroundVoxelModel bridge;
  SimBackgroundVoxelModel_Build(&bridge_object, kSimBackgroundVoxelDetail_High, &bridge);
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
  MaterialZBounds(&bridge, kSimVoxelMaterial_Paving, &paving_min_z, &paving_max_z);
  MaterialZBounds(&bridge, kSimVoxelMaterial_WallLight, &rail_min_z, &rail_max_z);
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
      .cell_x = 17,
      .cell_y = 22,
      .bridge_axis = kSimBackgroundBridgeAxis_EastWest,
      .bridge_bank_a_x = 16,
      .bridge_bank_a_y = 22,
      .bridge_bank_b_x = 18,
      .bridge_bank_b_y = 22,
  };
  SimBackgroundVoxelObject bloodpool_north_south = {
      .kind = kSimBackgroundVoxel_Bridge,
      .cell_x = 18,
      .cell_y = 21,
      .bridge_axis = kSimBackgroundBridgeAxis_NorthSouth,
      .bridge_bank_a_x = 18,
      .bridge_bank_a_y = 20,
      .bridge_bank_b_x = 18,
      .bridge_bank_b_y = 22,
  };
  SimBackgroundBridgeBounds ew = SimBackgroundBridge_ResolveBounds(&bloodpool_east_west);
  SimBackgroundBridgeBounds ns = SimBackgroundBridge_ResolveBounds(&bloodpool_north_south);
  CHECK(ew.origin_x + ew.width < ns.origin_x);
  CHECK(ns.origin_y + ns.depth < ew.origin_y);

  /* Invalid bridge metadata fails closed instead of compiling a misleading
   * north-south fallback with arbitrary dimensions. */
  SimBackgroundVoxelObject invalid_bridge = {
      .kind = kSimBackgroundVoxel_Bridge,
      .bridge_axis = kSimBackgroundBridgeAxis_None,
  };
  SimBackgroundVoxelModel invalid_bridge_model;
  SimBackgroundVoxelModel_Build(&invalid_bridge, kSimBackgroundVoxelDetail_High,
                                &invalid_bridge_model);
  CHECK(invalid_bridge_model.face_count == 0);
  SimBackgroundBridgeBounds null_bounds = SimBackgroundBridge_ResolveBounds(NULL);
  CHECK(null_bounds.width == 0.0f && null_bounds.depth == 0.0f);

  SimBackgroundVoxelObject construction_object = {
      .kind = kSimBackgroundVoxel_House,
      .flags = kSimBackgroundVoxel_UnderConstruction,
  };
  SimBackgroundVoxelModel construction;
  SimBackgroundVoxelModel_Build(&construction_object, kSimBackgroundVoxelDetail_Balanced,
                                &construction);
  CHECK(!construction.overflow);
  /* Clean roof surfaces use fewer faces than the deliberately skeletal frame;
   * construction identity is material-based, not a face-count heuristic. */
  CHECK(construction.face_count != house.face_count);
  CHECK(MaterialFaces(&construction, kSimVoxelMaterial_Wood) > 0);

  /* Every family respects each performance target, and richer targets are
   * genuinely richer rather than four labels selecting the same geometry. */
  for (int kind = kSimBackgroundVoxel_House; kind < kSimBackgroundVoxelKindCount; kind++) {
    SimBackgroundVoxelModel low =
        Build((SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_Low);
    SimBackgroundVoxelModel balanced =
        Build((SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_Balanced);
    SimBackgroundVoxelModel high =
        Build((SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_High);
    SimBackgroundVoxelModel ultra =
        Build((SimBackgroundVoxelKind)kind, kSimBackgroundVoxelDetail_Ultra);
    CHECK(low.face_count <= balanced.face_count);
    CHECK(balanced.face_count <= high.face_count);
    CHECK(high.face_count <= ultra.face_count);
    /* The sparse native rocks already fit Low in full. Higher settings need
     * not invent extra stones or subdivide flat faces for a larger count. */
    if (kind == kSimBackgroundVoxel_Boulder || kind == kSimBackgroundVoxel_Rocks ||
        kind == kSimBackgroundVoxel_AnimalPen) {
      CHECK(SameBounds(&low, &ultra));
      continue;
    }
    CHECK(low.face_count < balanced.face_count);
    CHECK(balanced.face_count < high.face_count);
    CHECK(high.face_count < ultra.face_count);
  }

  /* Every density/style boundary is a real hard budget. This exercises more
   * than the representative fixtures above so a newly authored variant cannot
   * overflow only for one deterministic town coordinate. */
  for (int kind = kSimBackgroundVoxel_House; kind < kSimBackgroundVoxelKindCount; kind++) {
    for (int detail = kSimBackgroundVoxelDetail_Low; detail < kSimBackgroundVoxelDetail_Count;
         detail++) {
      for (int style = kSimBackgroundVoxelStyle_Basic; style < kSimBackgroundVoxelStyle_Count;
           style++) {
        for (int seed = 0; seed < 4; seed++) {
          SimBackgroundVoxelObject object = {
              .kind = (uint8_t)kind,
              .cell_x = (uint8_t)seed,
              .cell_y = (uint8_t)(seed * 3),
              .record_slot = (uint8_t)(seed + 1),
              .group = (uint8_t)(seed & 1),
          };
          SimBackgroundVoxelModel model;
          SimBackgroundVoxelModel_BuildStyled(&object, (SimBackgroundVoxelDetail)detail,
                                              (SimBackgroundVoxelStyle)style, &model);
          CHECK(!model.overflow);
          CHECK(model.face_budget ==
                SimBackgroundVoxelModel_ObjectFaceBudget(
                    &object, (SimBackgroundVoxelDetail)detail));
          CHECK(model.authored_face_count <= model.face_budget);
          CHECK(SimBackgroundVoxelModel_HeightBound(&object, (SimBackgroundVoxelDetail)detail,
                                                    (SimBackgroundVoxelStyle)style) >= model.max_z);
          CheckMeasuredBounds(&object, (SimBackgroundVoxelDetail)detail,
                              (SimBackgroundVoxelStyle)style, &model);
        }
      }
    }
  }

  /* Regional families must honor the same configurable quality boundaries as
   * the original Fillmore model. Exercise all 18 ROM-selected identities at
   * every density/style combination rather than validating only one town. */
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0; level < kSimBackgroundDevelopmentLevelCount; level++)
      for (int detail = kSimBackgroundVoxelDetail_Low; detail < kSimBackgroundVoxelDetail_Count;
           detail++)
        for (int style = kSimBackgroundVoxelStyle_Basic; style < kSimBackgroundVoxelStyle_Count;
             style++) {
          SimBackgroundVoxelObject object = {
              .kind = kSimBackgroundVoxel_House,
              .town = (uint8_t)town,
              .development_level = (uint8_t)level,
              .cell_x = (uint8_t)(town * 3),
              .cell_y = (uint8_t)(level * 5),
              .record_slot = (uint8_t)(town * 3 + level),
          };
          SimBackgroundVoxelModel regional;
          SimBackgroundVoxelModel_BuildStyled(&object, (SimBackgroundVoxelDetail)detail,
                                              (SimBackgroundVoxelStyle)style, &regional);
          CHECK(!regional.overflow && regional.face_count > 0);
          CHECK(regional.face_count <=
                SimBackgroundVoxelModel_FaceBudget((SimBackgroundVoxelDetail)detail));
          CHECK(SimBackgroundVoxelModel_HeightBound(&object, (SimBackgroundVoxelDetail)detail,
                                                    (SimBackgroundVoxelStyle)style) >=
                regional.max_z);
          CheckMeasuredBounds(&object, (SimBackgroundVoxelDetail)detail,
                              (SimBackgroundVoxelStyle)style, &regional);
        }

  /* Retained height bounds must survive LOD changes and animated/unfinished
   * variants without rebuilding on every windmill tick. */
  for (int kind = kSimBackgroundVoxel_House; kind <= kSimBackgroundVoxel_Factory; kind++)
    for (int flags = 0; flags <= kSimBackgroundVoxel_AlternateFacing; flags++)
      for (int style = kSimBackgroundVoxelStyle_Basic; style < kSimBackgroundVoxelStyle_Count;
           style++)
        for (int phase = 0; phase < 3; phase++) {
          SimBackgroundVoxelObject object = {
              .kind = (uint8_t)kind,
              .flags = (uint8_t)flags,
              .town = 6,
              .development_level = 2,
              .animation_phase = (uint8_t)phase,
          };
          const SimBackgroundVoxelObject captured = object;
          float prior_height = 0.0f;
          for (int detail = kSimBackgroundVoxelDetail_Low; detail < kSimBackgroundVoxelDetail_Count;
               detail++) {
            const float height = SimBackgroundVoxelModel_HeightBound(
                &object, (SimBackgroundVoxelDetail)detail, (SimBackgroundVoxelStyle)style);
            SimBackgroundVoxelModel model;
            SimBackgroundVoxelModel_BuildStyled(&object, (SimBackgroundVoxelDetail)detail,
                                                (SimBackgroundVoxelStyle)style, &model);
            CHECK(height >= prior_height && height >= model.max_z);
            CheckMeasuredBounds(&object, (SimBackgroundVoxelDetail)detail,
                                (SimBackgroundVoxelStyle)style, &model);
            prior_height = height;
            if (kind == kSimBackgroundVoxel_Windmill) {
              SimBackgroundVoxelObject other_pose = object;
              other_pose.animation_phase = (uint8_t)((phase + 1) % 3);
              CHECK(height == SimBackgroundVoxelModel_HeightBound(&other_pose,
                                                                  (SimBackgroundVoxelDetail)detail,
                                                                  (SimBackgroundVoxelStyle)style));
            }
          }
          CHECK(!memcmp(&object, &captured, sizeof(object)));
          CHECK(SimBackgroundVoxelModel_HeightBound(&object, (SimBackgroundVoxelDetail)-1,
                                                    (SimBackgroundVoxelStyle)-1) ==
                SimBackgroundVoxelModel_HeightBound(&object, kSimBackgroundVoxelDetail_High,
                                                    kSimBackgroundVoxelStyle_Varied));
        }

  /* Houses are the most numerous object in a developed town, so a regional
   * family that stops gaining geometry short of Ultra is where the quality
   * setting most visibly does nothing. Sixteen of the eighteen identities
   * were flat between High and Ultra. */
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0; level < kSimBackgroundDevelopmentLevelCount; level++) {
      SimBackgroundVoxelObject object = {
          .kind = kSimBackgroundVoxel_House,
          .town = (uint8_t)town,
          .development_level = (uint8_t)level,
          .cell_x = (uint8_t)(town * 3),
          .cell_y = (uint8_t)(level * 5),
          .record_slot = (uint8_t)(town * 3 + level),
      };
      uint16_t previous = 0;
      for (int detail = kSimBackgroundVoxelDetail_Low; detail < kSimBackgroundVoxelDetail_Count;
           detail++) {
        SimBackgroundVoxelModel step;
        SimBackgroundVoxelModel_BuildStyled(&object, (SimBackgroundVoxelDetail)detail,
                                            kSimBackgroundVoxelStyle_Varied, &step);
        CHECK(!step.overflow && step.face_count > previous);
        previous = step.face_count;
      }
    }

  /* Varied house styling is now facade-only. Across every regional identity,
   * High and Ultra may add visible surface detail but never a new footprint,
   * eave, ridge, or height beyond the Architectural model. */
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0; level < kSimBackgroundDevelopmentLevelCount; level++)
      for (int detail = kSimBackgroundVoxelDetail_High; detail <= kSimBackgroundVoxelDetail_Ultra;
           detail++) {
        SimBackgroundVoxelObject object = {
            .kind = kSimBackgroundVoxel_House,
            .town = (uint8_t)town,
            .development_level = (uint8_t)level,
            .cell_x = (uint8_t)(town * 3),
            .cell_y = (uint8_t)(level * 5),
            .record_slot = (uint8_t)(town * 3 + level),
        };
        SimBackgroundVoxelModel architectural, varied;
        SimBackgroundVoxelModel_BuildStyled(&object, (SimBackgroundVoxelDetail)detail,
                                            kSimBackgroundVoxelStyle_Architectural, &architectural);
        SimBackgroundVoxelModel_BuildStyled(&object, (SimBackgroundVoxelDetail)detail,
                                            kSimBackgroundVoxelStyle_Varied, &varied);
        CHECK(SameBounds(&architectural, &varied));
        CHECK(ModelHash(&architectural) != ModelHash(&varied));
      }

  CHECK(UniqueVariedModels(kSimBackgroundVoxel_House, kSimBackgroundVoxelDetail_High) >= 4);
  CHECK(UniqueVariedModels(kSimBackgroundVoxel_House, kSimBackgroundVoxelDetail_Ultra) >= 6);
  CHECK(UniqueVariedModels(kSimBackgroundVoxel_Factory, kSimBackgroundVoxelDetail_High) >= 3);
  CHECK(UniqueVariedModels(kSimBackgroundVoxel_Tree, kSimBackgroundVoxelDetail_High) >= 4);

  if (failures) {
    fprintf(stderr, "%d sim background voxel model checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel model checks passed");
  return 0;
}
