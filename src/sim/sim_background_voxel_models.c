#include "sim_background_voxel_models.h"

#include <float.h>
#include <string.h>

typedef enum ModelBoxFaces {
  kBoxFace_North = 1u << 0,
  kBoxFace_East = 1u << 1,
  kBoxFace_South = 1u << 2,
  kBoxFace_West = 1u << 3,
  kBoxFace_Top = 1u << 4,
  kBoxFace_AllVisible = kBoxFace_North | kBoxFace_East |
      kBoxFace_South | kBoxFace_West | kBoxFace_Top,
} ModelBoxFaces;

static SimBackgroundVoxelModelPoint Point(float x, float y, float z) {
  return (SimBackgroundVoxelModelPoint){x, y, z};
}

static void IncludePoint(SimBackgroundVoxelModel *model,
                         SimBackgroundVoxelModelPoint point) {
  if (point.x < model->min_x) model->min_x = point.x;
  if (point.y < model->min_y) model->min_y = point.y;
  if (point.z < model->min_z) model->min_z = point.z;
  if (point.x > model->max_x) model->max_x = point.x;
  if (point.y > model->max_y) model->max_y = point.y;
  if (point.z > model->max_z) model->max_z = point.z;
}

static void AddFace(SimBackgroundVoxelModel *model,
                    SimBackgroundVoxelMaterial material,
                    uint8_t brightness,
                    SimBackgroundVoxelModelPoint a,
                    SimBackgroundVoxelModelPoint b,
                    SimBackgroundVoxelModelPoint c,
                    SimBackgroundVoxelModelPoint d) {
  if (model->face_count >= model->face_budget ||
      model->face_count >= kSimBackgroundVoxelModelMaxFaces) {
    model->overflow = true;
    return;
  }
  SimBackgroundVoxelModelFace *face = &model->faces[model->face_count++];
  face->points[0] = a;
  face->points[1] = b;
  face->points[2] = c;
  face->points[3] = d;
  face->material = (uint8_t)material;
  face->brightness = brightness;
  for (int point = 0; point < 4; point++) face->occlusion[point] = 255;
  IncludePoint(model, a);
  IncludePoint(model, b);
  IncludePoint(model, c);
  IncludePoint(model, d);
}

uint16_t SimBackgroundVoxelModel_FaceBudget(
    SimBackgroundVoxelDetail detail) {
  switch (detail) {
    case kSimBackgroundVoxelDetail_Low: return 64;
    case kSimBackgroundVoxelDetail_Balanced: return 160;
    case kSimBackgroundVoxelDetail_High: return 256;
    case kSimBackgroundVoxelDetail_Ultra: return 384;
    case kSimBackgroundVoxelDetail_Count: break;
  }
  return 256;
}

static void AddBox(SimBackgroundVoxelModel *model,
                   float x0, float y0, float z0,
                   float x1, float y1, float z1,
                   SimBackgroundVoxelMaterial material,
                   uint8_t faces) {
  if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
  if (model->box_count >= kSimBackgroundVoxelModelMaxBoxes) {
    model->overflow = true;
    return;
  }
  model->boxes[model->box_count++] = (SimBackgroundVoxelModelBox){
    x0, y0, z0, x1, y1, z1,
  };
  if (faces & kBoxFace_Top)
    AddFace(model, material, 255,
            Point(x0, y0, z1), Point(x1, y0, z1),
            Point(x1, y1, z1), Point(x0, y1, z1));
  if (faces & kBoxFace_North)
    AddFace(model, material, 178,
            Point(x1, y0, z0), Point(x0, y0, z0),
            Point(x0, y0, z1), Point(x1, y0, z1));
  if (faces & kBoxFace_East)
    AddFace(model, material, 204,
            Point(x1, y1, z0), Point(x1, y0, z0),
            Point(x1, y0, z1), Point(x1, y1, z1));
  if (faces & kBoxFace_South)
    AddFace(model, material, 232,
            Point(x0, y1, z0), Point(x1, y1, z0),
            Point(x1, y1, z1), Point(x0, y1, z1));
  if (faces & kBoxFace_West)
    AddFace(model, material, 190,
            Point(x0, y0, z0), Point(x0, y1, z0),
            Point(x0, y1, z1), Point(x0, y0, z1));
}

static void AddStandardBox(SimBackgroundVoxelModel *model,
                           float x0, float y0, float z0,
                           float x1, float y1, float z1,
                           SimBackgroundVoxelMaterial material) {
  AddBox(model, x0, y0, z0, x1, y1, z1, material,
         kBoxFace_AllVisible);
}

static void BuildConstructionFrame(SimBackgroundVoxelModel *model,
                                   float width, float depth, float height) {
  AddStandardBox(model, 1.0f, 1.0f, 0.0f, width - 1.0f, depth - 1.0f,
                 1.5f, kSimVoxelMaterial_Trim);
  const float post = 1.5f;
  const float inset = 2.0f;
  AddStandardBox(model, inset, inset, 1.5f, inset + post, inset + post,
                 height, kSimVoxelMaterial_Wood);
  AddStandardBox(model, width - inset - post, inset, 1.5f,
                 width - inset, inset + post, height,
                 kSimVoxelMaterial_Wood);
  AddStandardBox(model, inset, depth - inset - post, 1.5f,
                 inset + post, depth - inset, height,
                 kSimVoxelMaterial_Wood);
  AddStandardBox(model, width - inset - post, depth - inset - post, 1.5f,
                 width - inset, depth - inset, height,
                 kSimVoxelMaterial_Wood);
  AddStandardBox(model, inset, depth - inset - 0.7f, height * 0.55f,
                 width - inset, depth - inset + 0.7f,
                 height * 0.55f + 1.2f, kSimVoxelMaterial_Wood);
  AddStandardBox(model, inset, inset - 0.7f, height - 1.2f,
                 width - inset, inset + 0.7f, height,
                 kSimVoxelMaterial_Wood);
}

static int DetailChoice(SimBackgroundVoxelDetail detail,
                        int low, int balanced, int high, int ultra) {
  switch (detail) {
    case kSimBackgroundVoxelDetail_Low: return low;
    case kSimBackgroundVoxelDetail_Balanced: return balanced;
    case kSimBackgroundVoxelDetail_High: return high;
    case kSimBackgroundVoxelDetail_Ultra: return ultra;
    case kSimBackgroundVoxelDetail_Count: break;
  }
  return high;
}

static void AddGableRoofX(SimBackgroundVoxelModel *model,
                          float x0, float x1, float y0, float y1,
                          float eave_z, float ridge_z,
                          SimBackgroundVoxelMaterial roof,
                          SimBackgroundVoxelMaterial gable) {
  float ridge_x = (x0 + x1) * 0.5f;
  AddFace(model, roof, 218,
          Point(x0, y0, eave_z), Point(ridge_x, y0, ridge_z),
          Point(ridge_x, y1, ridge_z), Point(x0, y1, eave_z));
  AddFace(model, roof, 248,
          Point(ridge_x, y0, ridge_z), Point(x1, y0, eave_z),
          Point(x1, y1, eave_z), Point(ridge_x, y1, ridge_z));
  AddFace(model, gable, 232,
          Point(x0, y1, eave_z), Point(x1, y1, eave_z),
          Point(ridge_x, y1, ridge_z), Point(ridge_x, y1, ridge_z));
  AddFace(model, gable, 178,
          Point(x1, y0, eave_z), Point(x0, y0, eave_z),
          Point(ridge_x, y0, ridge_z), Point(ridge_x, y0, ridge_z));
}

static void AddGableRoofY(SimBackgroundVoxelModel *model,
                          float x0, float x1, float y0, float y1,
                          float eave_z, float ridge_z,
                          SimBackgroundVoxelMaterial roof,
                          SimBackgroundVoxelMaterial gable) {
  float ridge_y = (y0 + y1) * 0.5f;
  AddFace(model, roof, 190,
          Point(x0, y0, eave_z), Point(x1, y0, eave_z),
          Point(x1, ridge_y, ridge_z), Point(x0, ridge_y, ridge_z));
  AddFace(model, roof, 242,
          Point(x0, ridge_y, ridge_z), Point(x1, ridge_y, ridge_z),
          Point(x1, y1, eave_z), Point(x0, y1, eave_z));
  AddFace(model, gable, 204,
          Point(x1, y0, eave_z), Point(x1, y1, eave_z),
          Point(x1, ridge_y, ridge_z), Point(x1, ridge_y, ridge_z));
  AddFace(model, gable, 190,
          Point(x0, y1, eave_z), Point(x0, y0, eave_z),
          Point(x0, ridge_y, ridge_z), Point(x0, ridge_y, ridge_z));
}

static void AddShedRoofX(SimBackgroundVoxelModel *model,
                         float x0, float x1, float y0, float y1,
                         float low_z, float high_z,
                         SimBackgroundVoxelMaterial roof,
                         SimBackgroundVoxelMaterial gable) {
  AddFace(model, roof, 232,
          Point(x0, y0, low_z), Point(x1, y0, high_z),
          Point(x1, y1, high_z), Point(x0, y1, low_z));
  AddFace(model, gable, 232,
          Point(x0, y1, low_z), Point(x1, y1, low_z),
          Point(x1, y1, high_z), Point(x1, y1, high_z));
  AddFace(model, gable, 178,
          Point(x1, y0, high_z), Point(x0, y0, low_z),
          Point(x1, y0, low_z), Point(x1, y0, high_z));
}

static void BuildHouse(const SimBackgroundVoxelObject *object,
                       SimBackgroundVoxelDetail detail,
                       SimBackgroundVoxelModel *model) {
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    AddStandardBox(model, 2.0f, 3.0f, 1.5f, 14.0f, 14.0f, 6.5f,
                   kSimVoxelMaterial_Wall);
    BuildConstructionFrame(model, 16.0f, 16.0f, 12.0f);
    return;
  }

  AddStandardBox(model, 1.5f, 2.0f, 0.0f, 14.5f, 15.0f, 2.0f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 2.5f, 3.0f, 2.0f, 13.5f, 14.5f, 10.0f,
                 kSimVoxelMaterial_Wall);
  /* A four-pixel rise keeps the gable recognizable without letting the roof
   * dominate the finished house. The older six-pixel rise was the remaining
   * source of the too-tall silhouette even after presentation scaling. */
  AddGableRoofX(model, 1.0f, 15.0f, 2.0f, 15.0f, 10.0f, 14.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 7.0f, 14.1f, 2.0f, 10.0f, 15.3f, 7.5f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;

  AddStandardBox(model, 3.7f, 14.1f, 5.0f, 6.2f, 15.2f, 8.0f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 10.8f, 14.1f, 5.0f, 13.3f, 15.2f, 8.0f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 2.5f, 14.0f, 9.0f, 13.5f, 15.0f, 10.0f,
                 kSimVoxelMaterial_Trim);

  if (detail >= kSimBackgroundVoxelDetail_High) {
    /* Raised window and door frames are large enough to survive the authentic
     * 256-pixel viewport while still reading as individual voxel pieces. */
    static const float windows[][2] = {{3.7f, 6.2f}, {10.8f, 13.3f}};
    for (int i = 0; i < 2; i++) {
      float x0 = windows[i][0], x1 = windows[i][1];
      AddStandardBox(model, x0 - 0.4f, 14.0f, 4.6f,
                     x0, 15.4f, 8.4f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x1, 14.0f, 4.6f,
                     x1 + 0.4f, 15.4f, 8.4f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x0 - 0.4f, 14.0f, 8.0f,
                     x1 + 0.4f, 15.4f, 8.4f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x0 - 0.4f, 14.0f, 4.6f,
                     x1 + 0.4f, 15.4f, 5.0f, kSimVoxelMaterial_Trim);
    }
    AddStandardBox(model, 6.5f, 14.0f, 2.0f, 7.0f, 15.5f, 8.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 10.0f, 14.0f, 2.0f, 10.5f, 15.5f, 8.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 6.5f, 14.0f, 7.5f, 10.5f, 15.5f, 8.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 7.0f, 3.0f, 14.0f, 9.0f, 14.0f, 15.0f,
                   kSimVoxelMaterial_RoofLight);
  }

  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int block = 0; block < 4; block++) {
      float x0 = 2.0f + block * 3.0f;
      AddStandardBox(model, x0, 14.4f, 0.5f, x0 + 2.2f, 15.4f, 2.1f,
                     block & 1 ? kSimVoxelMaterial_WallLight
                               : kSimVoxelMaterial_Trim);
    }
    for (int rib = 0; rib < 4; rib++) {
      float y = 3.5f + rib * 2.5f;
      AddStandardBox(model, 6.8f, y, 14.7f, 9.2f, y + 0.45f, 15.2f,
                     kSimVoxelMaterial_Trim);
    }
  }
}

static void BuildCathedral(SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  /* The full 2x2 scene footprint is protected land, but the visible temple is
   * intentionally compact within it. The rear clearance is what prevents the
   * protected footprint from reading as one oversized building slab. */
  AddStandardBox(model, 0.5f, 8.5f, 0.0f, 31.5f, 31.5f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 2.0f, 10.0f, 1.5f, 30.0f, 30.0f, 3.0f,
                 kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 3.5f, 11.0f, 3.0f, 28.5f, 29.0f, 16.0f,
                 kSimVoxelMaterial_Wall);

  /* The first 16 pixels are the full-height lower mass. The second source
   * layer is compressed to eight pixels for perspective correction. */
  AddGableRoofX(model, 2.0f, 30.0f, 9.5f, 30.5f, 16.0f, 24.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);

  int columns = DetailChoice(detail, 3, 5, 6, 7);
  for (int i = 0; i < columns; i++) {
    float x = columns == 1 ? 15.0f : 4.0f + 22.0f * i / (columns - 1);
    AddStandardBox(model, x, 28.8f, 4.0f,
                   x + 2.0f, 31.0f, 15.7f,
                   kSimVoxelMaterial_WallLight);
    if (detail != kSimBackgroundVoxelDetail_Low) {
      AddStandardBox(model, x - 0.4f, 28.5f, 3.0f,
                     x + 2.4f, 31.3f, 4.3f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x - 0.4f, 28.5f, 15.0f,
                     x + 2.4f, 31.3f, 16.3f, kSimVoxelMaterial_Trim);
    }
  }
  AddStandardBox(model, 14.0f, 29.2f, 3.0f, 18.0f, 31.5f, 12.5f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 0.0f, 29.5f, 0.0f, 32.0f, 32.0f, 0.8f,
                 kSimVoxelMaterial_WallLight);

  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int side = 0; side < 2; side++) {
      float x0 = side ? 28.0f : 1.5f;
      AddStandardBox(model, x0, 12.0f, 2.0f, x0 + 2.5f, 16.0f, 14.0f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, x0, 21.0f, 2.0f, x0 + 2.5f, 25.0f, 14.0f,
                     kSimVoxelMaterial_Trim);
    }
    AddStandardBox(model, 13.3f, 29.0f, 2.5f, 14.0f, 31.7f, 13.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 18.0f, 29.0f, 2.5f, 18.7f, 31.7f, 13.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 13.3f, 29.0f, 12.5f, 18.7f, 31.7f, 13.2f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 3.0f, 30.0f, 0.8f, 29.0f, 32.0f, 1.5f,
                   kSimVoxelMaterial_Trim);
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int panel = 0; panel < 6; panel++) {
      float x = 4.5f + panel * 4.0f;
      if (x > 13.0f && x < 19.0f) continue;
      AddStandardBox(model, x, 29.1f, 6.0f, x + 1.8f, 30.2f, 10.0f,
                     kSimVoxelMaterial_Dark);
    }
    AddStandardBox(model, 15.2f, 18.0f, 21.0f, 16.8f, 20.0f, 24.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 14.0f, 18.0f, 22.0f, 18.0f, 20.0f, 23.2f,
                   kSimVoxelMaterial_Trim);
  }
}

static void AddBlade(SimBackgroundVoxelModel *model,
                     float cx, float cy, float cz,
                     float dx, float dz) {
  const float inner = 2.2f, outer = 10.0f, half_width = 1.15f;
  const float px = -dz, pz = dx;
  SimBackgroundVoxelModelPoint front[4] = {
    Point(cx + dx * inner + px * half_width, cy + 0.6f,
          cz + dz * inner + pz * half_width),
    Point(cx + dx * outer + px * half_width, cy + 0.6f,
          cz + dz * outer + pz * half_width),
    Point(cx + dx * outer - px * half_width, cy + 0.6f,
          cz + dz * outer - pz * half_width),
    Point(cx + dx * inner - px * half_width, cy + 0.6f,
          cz + dz * inner - pz * half_width),
  };
  SimBackgroundVoxelModelPoint back[4];
  for (int i = 0; i < 4; i++) {
    back[i] = front[i];
    back[i].y = cy;
  }
  AddFace(model, kSimVoxelMaterial_Blade, 255,
          front[0], front[1], front[2], front[3]);
  AddFace(model, kSimVoxelMaterial_Blade, 190,
          back[3], back[2], back[1], back[0]);
  AddFace(model, kSimVoxelMaterial_Blade, 215,
          back[0], back[1], front[1], front[0]);
  AddFace(model, kSimVoxelMaterial_Blade, 215,
          back[2], back[3], front[3], front[2]);
  AddFace(model, kSimVoxelMaterial_Blade, 232,
          back[1], back[2], front[2], front[1]);
}

static void AddBladeInlay(SimBackgroundVoxelModel *model,
                          float cx, float cy, float cz,
                          float dx, float dz) {
  const float inner = 3.2f, outer = 9.2f, half_width = 0.28f;
  const float px = -dz, pz = dx;
  AddFace(model, kSimVoxelMaterial_Wood, 255,
          Point(cx + dx * inner + px * half_width, cy + 0.64f,
                cz + dz * inner + pz * half_width),
          Point(cx + dx * outer + px * half_width, cy + 0.64f,
                cz + dz * outer + pz * half_width),
          Point(cx + dx * outer - px * half_width, cy + 0.64f,
                cz + dz * outer - pz * half_width),
          Point(cx + dx * inner - px * half_width, cy + 0.64f,
                cz + dz * inner - pz * half_width));
}

static void BuildWindmill(const SimBackgroundVoxelObject *object,
                          SimBackgroundVoxelDetail detail,
                          SimBackgroundVoxelModel *model) {
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    AddStandardBox(model, 8.0f, 3.0f, 0.0f, 24.0f, 15.0f, 12.0f,
                   kSimVoxelMaterial_Wall);
    BuildConstructionFrame(model, 32.0f, 16.0f, 24.0f);
    return;
  }

  AddStandardBox(model, 6.0f, 2.0f, 0.0f, 26.0f, 15.0f, 2.0f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 8.0f, 3.0f, 2.0f, 24.0f, 14.5f, 18.0f,
                 kSimVoxelMaterial_Wall);
  AddStandardBox(model, 9.5f, 3.5f, 18.0f, 22.5f, 14.0f, 22.0f,
                 kSimVoxelMaterial_WallLight);
  AddGableRoofX(model, 6.5f, 25.5f, 2.0f, 15.0f, 22.0f, 30.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 13.5f, 14.0f, 2.0f, 18.5f, 15.5f, 9.0f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 14.0f, 14.1f, 18.0f, 18.0f, 15.2f, 22.0f,
                 kSimVoxelMaterial_Wood);

  const float diagonal = 0.70710678f;
  const bool alternate = (object->record_slot & 1u) != 0;
  const float dx = alternate ? 1.0f : diagonal;
  const float dz = alternate ? 0.0f : diagonal;
  AddBlade(model, 16.0f, 15.0f, 21.0f, dx, dz);
  AddBlade(model, 16.0f, 15.0f, 21.0f, -dz, dx);
  AddBlade(model, 16.0f, 15.0f, 21.0f, -dx, -dz);
  AddBlade(model, 16.0f, 15.0f, 21.0f, dz, -dx);
  AddStandardBox(model, 14.3f, 14.6f, 19.3f, 17.7f, 16.0f, 22.7f,
                 kSimVoxelMaterial_Wood);

  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddBladeInlay(model, 16.0f, 15.0f, 21.0f, dx, dz);
    AddBladeInlay(model, 16.0f, 15.0f, 21.0f, -dz, dx);
    AddBladeInlay(model, 16.0f, 15.0f, 21.0f, -dx, -dz);
    AddBladeInlay(model, 16.0f, 15.0f, 21.0f, dz, -dx);
    AddStandardBox(model, 9.5f, 14.0f, 10.0f, 12.5f, 15.2f, 14.0f,
                   kSimVoxelMaterial_Dark);
    AddStandardBox(model, 19.5f, 14.0f, 10.0f, 22.5f, 15.2f, 14.0f,
                   kSimVoxelMaterial_Dark);
    AddStandardBox(model, 14.5f, 3.0f, 30.0f, 17.5f, 14.0f, 31.0f,
                   kSimVoxelMaterial_RoofLight);
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int level = 0; level < 4; level++) {
      float z = 4.0f + level * 3.5f;
      AddStandardBox(model, 7.5f, 13.8f, z, 9.0f, 15.0f, z + 1.0f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 23.0f, 13.8f, z, 24.5f, 15.0f, z + 1.0f,
                     kSimVoxelMaterial_Trim);
    }
  }
}

static void BuildFactory(const SimBackgroundVoxelObject *object,
                         SimBackgroundVoxelDetail detail,
                         SimBackgroundVoxelModel *model) {
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    AddStandardBox(model, 1.0f, 1.0f, 0.0f, 31.0f, 31.0f, 3.0f,
                   kSimVoxelMaterial_Wall);
    BuildConstructionFrame(model, 32.0f, 32.0f, 10.0f);
    return;
  }

  /* Keep both horizontal arms architecturally consistent. The open centre and
   * connecting right spine establish the sideways-U without making the near
   * arm look like an unrelated, shorter building. */
  AddStandardBox(model, 0.5f, 0.5f, 0.0f, 22.0f, 9.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 0.5f, 23.0f, 0.0f, 22.0f, 31.5f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 21.0f, 0.5f, 0.0f, 31.5f, 31.5f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 1.5f, 1.5f, 1.5f, 22.0f, 8.5f, 9.0f,
                 kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 1.5f, 23.5f, 1.5f, 22.0f, 30.5f, 9.0f,
                 kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 21.0f, 1.5f, 1.5f, 30.5f, 30.5f, 9.0f,
                 kSimVoxelMaterial_Wall);
  AddStandardBox(model, 20.8f, 1.0f, 8.7f, 31.0f, 31.0f, 9.5f,
                 kSimVoxelMaterial_Roof);
  AddGableRoofY(model, 0.8f, 22.2f, 0.8f, 9.2f, 9.0f, 12.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddGableRoofY(model, 0.8f, 22.2f, 22.8f, 31.2f, 9.0f, 12.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);

  static const float chimney_xy[][2] = {
    {4.0f, 5.0f}, {4.0f, 24.0f},
    {16.0f, 5.0f}, {16.0f, 24.0f},
  };
  int chimneys = DetailChoice(detail, 1, 2, 4, 4);
  for (int i = 0; i < chimneys; i++) {
    float x = chimney_xy[i][0], y = chimney_xy[i][1];
    float base_z = 10.0f;
    AddStandardBox(model, x, y, base_z, x + 3.0f, y + 3.0f,
                   base_z + 6.0f,
                   kSimVoxelMaterial_Metal);
    AddStandardBox(model, x - 0.5f, y - 0.5f, base_z + 5.0f,
                   x + 3.5f, y + 3.5f, base_z + 7.0f,
                   kSimVoxelMaterial_Dark);
  }
  /* Public loading bays on the front arm and another bay opening into the
   * courtyard make the missing centre legible at the gameplay camera. */
  AddStandardBox(model, 13.0f, 30.0f, 1.5f, 19.0f, 32.0f, 6.2f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 4.0f, 30.0f, 3.0f, 8.0f, 31.5f, 5.8f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 24.0f, 30.0f, 3.5f, 28.0f, 31.5f, 6.5f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 8.0f, 8.2f, 2.5f, 15.0f, 9.5f, 7.2f,
                 kSimVoxelMaterial_Dark);

  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int i = 0; i < 3; i++) {
      float x = 3.0f + i * 10.0f;
      AddStandardBox(model, x, 29.8f, 3.0f, x + 0.5f, 31.8f, 7.0f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, x + 5.0f, 29.8f, 3.0f, x + 5.5f, 31.8f, 7.0f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, x, 29.8f, 6.5f, x + 5.5f, 31.8f, 7.0f,
                     kSimVoxelMaterial_Trim);
    }
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int vent = 0; vent < 3; vent++) {
      float y = 12.0f + vent * 2.6f;
      AddStandardBox(model, 21.0f, y, 3.0f, 22.2f, y + 1.4f, 6.5f,
                     kSimVoxelMaterial_Metal);
    }
  }
}

enum { kTreeCrownMaxResolution = 9 };

static bool TreeCrownVoxel(int x, int y, int z, int resolution,
                           uint32_t seed) {
  float nx = ((x + 0.5f) * 2.0f / resolution) - 1.0f;
  float ny = ((y + 0.5f) * 2.0f / resolution) - 1.0f;
  float height = (z + 0.5f) / resolution;
  uint32_t profile = seed % 4u;
  if (profile == 1u) nx -= height > 0.35f ? 0.10f : -0.04f;
  if (profile == 2u) ny += height > 0.55f ? 0.12f : -0.05f;
  float radius = 1.02f - height * (profile == 3u ? 0.96f : 0.90f);
  if (profile == 2u && height < 0.38f) radius += 0.08f;
  /* A small flare at branch-tier boundaries keeps the evergreen readable as
   * layered voxel foliage while preserving one unmistakable pointed crown. */
  int tier = resolution >= 7 ? 3 : 2;
  int tier_offset = (int)(profile & 1u);
  if (z + 1 < resolution && (z + tier_offset) % tier == 0)
    radius += profile == 3u ? 0.13f : 0.08f;
  if (z + 1 == resolution)
    return x == resolution / 2 && y == resolution / 2;
  return nx * nx + ny * ny <= radius * radius;
}

static SimBackgroundVoxelMaterial TreeCrownMaterial(
    uint32_t seed, int x, int y, int z, int resolution,
    SimBackgroundVoxelDetail detail) {
  if (z * 4 < resolution)
    return kSimVoxelMaterial_LeavesDark;
  if (z * 3 >= resolution * 2)
    return kSimVoxelMaterial_LeavesLight;
  if (detail >= kSimBackgroundVoxelDetail_High) {
    uint32_t patch = seed ^ (uint32_t)x * 0x9E37u ^
        (uint32_t)y * 0x7F4Au ^ (uint32_t)z * 0x45D9u;
    if (patch % 13u == 0)
      return kSimVoxelMaterial_LeavesLight;
    if (patch % 17u == 0)
      return kSimVoxelMaterial_LeavesDark;
  }
  return kSimVoxelMaterial_Leaves;
}

static void BuildTreeCrown(SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model,
                           uint32_t seed, float offset_x, float offset_y) {
  bool occupied[kTreeCrownMaxResolution][kTreeCrownMaxResolution]
               [kTreeCrownMaxResolution] = {{{false}}};
  int resolution = DetailChoice(detail, 3, 5, 7, 9);
  for (int z = 0; z < resolution; z++)
    for (int y = 0; y < resolution; y++)
      for (int x = 0; x < resolution; x++)
        occupied[x][y][z] = TreeCrownVoxel(
            x, y, z, resolution, seed);

  const float crown_x0 = 1.0f + offset_x;
  const float crown_y0 = 1.0f + offset_y;
  const float crown_z0 = 3.5f;
  const float voxel_xy = 14.0f / resolution;
  const float voxel_z = 11.5f / resolution;
  for (int z = 0; z < resolution; z++)
    for (int y = 0; y < resolution; y++)
      for (int x = 0; x < resolution; x++) {
        if (!occupied[x][y][z]) continue;
        uint8_t faces = 0;
        if (y == 0 || !occupied[x][y - 1][z]) faces |= kBoxFace_North;
        if (x + 1 == resolution || !occupied[x + 1][y][z])
          faces |= kBoxFace_East;
        if (y + 1 == resolution || !occupied[x][y + 1][z])
          faces |= kBoxFace_South;
        if (x == 0 || !occupied[x - 1][y][z]) faces |= kBoxFace_West;
        if (z + 1 == resolution || !occupied[x][y][z + 1])
          faces |= kBoxFace_Top;
        AddBox(model,
               crown_x0 + x * voxel_xy,
               crown_y0 + y * voxel_xy,
               crown_z0 + z * voxel_z,
               crown_x0 + (x + 1) * voxel_xy,
               crown_y0 + (y + 1) * voxel_xy,
               crown_z0 + (z + 1) * voxel_z,
               TreeCrownMaterial(seed, x, y, z, resolution, detail), faces);
      }

  /* Neighbour metadata is deliberately not turned into geometry. Earlier
   * connector cuboids made only some sides of a tree sprout square foliage
   * bars, especially at the edge of a forest. Adjacent pointed crowns may
   * overlap naturally after projection without changing either silhouette. */
}

static void BuildTree(const SimBackgroundVoxelObject *object,
                      SimBackgroundVoxelDetail detail,
                      SimBackgroundVoxelModel *model) {
  uint32_t seed = (uint32_t)object->cell_x * 0x45D9F3Bu ^
      (uint32_t)object->cell_y * 0x119DE1F3u ^
      (uint32_t)object->group * 0x3449u;
  float offset_x = ((int)(seed & 3u) - 1.5f) * 0.18f;
  float offset_y = ((int)((seed >> 2) & 3u) - 1.5f) * 0.18f;

  /* Every tree is an authored object, including forest interiors. A compact
   * tapered trunk supports a pointed, tiered evergreen crown with internal
   * faces removed. The permanent forest therefore stays visually distinct
   * from the round destructible shrubs without spending fill-rate on hidden
   * cubes. */
  AddStandardBox(model, 6.3f, 6.3f, 0.0f, 9.7f, 9.7f, 4.2f,
                 kSimVoxelMaterial_Trunk);
  AddStandardBox(model, 7.0f, 7.0f, 3.4f, 9.0f, 9.0f, 5.5f,
                 kSimVoxelMaterial_Trunk);
  BuildTreeCrown(detail, model, seed, offset_x, offset_y);
}

static void RecomputeModelBounds(SimBackgroundVoxelModel *model) {
  model->min_x = model->min_y = model->min_z = FLT_MAX;
  model->max_x = model->max_y = model->max_z = -FLT_MAX;
  for (uint16_t face = 0; face < model->face_count; face++)
    for (int point = 0; point < 4; point++)
      IncludePoint(model, model->faces[face].points[point]);
}

static bool NearlyEqual(float a, float b) {
  float difference = a - b;
  return difference > -0.0001f && difference < 0.0001f;
}

typedef struct AxisFace {
  int axis;
  float normal;
  float plane;
  float u0, u1, v0, v1;
} AxisFace;

static bool GetAxisFace(const SimBackgroundVoxelModelFace *face,
                        AxisFace *out) {
  const SimBackgroundVoxelModelPoint *a = &face->points[0];
  const SimBackgroundVoxelModelPoint *b = &face->points[1];
  const SimBackgroundVoxelModelPoint *d = &face->points[3];
  float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
  float vx = d->x - a->x, vy = d->y - a->y, vz = d->z - a->z;
  float normal[3] = {
    uy * vz - uz * vy,
    uz * vx - ux * vz,
    ux * vy - uy * vx,
  };
  int axis = 0;
  if (normal[1] * normal[1] > normal[axis] * normal[axis]) axis = 1;
  if (normal[2] * normal[2] > normal[axis] * normal[axis]) axis = 2;
  float length = normal[axis] < 0.0f ? -normal[axis] : normal[axis];
  if (length < 0.0001f) return false;
  for (int other = 0; other < 3; other++) {
    if (other == axis) continue;
    float component = normal[other] < 0.0f ? -normal[other] : normal[other];
    if (component > length * 0.001f) return false;
  }
  /* Match the authored AddBox winding correction used by lighting. */
  float outward = normal[axis] > 0.0f ? 1.0f : -1.0f;
  if (axis != 2 || outward < 0.0f) outward = -outward;
  float coordinate[4][3];
  for (int point = 0; point < 4; point++) {
    coordinate[point][0] = face->points[point].x;
    coordinate[point][1] = face->points[point].y;
    coordinate[point][2] = face->points[point].z;
  }
  int u_axis = axis == 0 ? 1 : 0;
  int v_axis = axis == 2 ? 1 : 2;
  if (axis == 1) v_axis = 2;
  *out = (AxisFace){
    .axis = axis,
    .normal = outward,
    .plane = coordinate[0][axis],
    .u0 = coordinate[0][u_axis], .u1 = coordinate[0][u_axis],
    .v0 = coordinate[0][v_axis], .v1 = coordinate[0][v_axis],
  };
  for (int point = 1; point < 4; point++) {
    float u = coordinate[point][u_axis];
    float v = coordinate[point][v_axis];
    if (u < out->u0) out->u0 = u;
    if (u > out->u1) out->u1 = u;
    if (v < out->v0) out->v0 = v;
    if (v > out->v1) out->v1 = v;
  }
  return true;
}

static bool PointInsideBox(const SimBackgroundVoxelModelBox *box,
                           float x, float y, float z) {
  const float epsilon = 0.0001f;
  return x > box->x0 + epsilon && x < box->x1 - epsilon &&
      y > box->y0 + epsilon && y < box->y1 - epsilon &&
      z > box->z0 + epsilon && z < box->z1 - epsilon;
}

static bool PointInsideAnyBox(const SimBackgroundVoxelModel *model,
                              float x, float y, float z) {
  for (uint16_t box = 0; box < model->box_count; box++)
    if (PointInsideBox(&model->boxes[box], x, y, z)) return true;
  return false;
}

static bool FaceIsBuried(const SimBackgroundVoxelModel *model,
                         const SimBackgroundVoxelModelFace *face) {
  AxisFace axis_face;
  if (!GetAxisFace(face, &axis_face)) return false;
  SimBackgroundVoxelModelPoint center = {0.0f, 0.0f, 0.0f};
  for (int point = 0; point < 4; point++) {
    center.x += face->points[point].x * 0.25f;
    center.y += face->points[point].y * 0.25f;
    center.z += face->points[point].z * 0.25f;
  }
  const float normal_step = 0.015f;
  for (int sample = 0; sample < 5; sample++) {
    SimBackgroundVoxelModelPoint point = sample == 4
        ? center : face->points[sample];
    if (sample < 4) {
      /* Keep corner samples away from exact solid boundaries. */
      point.x += (center.x - point.x) * 0.002f;
      point.y += (center.y - point.y) * 0.002f;
      point.z += (center.z - point.z) * 0.002f;
    }
    float *coordinate[3] = {&point.x, &point.y, &point.z};
    *coordinate[axis_face.axis] += axis_face.normal * normal_step;
    if (!PointInsideAnyBox(model, point.x, point.y, point.z)) return false;
  }
  return true;
}

static bool FacesAreDuplicate(const SimBackgroundVoxelModelFace *a,
                              const SimBackgroundVoxelModelFace *b) {
  if (a->material != b->material) return false;
  AxisFace left, right;
  if (!GetAxisFace(a, &left) || !GetAxisFace(b, &right)) return false;
  return left.axis == right.axis && NearlyEqual(left.normal, right.normal) &&
      NearlyEqual(left.plane, right.plane) &&
      NearlyEqual(left.u0, right.u0) && NearlyEqual(left.u1, right.u1) &&
      NearlyEqual(left.v0, right.v0) && NearlyEqual(left.v1, right.v1);
}

static void RemoveBuriedAndDuplicateFaces(SimBackgroundVoxelModel *model) {
  uint16_t write = 0;
  for (uint16_t face = 0; face < model->face_count; face++) {
    if (FaceIsBuried(model, &model->faces[face])) continue;
    bool duplicate = false;
    for (uint16_t prior = 0; prior < write; prior++)
      if (FacesAreDuplicate(&model->faces[prior], &model->faces[face])) {
        duplicate = true;
        break;
      }
    if (!duplicate) model->faces[write++] = model->faces[face];
  }
  model->face_count = write;
}

static void ComputeCornerOcclusion(SimBackgroundVoxelModel *model) {
  const float normal_step = 0.02f;
  const float tangent_step = 0.04f;
  static const uint8_t visibility[] = {255, 226, 204, 178};
  for (uint16_t face_index = 0; face_index < model->face_count; face_index++) {
    SimBackgroundVoxelModelFace *face = &model->faces[face_index];
    AxisFace axis_face;
    if (!GetAxisFace(face, &axis_face)) continue;
    int tangent[2], at = 0;
    for (int axis = 0; axis < 3; axis++)
      if (axis != axis_face.axis) tangent[at++] = axis;
    float center[3] = {0.0f, 0.0f, 0.0f};
    for (int point = 0; point < 4; point++) {
      center[0] += face->points[point].x * 0.25f;
      center[1] += face->points[point].y * 0.25f;
      center[2] += face->points[point].z * 0.25f;
    }
    for (int point = 0; point < 4; point++) {
      float origin[3] = {
        face->points[point].x,
        face->points[point].y,
        face->points[point].z,
      };
      origin[axis_face.axis] += axis_face.normal * normal_step;
      float direction[2] = {
        origin[tangent[0]] < center[tangent[0]] ? -1.0f : 1.0f,
        origin[tangent[1]] < center[tangent[1]] ? -1.0f : 1.0f,
      };
      float side_a[3] = {origin[0], origin[1], origin[2]};
      float side_b[3] = {origin[0], origin[1], origin[2]};
      float corner[3] = {origin[0], origin[1], origin[2]};
      side_a[tangent[0]] += direction[0] * tangent_step;
      side_b[tangent[1]] += direction[1] * tangent_step;
      corner[tangent[0]] += direction[0] * tangent_step;
      corner[tangent[1]] += direction[1] * tangent_step;
      bool occupied_a = PointInsideAnyBox(
          model, side_a[0], side_a[1], side_a[2]);
      bool occupied_b = PointInsideAnyBox(
          model, side_b[0], side_b[1], side_b[2]);
      bool occupied_corner = PointInsideAnyBox(
          model, corner[0], corner[1], corner[2]);
      int occlusion = occupied_a && occupied_b
          ? 3 : (int)occupied_a + (int)occupied_b + (int)occupied_corner;
      face->occlusion[point] = visibility[occlusion];
    }
  }
}

static void FinalizeModelSurface(SimBackgroundVoxelModel *model) {
  model->authored_face_count = model->face_count;
  RemoveBuriedAndDuplicateFaces(model);
  ComputeCornerOcclusion(model);
  RecomputeModelBounds(model);
}

static void BuildAlternateFacingHouse(SimBackgroundVoxelDetail detail,
                                      SimBackgroundVoxelModel *model) {
  /* The authentic alternate is not a construction frame or a bare 90-degree
   * rotation. Its finished main gable remains readable while a lower side
   * mass reveals the other perspective. Compress and shift the authored main
   * house, then use the freed footprint for that completed side wing. */
  for (uint16_t face = 0; face < model->face_count; face++)
    for (int point = 0; point < 4; point++)
      model->faces[face].points[point].x =
          3.8f + model->faces[face].points[point].x * 0.76f;
  for (uint16_t box = 0; box < model->box_count; box++) {
    model->boxes[box].x0 = 3.8f + model->boxes[box].x0 * 0.76f;
    model->boxes[box].x1 = 3.8f + model->boxes[box].x1 * 0.76f;
  }
  RecomputeModelBounds(model);

  AddStandardBox(model, 0.7f, 4.0f, 0.0f, 6.4f, 15.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 1.1f, 5.0f, 1.5f, 6.0f, 14.3f, 6.5f,
                 kSimVoxelMaterial_WallLight);
  AddShedRoofX(model, 0.6f, 6.6f, 4.2f, 14.8f, 6.5f, 8.5f,
               kSimVoxelMaterial_RoofLight,
               kSimVoxelMaterial_WallLight);
  if (detail != kSimBackgroundVoxelDetail_Low) {
    AddStandardBox(model, 2.2f, 14.0f, 2.8f, 4.6f, 15.1f, 5.5f,
                   kSimVoxelMaterial_Dark);
    AddStandardBox(model, 1.8f, 13.9f, 5.3f, 5.0f, 15.2f, 5.8f,
                   kSimVoxelMaterial_Trim);
  }
}

static void AddCathedralFacadeDecorations(
    SimBackgroundVoxelModel *model,
    SimBackgroundVoxelDetail detail) {
  /* A shallow winged relief recalls the gold eagle in the original Fillmore
   * cathedral art. It sits just in front of the centre gable, parallel to the
   * facade, so it actually presents toward the gameplay camera. Broad pieces
   * survive native resolution better than literal one-voxel feather detail. */
  const float crest_y0 = 31.2f, crest_y1 = 31.8f;
  AddStandardBox(model, 15.3f, crest_y0, 17.2f,
                 16.7f, crest_y1, 20.2f,
                 kSimVoxelMaterial_Gold);
  AddStandardBox(model, 12.0f, crest_y0, 18.0f,
                 15.3f, crest_y1, 18.9f,
                 kSimVoxelMaterial_Gold);
  AddStandardBox(model, 16.7f, crest_y0, 18.0f,
                 20.0f, crest_y1, 18.9f,
                 kSimVoxelMaterial_Gold);
  if (detail < kSimBackgroundVoxelDetail_High) return;

  /* A single broad diamond reads as a rose window at authentic resolution.
   * It is a facade quad rather than a protruding cube, so it cannot recreate
   * the square rooftop cap that the simplified silhouette deliberately lost. */
  const float facade_y = 31.82f;
  const SimBackgroundVoxelModelPoint outer[] = {
    {16.0f, facade_y, 13.1f}, {18.4f, facade_y, 15.5f},
    {16.0f, facade_y, 17.9f}, {13.6f, facade_y, 15.5f},
  };
  const SimBackgroundVoxelModelPoint inner[] = {
    {16.0f, facade_y + 0.01f, 13.8f},
    {17.7f, facade_y + 0.01f, 15.5f},
    {16.0f, facade_y + 0.01f, 17.2f},
    {14.3f, facade_y + 0.01f, 15.5f},
  };
  AddFace(model, kSimVoxelMaterial_Glass, 255,
          inner[0], inner[1], inner[2], inner[3]);
  if (detail == kSimBackgroundVoxelDetail_Ultra)
    for (int edge = 0; edge < 4; edge++) {
      int next = (edge + 1) & 3;
      AddFace(model, kSimVoxelMaterial_Gold, 255,
              outer[edge], outer[next], inner[next], inner[edge]);
    }

  AddStandardBox(model, 11.5f, crest_y0, 18.7f,
                 13.2f, crest_y1, 19.5f,
                 kSimVoxelMaterial_Gold);
  AddStandardBox(model, 18.8f, crest_y0, 18.7f,
                 20.5f, crest_y1, 19.5f,
                 kSimVoxelMaterial_Gold);

  AddStandardBox(model, 10.0f, 31.0f, 15.6f,
                 22.0f, 31.8f, 16.4f,
                 kSimVoxelMaterial_Gold);

  /* Gold doorway jambs and lintel add a second, lower accent without coating
   * the pale masonry or turning every facade edge into bright noise. */
  AddStandardBox(model, 13.4f, 31.0f, 2.8f,
                 14.2f, 31.8f, 12.8f,
                 kSimVoxelMaterial_Gold);
  AddStandardBox(model, 17.8f, 31.0f, 2.8f,
                 18.6f, 31.8f, 12.8f,
                 kSimVoxelMaterial_Gold);
  AddStandardBox(model, 13.4f, 31.0f, 12.0f,
                 18.6f, 31.8f, 12.8f,
                 kSimVoxelMaterial_Gold);
}

static void BuildSilhouetteTrim(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *model) {
  if (detail == kSimBackgroundVoxelDetail_Low ||
      (object->flags & kSimBackgroundVoxel_UnderConstruction))
    return;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      if (object->flags & kSimBackgroundVoxel_AlternateFacing) {
        AddStandardBox(model, 0.4f, 14.3f, 6.1f, 6.8f, 15.6f, 6.8f,
                       kSimVoxelMaterial_Trim);
        AddStandardBox(model, 7.1f, 14.2f, 9.5f, 14.2f, 15.6f, 10.2f,
                       kSimVoxelMaterial_Trim);
      } else {
        /* Broad fascia pieces produce a stable eave line at authentic output
         * resolution; tiny roof-edge cubes only reintroduce pixel noise. */
        AddStandardBox(model, 0.9f, 14.2f, 9.5f, 15.1f, 15.7f, 10.2f,
                       kSimVoxelMaterial_Trim);
        AddStandardBox(model, 0.8f, 2.0f, 9.5f, 1.6f, 15.2f, 10.2f,
                       kSimVoxelMaterial_Trim);
        AddStandardBox(model, 14.4f, 2.0f, 9.5f, 15.2f, 15.2f, 10.2f,
                       kSimVoxelMaterial_Trim);
      }
      break;
    case kSimBackgroundVoxel_Cathedral:
      AddStandardBox(model, 2.2f, 28.6f, 15.2f, 29.8f, 31.2f, 16.2f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 1.8f, 10.0f, 15.1f, 3.0f, 29.5f, 16.1f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 29.0f, 10.0f, 15.1f, 30.2f, 29.5f, 16.1f,
                     kSimVoxelMaterial_Trim);
      /* Keep the side roof planes uninterrupted. Earlier tower experiments
       * left either pointed caps or square blocks sitting on the slope; both
       * fought the simple cathedral silhouette approved from the source art. */
      AddGableRoofX(model, 10.0f, 22.0f, 27.5f, 31.2f,
                    16.2f, 20.5f,
                    kSimVoxelMaterial_RoofLight,
                    kSimVoxelMaterial_WallLight);
      AddCathedralFacadeDecorations(model, detail);
      break;
    case kSimBackgroundVoxel_Windmill:
      AddStandardBox(model, 7.0f, 13.8f, 17.4f, 25.0f, 15.4f, 18.3f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 7.4f, 13.8f, 2.0f, 8.5f, 15.2f, 18.0f,
                     kSimVoxelMaterial_Wood);
      AddStandardBox(model, 23.5f, 13.8f, 2.0f, 24.6f, 15.2f, 18.0f,
                     kSimVoxelMaterial_Wood);
      AddStandardBox(model, 6.4f, 13.0f, 16.8f, 25.6f, 15.8f, 17.5f,
                     kSimVoxelMaterial_Wood);
      AddStandardBox(model, 9.0f, 14.0f, 14.0f, 10.0f, 15.5f, 17.2f,
                     kSimVoxelMaterial_Wood);
      AddStandardBox(model, 22.0f, 14.0f, 14.0f, 23.0f, 15.5f, 17.2f,
                     kSimVoxelMaterial_Wood);
      if (detail >= kSimBackgroundVoxelDetail_High)
        AddStandardBox(model, 10.0f, 15.25f, 10.2f,
                       13.0f, 15.65f, 13.8f,
                       kSimVoxelMaterial_Glass);
      break;
    case kSimBackgroundVoxel_Factory:
      AddStandardBox(model, 1.0f, 7.8f, 8.5f, 21.2f, 9.3f, 9.4f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 1.0f, 29.7f, 8.5f, 21.2f, 31.7f, 9.4f,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 20.8f, 10.0f, 8.6f, 22.4f, 22.5f, 9.4f,
                     kSimVoxelMaterial_Trim);
      if (detail >= kSimBackgroundVoxelDetail_High) {
        AddStandardBox(model, 5.0f, 30.9f, 3.4f,
                       7.2f, 31.85f, 5.6f,
                       kSimVoxelMaterial_Glass);
        AddStandardBox(model, 9.0f, 30.9f, 3.4f,
                       11.2f, 31.85f, 5.6f,
                       kSimVoxelMaterial_Glass);
      }
      break;
    case kSimBackgroundVoxel_Tree:
      AddStandardBox(model, 5.7f, 6.8f, 0.0f, 10.3f, 9.2f, 1.0f,
                     kSimVoxelMaterial_Trunk);
      AddStandardBox(model, 6.8f, 5.7f, 0.0f, 9.2f, 10.3f, 1.0f,
                     kSimVoxelMaterial_Trunk);
      break;
  }
}

static void BuildFactoryCourtyardDetails(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *model) {
  if (detail == kSimBackgroundVoxelDetail_Low ||
      object->kind != kSimBackgroundVoxel_Factory ||
      (object->flags & kSimBackgroundVoxel_UnderConstruction))
    return;
  /* Leave the courtyard floor open so the same biome ground used to erase the
   * source sprite remains visible through the sideways-U. Sparse fixtures can
   * add detail at higher style settings without replacing the terrain. */
  AddStandardBox(model, 20.4f, 13.5f, 1.3f, 21.8f, 18.5f, 7.2f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 3.2f, 13.0f, 0.3f, 6.5f, 16.2f, 2.8f,
                 kSimVoxelMaterial_Wood);
  AddStandardBox(model, 7.0f, 16.8f, 0.3f, 10.0f, 19.7f, 2.1f,
                 kSimVoxelMaterial_Metal);
}

static uint32_t ObjectStyleSeed(const SimBackgroundVoxelObject *object) {
  return (uint32_t)object->cell_x * 0x9E3779B1u ^
      (uint32_t)object->cell_y * 0x85EBCA77u ^
      (uint32_t)object->record_slot * 0xC2B2AE3Du ^
      (uint32_t)object->group * 0x27D4EB2Fu;
}

static void BuildDeterministicVariation(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *model) {
  if (detail < kSimBackgroundVoxelDetail_High ||
      (object->flags & kSimBackgroundVoxel_UnderConstruction))
    return;
  uint32_t seed = ObjectStyleSeed(object);
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House: {
      uint32_t variant_count =
          detail == kSimBackgroundVoxelDetail_Ultra ? 6u : 4u;
      uint32_t variant = seed % variant_count;
      if (variant == 0u) {
        float x = seed & 2u ? 3.0f : 11.0f;
        AddStandardBox(model, x, 5.0f, 11.5f, x + 1.8f, 7.0f, 15.2f,
                       kSimVoxelMaterial_Dark);
        AddStandardBox(model, x - 0.3f, 4.7f, 14.6f,
                       x + 2.1f, 7.3f, 15.6f,
                       kSimVoxelMaterial_Trim);
      } else if (variant == 1u) {
        /* Broad porch canopy: visible at native resolution, but much calmer
         * than a scatter of decorative facade cubes. */
        AddStandardBox(model, 6.3f, 15.0f, 7.3f, 10.7f, 16.0f, 8.1f,
                       kSimVoxelMaterial_Roof);
        AddStandardBox(model, 6.6f, 15.1f, 1.2f, 7.2f, 15.8f, 7.4f,
                       kSimVoxelMaterial_Wood);
        AddStandardBox(model, 9.8f, 15.1f, 1.2f, 10.4f, 15.8f, 7.4f,
                       kSimVoxelMaterial_Wood);
      } else if (variant == 2u) {
        /* One compact dormer changes the roof silhouette without restoring
         * the noisy high-frequency roof stepping removed by the compiler. */
        AddStandardBox(model, 6.2f, 10.8f, 10.2f, 9.8f, 14.5f, 12.2f,
                       kSimVoxelMaterial_WallLight);
        AddGableRoofX(model, 5.7f, 10.3f, 10.3f, 14.8f, 12.2f, 13.8f,
                      kSimVoxelMaterial_RoofLight,
                      kSimVoxelMaterial_WallLight);
      } else if (variant == 3u) {
        /* A broad front bay changes the footprint/facade rhythm while staying
         * lower than the main eave and within the house's authored cell. */
        AddStandardBox(model, 1.8f, 13.8f, 1.5f, 5.4f, 15.7f, 6.7f,
                       kSimVoxelMaterial_WallLight);
        AddShedRoofX(model, 1.3f, 5.9f, 13.3f, 15.9f, 6.7f, 7.7f,
                     kSimVoxelMaterial_RoofLight,
                     kSimVoxelMaterial_WallLight);
      } else if (variant == 4u) {
        for (int dormer = 0; dormer < 2; dormer++) {
          float x0 = dormer ? 9.3f : 3.0f;
          AddStandardBox(model, x0, 10.8f, 10.1f,
                         x0 + 3.0f, 14.3f, 11.8f,
                         kSimVoxelMaterial_WallLight);
          AddGableRoofX(model, x0 - 0.35f, x0 + 3.35f,
                        10.4f, 14.7f, 11.8f, 13.1f,
                        kSimVoxelMaterial_RoofLight,
                        kSimVoxelMaterial_WallLight);
        }
      } else {
        AddStandardBox(model, 2.8f, 5.0f, 11.5f,
                       4.6f, 7.0f, 15.0f,
                       kSimVoxelMaterial_Dark);
        AddStandardBox(model, 5.8f, 15.0f, 7.2f,
                       11.2f, 16.0f, 8.1f,
                       kSimVoxelMaterial_Roof);
      }
      break;
    }
    case kSimBackgroundVoxel_Factory: {
      uint32_t variant = seed % 3u;
      if (variant == 0u) {
        AddStandardBox(model, 24.5f, 13.0f, 9.0f, 28.5f, 17.0f, 12.0f,
                       kSimVoxelMaterial_Metal);
        AddStandardBox(model, 25.2f, 13.7f, 12.0f, 27.8f, 16.3f, 13.0f,
                       kSimVoxelMaterial_Trim);
      } else if (variant == 1u) {
        AddStandardBox(model, 11.0f, 4.0f, 9.0f, 15.0f, 8.0f, 11.2f,
                       kSimVoxelMaterial_Metal);
      } else {
        AddStandardBox(model, 3.0f, 29.8f, 6.0f,
                       11.0f, 32.0f, 7.0f,
                       kSimVoxelMaterial_RoofLight);
        AddStandardBox(model, 4.0f, 30.0f, 1.5f,
                       4.8f, 31.8f, 6.2f,
                       kSimVoxelMaterial_Metal);
        AddStandardBox(model, 9.2f, 30.0f, 1.5f,
                       10.0f, 31.8f, 6.2f,
                       kSimVoxelMaterial_Metal);
      }
      break;
    }
    case kSimBackgroundVoxel_Tree:
      /* Seeded crown profiles already vary the outline. Appending cuboids at
       * this stage produced detached branch blocks on only a few sides. */
      break;
    case kSimBackgroundVoxel_Cathedral:
    case kSimBackgroundVoxel_Windmill:
      /* Unique town landmarks do not need random silhouettes. */
      break;
  }
}

void SimBackgroundVoxelModel_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *out) {
  SimBackgroundVoxelModel_BuildStyled(
      object, detail, kSimBackgroundVoxelStyle_Basic, out);
}

void SimBackgroundVoxelModel_BuildStyled(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    SimBackgroundVoxelModel *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (detail < kSimBackgroundVoxelDetail_Low ||
      detail >= kSimBackgroundVoxelDetail_Count)
    detail = kSimBackgroundVoxelDetail_High;
  if (style < kSimBackgroundVoxelStyle_Basic ||
      style >= kSimBackgroundVoxelStyle_Count)
    style = kSimBackgroundVoxelStyle_Varied;
  out->face_budget = SimBackgroundVoxelModel_FaceBudget(detail);
  out->min_x = out->min_y = out->min_z = FLT_MAX;
  out->max_x = out->max_y = out->max_z = -FLT_MAX;
  if (!object) return;

  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      BuildHouse(object, detail, out);
      /* Alternate-facing houses compress and shift the complete main mass.
       * Build seeded dormers, bays and porches first so their trim follows the
       * same transform as the door and windows. Appending a porch afterwards
       * left its canopy and posts visibly offset to the left of the opening. */
      if (style >= kSimBackgroundVoxelStyle_Varied)
        BuildDeterministicVariation(object, detail, out);
      if (object->flags & kSimBackgroundVoxel_AlternateFacing)
        BuildAlternateFacingHouse(detail, out);
      break;
    case kSimBackgroundVoxel_Cathedral:
      BuildCathedral(detail, out);
      break;
    case kSimBackgroundVoxel_Windmill:
      BuildWindmill(object, detail, out);
      break;
    case kSimBackgroundVoxel_Factory:
      BuildFactory(object, detail, out);
      break;
    case kSimBackgroundVoxel_Tree:
      BuildTree(object, detail, out);
      break;
  }
  if (style >= kSimBackgroundVoxelStyle_Trim)
    BuildSilhouetteTrim(object, detail, out);
  if (style >= kSimBackgroundVoxelStyle_Architectural)
    BuildFactoryCourtyardDetails(object, detail, out);
  if (style >= kSimBackgroundVoxelStyle_Varied &&
      object->kind != kSimBackgroundVoxel_House)
    BuildDeterministicVariation(object, detail, out);
  FinalizeModelSurface(out);
}
