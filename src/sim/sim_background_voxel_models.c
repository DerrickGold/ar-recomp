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
  int roof_steps = DetailChoice(detail, 2, 4, 6, 8);
  /* A four-pixel rise keeps the gable recognizable without letting the roof
   * dominate the finished house. The older six-pixel rise was the remaining
   * source of the too-tall silhouette even after presentation scaling. */
  float roof_step_height = 4.0f / roof_steps;
  for (int step = 0; step < roof_steps; step++) {
    float progress = roof_steps == 1 ? 0.0f : (float)step / (roof_steps - 1);
    float inset = 1.0f + progress * 6.0f;
    AddStandardBox(model, inset, 2.0f + progress * 1.5f,
                   10.0f + step * roof_step_height,
                   16.0f - inset, 15.0f - progress * 1.5f,
                   10.0f + (step + 1) * roof_step_height,
                   step >= roof_steps / 2
                       ? kSimVoxelMaterial_RoofLight
                       : kSimVoxelMaterial_Roof);
  }
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
    AddStandardBox(model, 11.0f, 5.0f, 11.5f, 13.0f, 7.0f, 15.5f,
                   kSimVoxelMaterial_Dark);
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
  int roof_steps = DetailChoice(detail, 3, 5, 8, 10);
  float roof_step_height = 8.0f / roof_steps;
  for (int step = 0; step < roof_steps; step++) {
    float progress = roof_steps == 1 ? 0.0f : (float)step / (roof_steps - 1);
    float inset = 1.5f + progress * 13.0f;
    /* A gable narrows only across X. Keeping its depth gives the temple the
     * long stepped ridge visible in the approved reference instead of a
     * vertically dominant pyramid. */
    AddStandardBox(model, inset, 9.5f + progress * 0.5f,
                   16.0f + step * roof_step_height,
                   32.0f - inset, 30.5f - progress * 0.5f,
                   16.0f + (step + 1) * roof_step_height,
                   step >= roof_steps / 2
                       ? kSimVoxelMaterial_RoofLight
                       : kSimVoxelMaterial_Roof);
  }

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
  int roof_steps = DetailChoice(detail, 2, 4, 6, 8);
  float roof_step_height = 8.0f / roof_steps;
  for (int step = 0; step < roof_steps; step++) {
    float progress = roof_steps == 1 ? 0.0f : (float)step / (roof_steps - 1);
    float inset = 6.5f + progress * 8.0f;
    AddStandardBox(model, inset, 2.0f + progress * 1.5f,
                   22.0f + step * roof_step_height,
                   32.0f - inset, 15.0f - progress * 1.5f,
                   22.0f + (step + 1) * roof_step_height,
                   step >= roof_steps / 2
                       ? kSimVoxelMaterial_RoofLight
                       : kSimVoxelMaterial_Roof);
  }
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

static void AddFactoryULayer(SimBackgroundVoxelModel *model,
                             float x0, float spine_x, float x1,
                             float y0, float gap_y0, float gap_y1, float y1,
                             float z0, float z1,
                             SimBackgroundVoxelMaterial material) {
  /* Open to the left: two horizontal arms join one right-hand spine. Omit the
   * touching east/west faces and restore only the spine wall exposed inside
   * the courtyard, avoiding coplanar internal faces and their seam noise. */
  AddBox(model, x0, y0, z0, spine_x, gap_y0, z1, material,
         kBoxFace_AllVisible & (uint8_t)~kBoxFace_East);
  AddBox(model, x0, gap_y1, z0, spine_x, y1, z1, material,
         kBoxFace_AllVisible & (uint8_t)~kBoxFace_East);
  AddBox(model, spine_x, y0, z0, x1, y1, z1, material,
         kBoxFace_AllVisible & (uint8_t)~kBoxFace_West);
  AddFace(model, material, 190,
          Point(spine_x, gap_y0, z0), Point(spine_x, gap_y1, z0),
          Point(spine_x, gap_y1, z1), Point(spine_x, gap_y0, z1));
}

static void AddFactoryArmRoof(SimBackgroundVoxelModel *model,
                              SimBackgroundVoxelDetail detail,
                              float y0, float y1) {
  int steps = DetailChoice(detail, 1, 2, 3, 4);
  float step_height = 3.0f / steps;
  for (int step = 0; step < steps; step++) {
    float progress = steps == 1 ? 0.0f : (float)step / (steps - 1);
    float inset = progress * 4.0f;
    AddStandardBox(model, 0.8f, y0 + inset,
                   9.0f + step * step_height,
                   22.2f, y1 - inset,
                   9.0f + (step + 1) * step_height,
                   step >= steps / 2
                       ? kSimVoxelMaterial_RoofLight
                       : kSimVoxelMaterial_Roof);
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

  /* The authentic 2x2 factory is a sideways U, open toward the left. Its
   * centre is real courtyard/ground, not a dark recess painted onto a block. */
  AddFactoryULayer(model, 0.5f, 21.5f, 31.5f,
                   0.5f, 11.5f, 20.5f, 31.5f,
                   0.0f, 1.5f, kSimVoxelMaterial_Trim);
  AddFactoryULayer(model, 1.5f, 21.5f, 30.5f,
                   1.5f, 11.5f, 20.5f, 30.5f,
                   1.5f, 8.0f, kSimVoxelMaterial_Wall);
  AddFactoryULayer(model, 0.8f, 21.5f, 31.2f,
                   1.0f, 11.0f, 21.0f, 31.0f,
                   7.5f, 9.0f, kSimVoxelMaterial_Roof);
  AddFactoryArmRoof(model, detail, 1.0f, 11.0f);
  AddFactoryArmRoof(model, detail, 21.0f, 31.0f);

  static const float chimney_xy[][2] = {
    {4.0f, 5.0f}, {4.0f, 24.0f},
    {16.0f, 5.0f}, {16.0f, 24.0f},
  };
  int chimneys = DetailChoice(detail, 1, 2, 4, 4);
  for (int i = 0; i < chimneys; i++) {
    float x = chimney_xy[i][0], y = chimney_xy[i][1];
    AddStandardBox(model, x, y, 10.0f, x + 3.0f, y + 3.0f, 16.0f,
                   kSimVoxelMaterial_Metal);
    AddStandardBox(model, x - 0.5f, y - 0.5f, 15.0f,
                   x + 3.5f, y + 3.5f, 17.0f,
                   kSimVoxelMaterial_Dark);
  }
  /* Public loading bays on the front arm and another bay opening into the
   * courtyard make the missing centre legible at the gameplay camera. */
  AddStandardBox(model, 13.0f, 30.0f, 1.5f, 19.0f, 32.0f, 7.0f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 4.0f, 30.0f, 3.5f, 8.0f, 31.5f, 6.5f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 24.0f, 30.0f, 3.5f, 28.0f, 31.5f, 6.5f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 8.0f, 10.7f, 2.5f, 15.0f, 12.0f, 7.0f,
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

static bool TreeCrownVoxel(int x, int y, int z, int resolution) {
  float nx = ((x + 0.5f) * 2.0f / resolution) - 1.0f;
  float ny = ((y + 0.5f) * 2.0f / resolution) - 1.0f;
  float height = (z + 0.5f) / resolution;
  float radius = 1.02f - height * 0.90f;
  /* A small flare at branch-tier boundaries keeps the evergreen readable as
   * layered voxel foliage while preserving one unmistakable pointed crown. */
  int tier = resolution >= 7 ? 3 : 2;
  if (z + 1 < resolution && z % tier == 0) radius += 0.08f;
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

static void BuildTreeCrown(const SimBackgroundVoxelObject *object,
                           SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model,
                           uint32_t seed, float offset_x, float offset_y) {
  bool occupied[kTreeCrownMaxResolution][kTreeCrownMaxResolution]
               [kTreeCrownMaxResolution] = {{{false}}};
  int resolution = DetailChoice(detail, 3, 5, 7, 9);
  for (int z = 0; z < resolution; z++)
    for (int y = 0; y < resolution; y++)
      for (int x = 0; x < resolution; x++)
        occupied[x][y][z] = TreeCrownVoxel(x, y, z, resolution);

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

  /* Connected cells meet through compact crown lobes. There is deliberately
   * no cell-sized shared slab: even a dense forest remains a collection of
   * readable round trees. The boundary face is omitted where neighbours meet. */
  if (object->tree_edges & kSimBackgroundTreeEdge_North)
    AddBox(model, 5.0f, 0.0f, 4.8f, 11.0f, 3.5f, 8.5f,
           kSimVoxelMaterial_LeavesDark,
           kBoxFace_AllVisible & (uint8_t)~kBoxFace_North);
  if (object->tree_edges & kSimBackgroundTreeEdge_East)
    AddBox(model, 12.5f, 5.0f, 4.8f, 16.0f, 11.0f, 8.5f,
           kSimVoxelMaterial_Leaves,
           kBoxFace_AllVisible & (uint8_t)~kBoxFace_East);
  if (object->tree_edges & kSimBackgroundTreeEdge_South)
    AddBox(model, 5.0f, 12.5f, 4.8f, 11.0f, 16.0f, 8.5f,
           kSimVoxelMaterial_Leaves,
           kBoxFace_AllVisible & (uint8_t)~kBoxFace_South);
  if (object->tree_edges & kSimBackgroundTreeEdge_West)
    AddBox(model, 0.0f, 5.0f, 4.8f, 3.5f, 11.0f, 8.5f,
           kSimVoxelMaterial_LeavesDark,
           kBoxFace_AllVisible & (uint8_t)~kBoxFace_West);
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
  BuildTreeCrown(object, detail, model, seed, offset_x, offset_y);
}

static void RecomputeModelBounds(SimBackgroundVoxelModel *model) {
  model->min_x = model->min_y = model->min_z = FLT_MAX;
  model->max_x = model->max_y = model->max_z = -FLT_MAX;
  for (uint16_t face = 0; face < model->face_count; face++)
    for (int point = 0; point < 4; point++)
      IncludePoint(model, model->faces[face].points[point]);
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
  RecomputeModelBounds(model);

  AddStandardBox(model, 0.7f, 4.0f, 0.0f, 6.4f, 15.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 1.1f, 5.0f, 1.5f, 6.0f, 14.3f, 6.5f,
                 kSimVoxelMaterial_WallLight);
  int roof_steps = DetailChoice(detail, 2, 3, 4, 5);
  float step_width = 4.8f / roof_steps;
  float step_height = 2.0f / roof_steps;
  for (int step = 0; step < roof_steps; step++)
    AddStandardBox(model,
                   0.6f + step * step_width, 4.2f, 6.5f + step * step_height,
                   6.6f, 14.8f, 6.5f + (step + 1) * step_height,
                   step >= roof_steps / 2
                       ? kSimVoxelMaterial_RoofLight
                       : kSimVoxelMaterial_Roof);
  if (detail != kSimBackgroundVoxelDetail_Low) {
    AddStandardBox(model, 2.2f, 14.0f, 2.8f, 4.6f, 15.1f, 5.5f,
                   kSimVoxelMaterial_Dark);
    AddStandardBox(model, 1.8f, 13.9f, 5.3f, 5.0f, 15.2f, 5.8f,
                   kSimVoxelMaterial_Trim);
  }
}

void SimBackgroundVoxelModel_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (detail < kSimBackgroundVoxelDetail_Low ||
      detail >= kSimBackgroundVoxelDetail_Count)
    detail = kSimBackgroundVoxelDetail_High;
  out->face_budget = SimBackgroundVoxelModel_FaceBudget(detail);
  out->min_x = out->min_y = out->min_z = FLT_MAX;
  out->max_x = out->max_y = out->max_z = -FLT_MAX;
  if (!object) return;

  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      BuildHouse(object, detail, out);
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
}
