#include "sim/voxels/sim_background_voxel_models.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sim/voxels/sim_background_bridge.h"
#include "sim/voxels/sim_background_voxel_region.h"

typedef enum ModelBoxFaces {
  kBoxFace_North = 1u << 0,
  kBoxFace_East = 1u << 1,
  kBoxFace_South = 1u << 2,
  kBoxFace_West = 1u << 3,
  kBoxFace_Top = 1u << 4,
  kBoxFace_AllVisible = kBoxFace_North | kBoxFace_East |
      kBoxFace_South | kBoxFace_West | kBoxFace_Top,
} ModelBoxFaces;

typedef struct SimBackgroundRockShape {
  float x, y, radius_x, radius_y, height;
} SimBackgroundRockShape;

static int RockShapes(const SimBackgroundVoxelObject *object,
                       const SimBackgroundRockShape **out) {
  /* The terrain atlas fixes both the count and placement of each stone.
   * Preserve those identities instead of randomly populating every tile. */
  static const SimBackgroundRockShape boulder[] = {
    {4.6f, 8.7f, 4.3f, 3.7f, 6.0f}, {10.2f, 9.0f, 4.4f, 5.2f, 8.0f},
  };
  static const SimBackgroundRockShape stones[5][4] = {
    {{12.3f, 13.2f, 2.0f, 1.4f, 2.2f}},
    {{3.2f, 5.2f, 3.1f, 2.2f, 4.2f}, {12.0f, 4.7f, 3.2f, 2.0f, 3.6f}},
    {{3.7f, 13.0f, 3.3f, 2.4f, 4.5f}, {10.4f, 2.8f, 2.0f, 1.0f, 1.6f},
     {12.1f, 6.1f, 2.5f, 1.4f, 2.0f}},
    {{4.4f, 5.5f, 1.8f, 1.4f, 2.8f}, {10.8f, 10.6f, 2.0f, 1.1f, 1.5f},
     {13.0f, 13.6f, 2.6f, 1.6f, 2.0f}},
    {{2.5f, 2.3f, 1.8f, 0.9f, 1.4f}, {4.0f, 6.3f, 2.4f, 1.5f, 2.3f},
     {3.8f, 13.2f, 1.8f, 1.4f, 2.8f},
     {12.2f, 12.5f, 3.0f, 2.4f, 4.2f}},
  };
  if (object->kind == kSimBackgroundVoxel_Boulder) {
    *out = boulder;
    return 2;
  }
  int layout = object->visual_metatile == 0x62 ? 0
      : object->visual_metatile == 0x63 ? 1
      : object->visual_metatile == 0x69 ? 2
      : object->visual_metatile == 0x6A ? 3 : 4;
  *out = stones[layout];
  return layout == 0 ? 1 : layout == 1 ? 2 : layout == 4 ? 4 : 3;
}

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
  face->outward_winding = false;
  for (int point = 0; point < 4; point++) face->occlusion[point] = 255;
  IncludePoint(model, a);
  IncludePoint(model, b);
  IncludePoint(model, c);
  IncludePoint(model, d);
}

/* Opt-in outward winding lets thin foliage keep a genuinely downward-facing
 * underside without changing the established box and roof conventions. */
static void AddOutwardFace(
    SimBackgroundVoxelModel *model, SimBackgroundVoxelMaterial material,
    uint8_t brightness, SimBackgroundVoxelModelPoint a,
    SimBackgroundVoxelModelPoint b, SimBackgroundVoxelModelPoint c,
    SimBackgroundVoxelModelPoint d) {
  uint16_t before = model->face_count;
  AddFace(model, material, brightness, a, b, c, d);
  if (model->face_count > before)
    model->faces[before].outward_winding = true;
}

static float HouseDepthScale(const SimBackgroundVoxelObject *object) {
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) return 1.0f;
  SimBackgroundVoxelHouseStyle style = SimBackgroundVoxelRegion_ObjectHouseStyle(object);
  return style == kSimBackgroundHouseStyle_Yurt ||
      style == kSimBackgroundHouseStyle_WhiteTent ||
      style == kSimBackgroundHouseStyle_Tent ? 1.0f : .72f;
}

static float HouseDepth(float y, float scale) {
  return 15.5f + (y - 15.5f) * scale;
}

int SimBackgroundVoxelModel_Contacts(const SimBackgroundVoxelObject *object,
                         SimBackgroundVoxelModelContact *out) {
  if (!object || !out) return 0;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House: {
      float scale = HouseDepthScale(object);
      SimBackgroundVoxelHouseStyle family = SimBackgroundVoxelRegion_ObjectHouseStyle(object);
      if (family == kSimBackgroundHouseStyle_Yurt) {
        bool frame = (object->flags & kSimBackgroundVoxel_UnderConstruction) != 0;
        int count = frame ? 4 : 8;
        for (int post = 0; post < count; post++) {
          float angle = 1.178097245f + post * (frame ? 2 : 1) * .785398163f;
          float x = 8 + cosf(angle) * 5.55f, y = 8 + sinf(angle) * 5.55f;
          out[post] = (SimBackgroundVoxelModelContact){x - .2f, y - .2f, x + .2f, y + .2f};
        }
        return count;
      }
      if (family == kSimBackgroundHouseStyle_WhiteTent || family == kSimBackgroundHouseStyle_Tent) {
        out[0] = (SimBackgroundVoxelModelContact){1.4f, 3, 1.8f, 13.9f};
        out[1] = (SimBackgroundVoxelModelContact){14.2f, 3, 14.6f, 13.9f};
        if ((object->flags & kSimBackgroundVoxel_AlternateFacing) &&
            !(object->flags & kSimBackgroundVoxel_UnderConstruction)) {
          out[2] = (SimBackgroundVoxelModelContact){5.0f, 15, 5.4f, 15.4f};
          out[3] = (SimBackgroundVoxelModelContact){10.6f, 15, 11.0f, 15.4f};
          return 4;
        }
        return 2;
      }
      if (!(object->flags & kSimBackgroundVoxel_UnderConstruction) &&
          SimBackgroundVoxelRegion_ObjectHouseStyle(object) == kSimBackgroundHouseStyle_MarahnaStilt) {
        for (int post = 0; post < 4; post++) {
          float x = post & 1 ? 12.5f : 2.5f;
          float x1 = x + 1.0f, y = post & 2 ? 13.5f : 4.0f;
          out[post] = (SimBackgroundVoxelModelContact){x, HouseDepth(y, scale),
                                                     x1, HouseDepth(y + 1, scale)};
        }
        return 4;
      }
      out[0] = (SimBackgroundVoxelModelContact){1.8f, HouseDepth(2.5f, scale),
                                              14.2f, HouseDepth(15.2f, scale)};
      return 1;
    }
    case kSimBackgroundVoxel_Cathedral:
      out[0] = (SimBackgroundVoxelModelContact){0.8f, 8.8f, 31.2f, 31.4f};
      return 1;
    case kSimBackgroundVoxel_Windmill:
      if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
        out[0] = (SimBackgroundVoxelModelContact){8.5f, 1.0f, 23.5f, 13.0f};
        if (object->animation_phase % 3 == 2) {
          out[1] = (SimBackgroundVoxelModelContact){12.7f, 13.0f, 19.3f, 16.4f};
          return 2;
        }
        out[1] = (SimBackgroundVoxelModelContact){7.0f, 9.5f, 8.0f, 10.5f};
        out[2] = (SimBackgroundVoxelModelContact){24.0f, 9.5f, 25.0f, 10.5f};
        return 3;
      }
      out[0] = (SimBackgroundVoxelModelContact){8.5f, 1.0f, 23.5f, 13.0f};
      out[1] = (SimBackgroundVoxelModelContact){12.7f, 13.0f, 19.3f, 16.4f};
      return 2;
    case kSimBackgroundVoxel_Factory:
      out[0] = (SimBackgroundVoxelModelContact){0.8f, 0.8f, 21.8f, 10.8f};
      out[1] = (SimBackgroundVoxelModelContact){0.8f, 22.2f, 21.8f, 31.2f};
      out[2] = (SimBackgroundVoxelModelContact){21.2f, 0.8f, 31.2f, 31.2f};
      return 3;
    case kSimBackgroundVoxel_Tree:
      out[0] = (SimBackgroundVoxelModelContact){7.0f, 7.0f, 9.0f, 9.0f};
      return 1;
    case kSimBackgroundVoxel_BroadTree:
      out[0] = (SimBackgroundVoxelModelContact){6.7f, 8.7f, 9.3f, 11.3f};
      return 1;
    case kSimBackgroundVoxel_Palm:
      out[0] = (SimBackgroundVoxelModelContact){6.8f, 6.8f, 9.2f, 9.2f};
      return 1;
    case kSimBackgroundVoxel_Shrub:
      out[0] = (SimBackgroundVoxelModelContact){5.8f, 5.8f, 10.2f, 10.2f};
      return 1;
    case kSimBackgroundVoxel_Boulder:
    case kSimBackgroundVoxel_Rocks:
      /* Stones already meet the terrain; do not paint contact decals beneath
       * them, especially a shared rectangle around scattered pebbles. */
      return 0;
    case kSimBackgroundVoxel_StoryTree:
      out[0] = (SimBackgroundVoxelModelContact){12.0f, 18.0f, 20.0f, 26.0f};
      return 1;
    case kSimBackgroundVoxel_BloodpoolCastle:
      /* Side strips include the corner towers; separate supports for the
       * keep and its turrets leave both courtyard lanes free of foundation. */
      out[0] = (SimBackgroundVoxelModelContact){2.0f, 4.0f, 7.0f, 32.0f};
      out[1] = (SimBackgroundVoxelModelContact){25.0f, 4.0f, 30.0f, 32.0f};
      out[2] = (SimBackgroundVoxelModelContact){7.0f, 4.0f, 25.0f, 7.0f};
      out[3] = (SimBackgroundVoxelModelContact){7.0f, 27.0f, 10.5f, 29.5f};
      out[4] = (SimBackgroundVoxelModelContact){11.0f, 10.0f, 21.0f, 22.0f};
      out[5] = (SimBackgroundVoxelModelContact){9.0f, 7.0f, 13.0f, 11.0f};
      out[6] = (SimBackgroundVoxelModelContact){19.0f, 7.0f, 23.0f, 11.0f};
      out[7] = (SimBackgroundVoxelModelContact){21.5f, 27.0f, 25.0f, 29.5f};
      out[8] = (SimBackgroundVoxelModelContact){10.5f, 26.5f, 13.5f, 32.0f};
      out[9] = (SimBackgroundVoxelModelContact){18.5f, 26.5f, 21.5f, 32.0f};
      out[10] = (SimBackgroundVoxelModelContact){13.5f, 26.5f, 18.5f, 32.0f};
      return 11;
    case kSimBackgroundVoxel_MarahnaTemple:
      out[0] = (SimBackgroundVoxelModelContact){1.0f, 7.5f, 10.0f, 16.5f};
      out[1] = (SimBackgroundVoxelModelContact){10.5f, 8.5f, 21.5f, 22.5f};
      out[2] = (SimBackgroundVoxelModelContact){22.0f, 7.5f, 31.0f, 16.5f};
      out[3] = (SimBackgroundVoxelModelContact){14.0f, 19.5f, 18.0f, 32.0f};
      /* Thin supports follow the wrapping walls without filling the courts. */
      out[4] = (SimBackgroundVoxelModelContact){0.0f, 12.0f, 2.0f, 32.0f};
      out[5] = (SimBackgroundVoxelModelContact){2.0f, 29.0f, 9.5f, 32.0f};
      out[6] = (SimBackgroundVoxelModelContact){9.5f, 29.0f, 12.5f, 32.0f};
      out[7] = (SimBackgroundVoxelModelContact){30.0f, 12.0f, 32.0f, 32.0f};
      out[8] = (SimBackgroundVoxelModelContact){22.5f, 29.0f, 30.0f, 32.0f};
      out[9] = (SimBackgroundVoxelModelContact){19.5f, 29.0f, 22.5f, 32.0f};
      out[10] = (SimBackgroundVoxelModelContact){3.6f, 23.1f, 8.4f, 27.9f};
      out[11] = (SimBackgroundVoxelModelContact){23.6f, 23.1f, 28.4f, 27.9f};
      return 12;
    case kSimBackgroundVoxel_Pyramid:
      out[0] = (SimBackgroundVoxelModelContact){0.5f, 0.5f, 31.5f, 31.5f};
      return 1;
    case kSimBackgroundVoxel_Bridge:
      return 0;
  }
  return 0;
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

uint16_t SimBackgroundVoxelModel_ObjectFaceBudget(
    const SimBackgroundVoxelObject *object, SimBackgroundVoxelDetail detail) {
  /* These unique town landmarks retain their complete architectural masses at
   * Low. Repeated buildings and vegetation keep the normal density limits. */
  if (object && object->kind == kSimBackgroundVoxel_BloodpoolCastle &&
      detail == kSimBackgroundVoxelDetail_Low) return 128;
  if (object && object->kind == kSimBackgroundVoxel_MarahnaTemple) {
    if (detail == kSimBackgroundVoxelDetail_Low) return 144;
    if (detail == kSimBackgroundVoxelDetail_Balanced) return 256;
    if (detail == kSimBackgroundVoxelDetail_High) return 320;
  }
  if (object && object->kind == kSimBackgroundVoxel_StoryTree) {
    static const uint16_t budgets[] = {128, 192, 320, 384};
    if (detail >= 0 && detail < kSimBackgroundVoxelDetail_Count) return budgets[detail];
  }
  return SimBackgroundVoxelModel_FaceBudget(detail);
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
                                   float width, float depth, float height);

enum {
  kStoneBridgeDeckBrightness = 245,
  kStoneBridgeCourseBrightness = 218,
  kStoneBridgeFineCourseBrightness = 204,
  kStoneBridgeFineCourseCount = 2,
  /* The two outer opening faces differ deliberately so their authored shade
   * follows the model's directional side-lighting on each bridge axis. */
  kStoneBridgeEastWestNearOpeningBrightness = 170,
  kStoneBridgeEastWestFarOpeningBrightness = 190,
  kStoneBridgeNorthSouthNearOpeningBrightness = 178,
  kStoneBridgeNorthSouthFarOpeningBrightness = 198,
};

static void BuildStoneBridge(const SimBackgroundVoxelObject *object,
                             SimBackgroundVoxelDetail detail,
                             SimBackgroundVoxelModel *model) {
  const SimBackgroundBridgeBounds bounds =
      SimBackgroundBridge_ResolveBounds(object);
  const float width = bounds.width, depth = bounds.depth;
  if (width <= 0.0f || depth <= 0.0f) return;
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    /* Native visual class 2 has one held construction frame before its
     * completed stone stage. Keep the crossing visibly open and skeletal
     * instead of publishing the full parapets as soon as its E1/E2 semantic
     * marker appears. */
    AddStandardBox(model, 1.0f, 1.0f, -1.2f,
                   width - 1.0f, depth - 1.0f, 0.0f,
                   kSimVoxelMaterial_Wall);
    BuildConstructionFrame(model, width, depth, 4.5f);
    return;
  }
  const float deck_height = 0.35f;
  const float slab_bottom = -2.4f;
  const float parapet_embed = -0.75f;
  const float parapet_body_width = 1.15f;
  const float parapet_cap_width = 1.42f;
  const float post_width = 1.75f;
  const float underside_offset = 0.02f;
  const float underside_bottom = -1.9f;
  const float underside_top = -0.7f;
  const float coarse_course_half_width = 0.18f;
  const float coarse_course_lift = 0.025f;
  const float fine_course_half_width = 0.14f;
  const float fine_course_lift = 0.03f;
  /* Low slab, visible masonry sides, separate paving cap. There is no bottom
   * face: the dark inset below reads as the shallow opening seen in the native
   * graphic without inventing timber supports or free-standing piers. */
  AddBox(model, 0.0f, 0.0f, slab_bottom, width, depth, deck_height,
         kSimVoxelMaterial_Wall,
         kBoxFace_AllVisible & ~kBoxFace_Top);
  AddFace(model, kSimVoxelMaterial_Paving, kStoneBridgeDeckBrightness,
          Point(0.0f, 0.0f, deck_height),
          Point(width, 0.0f, deck_height),
          Point(width, depth, deck_height),
          Point(0.0f, depth, deck_height));

  /* The approved native-stone silhouette has real parapets, not painted curb
   * lines.  A grey-green masonry body carries a narrow pale cap, while the
   * balanced tier adds compact terminal posts at the banks. */
  /* Keep enough height for the approved railing silhouette. The detached
   * pale bar seen in Marahna was residual native ground art, not this model's
   * far parapet; flattening the real parapet would only turn the bridge back
   * into the block the authored model was meant to replace. */
  const float parapet_body_height = 2.65f;
  const float parapet_cap_height = 3.25f;
  const float post_height = SimBackgroundBridge_AuthoredHeight();
  if (object->bridge_axis == kSimBackgroundBridgeAxis_EastWest) {
    AddStandardBox(model, 0.0f, 0.0f, parapet_embed,
                   width, parapet_body_width, parapet_body_height,
                   kSimVoxelMaterial_WallLight);
    AddStandardBox(model, 0.0f, depth - parapet_body_width, parapet_embed,
                   width, depth, parapet_body_height,
                   kSimVoxelMaterial_WallLight);
    AddStandardBox(model, 0.0f, 0.0f, parapet_body_height,
                   width, parapet_cap_width, parapet_cap_height,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 0.0f, depth - parapet_cap_width,
                   parapet_body_height,
                   width, depth, parapet_cap_height,
                   kSimVoxelMaterial_Trim);
    AddFace(model, kSimVoxelMaterial_Dark,
            kStoneBridgeEastWestNearOpeningBrightness,
            Point(width, -underside_offset, underside_bottom),
            Point(0.0f, -underside_offset, underside_bottom),
            Point(0.0f, -underside_offset, underside_top),
            Point(width, -underside_offset, underside_top));
    AddFace(model, kSimVoxelMaterial_Dark,
            kStoneBridgeEastWestFarOpeningBrightness,
            Point(0.0f, depth + underside_offset, underside_bottom),
            Point(width, depth + underside_offset, underside_bottom),
            Point(width, depth + underside_offset, underside_top),
            Point(0.0f, depth + underside_offset, underside_top));
    if (detail >= kSimBackgroundVoxelDetail_Balanced) {
      AddStandardBox(model, 0.0f, 0.0f, parapet_embed,
                     post_width, post_width, post_height,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, 0.0f, depth - post_width, parapet_embed,
                     post_width, depth, post_height, kSimVoxelMaterial_Trim);
      AddStandardBox(model, width - post_width, 0.0f, parapet_embed,
                     width, post_width, post_height, kSimVoxelMaterial_Trim);
      AddStandardBox(model, width - post_width, depth - post_width,
                     parapet_embed,
                     width, depth, post_height, kSimVoxelMaterial_Trim);
    }
  } else {
    AddStandardBox(model, 0.0f, 0.0f, parapet_embed,
                   parapet_body_width, depth, parapet_body_height,
                   kSimVoxelMaterial_WallLight);
    AddStandardBox(model, width - parapet_body_width, 0.0f, parapet_embed,
                   width, depth, parapet_body_height,
                   kSimVoxelMaterial_WallLight);
    AddStandardBox(model, 0.0f, 0.0f, parapet_body_height,
                   parapet_cap_width, depth, parapet_cap_height,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, width - parapet_cap_width, 0.0f,
                   parapet_body_height,
                   width, depth, parapet_cap_height,
                   kSimVoxelMaterial_Trim);
    AddFace(model, kSimVoxelMaterial_Dark,
            kStoneBridgeNorthSouthNearOpeningBrightness,
            Point(-underside_offset, 0.0f, underside_bottom),
            Point(-underside_offset, depth, underside_bottom),
            Point(-underside_offset, depth, underside_top),
            Point(-underside_offset, 0.0f, underside_top));
    AddFace(model, kSimVoxelMaterial_Dark,
            kStoneBridgeNorthSouthFarOpeningBrightness,
            Point(width + underside_offset, depth, underside_bottom),
            Point(width + underside_offset, 0.0f, underside_bottom),
            Point(width + underside_offset, 0.0f, underside_top),
            Point(width + underside_offset, depth, underside_top));
    if (detail >= kSimBackgroundVoxelDetail_Balanced) {
      AddStandardBox(model, 0.0f, 0.0f, parapet_embed,
                     post_width, post_width, post_height,
                     kSimVoxelMaterial_Trim);
      AddStandardBox(model, width - post_width, 0.0f, parapet_embed,
                     width, post_width, post_height, kSimVoxelMaterial_Trim);
      AddStandardBox(model, 0.0f, depth - post_width, parapet_embed,
                     post_width, depth, post_height, kSimVoxelMaterial_Trim);
      AddStandardBox(model, width - post_width, depth - post_width,
                     parapet_embed,
                     width, depth, post_height, kSimVoxelMaterial_Trim);
    }
  }
  /* Higher density resolves restrained transverse masonry courses on the
   * paving. They are stone-on-stone, never timber slats, and each tier adds a
   * visible feature so the global quality control remains truthful. */
  if (detail >= kSimBackgroundVoxelDetail_High) {
    if (object->bridge_axis == kSimBackgroundBridgeAxis_EastWest) {
      float x = width * 0.5f;
      AddFace(model, kSimVoxelMaterial_WallLight,
              kStoneBridgeCourseBrightness,
              Point(x - coarse_course_half_width, parapet_body_width,
                    deck_height + coarse_course_lift),
              Point(x + coarse_course_half_width, parapet_body_width,
                    deck_height + coarse_course_lift),
              Point(x + coarse_course_half_width,
                    depth - parapet_body_width,
                    deck_height + coarse_course_lift),
              Point(x - coarse_course_half_width,
                    depth - parapet_body_width,
                    deck_height + coarse_course_lift));
    } else {
      float y = depth * 0.5f;
      AddFace(model, kSimVoxelMaterial_WallLight,
              kStoneBridgeCourseBrightness,
              Point(parapet_body_width, y - coarse_course_half_width,
                    deck_height + coarse_course_lift),
              Point(width - parapet_body_width,
                    y - coarse_course_half_width,
                    deck_height + coarse_course_lift),
              Point(width - parapet_body_width,
                    y + coarse_course_half_width,
                    deck_height + coarse_course_lift),
              Point(parapet_body_width, y + coarse_course_half_width,
                    deck_height + coarse_course_lift));
    }
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int course = 1; course <= kStoneBridgeFineCourseCount; course++) {
      float t = course / (float)(kStoneBridgeFineCourseCount + 1);
      if (object->bridge_axis == kSimBackgroundBridgeAxis_EastWest) {
        float x = width * t;
        AddFace(model, kSimVoxelMaterial_WallLight,
                kStoneBridgeFineCourseBrightness,
                Point(x - fine_course_half_width, parapet_body_width,
                      deck_height + fine_course_lift),
                Point(x + fine_course_half_width, parapet_body_width,
                      deck_height + fine_course_lift),
                Point(x + fine_course_half_width,
                      depth - parapet_body_width,
                      deck_height + fine_course_lift),
                Point(x - fine_course_half_width,
                      depth - parapet_body_width,
                      deck_height + fine_course_lift));
      } else {
        float y = depth * t;
        AddFace(model, kSimVoxelMaterial_WallLight,
                kStoneBridgeFineCourseBrightness,
                Point(parapet_body_width, y - fine_course_half_width,
                      deck_height + fine_course_lift),
                Point(width - parapet_body_width,
                      y - fine_course_half_width,
                      deck_height + fine_course_lift),
                Point(width - parapet_body_width,
                      y + fine_course_half_width,
                      deck_height + fine_course_lift),
                Point(parapet_body_width, y + fine_course_half_width,
                      deck_height + fine_course_lift));
      }
    }
  }
}

static void AddOctagonalFrustum(
    SimBackgroundVoxelModel *model, float center_x, float center_y,
    float lower_radius, float upper_radius, float z0, float z1,
    SimBackgroundVoxelMaterial material) {
  /* A fixed octagon gives round structures their identity while retaining
   * the broad planar faces and deliberately stepped shading of the town art. */
  static const float unit[8][2] = {
    {0.0f, -1.0f}, {0.7071f, -0.7071f}, {1.0f, 0.0f},
    {0.7071f, 0.7071f}, {0.0f, 1.0f}, {-0.7071f, 0.7071f},
    {-1.0f, 0.0f}, {-0.7071f, -0.7071f},
  };
  static const uint8_t brightness[8] = {
    178, 190, 204, 218, 232, 218, 190, 178,
  };
  for (int side = 0; side < 8; side++) {
    int next = (side + 1) & 7;
    AddFace(model, material, brightness[side],
            Point(center_x + unit[side][0] * lower_radius,
                  center_y + unit[side][1] * lower_radius, z0),
            Point(center_x + unit[next][0] * lower_radius,
                  center_y + unit[next][1] * lower_radius, z0),
            Point(center_x + unit[next][0] * upper_radius,
                  center_y + unit[next][1] * upper_radius, z1),
            Point(center_x + unit[side][0] * upper_radius,
                  center_y + unit[side][1] * upper_radius, z1));
  }
}

/* Hipped cap. A zero upper radius closes the peak without a detached
 * capstone; every detail level retains the same pointed silhouette. */
static void AddSquareFrustum(SimBackgroundVoxelModel *model,
                             float x, float y, float r0, float r1,
                             float z0, float z1,
                             SimBackgroundVoxelMaterial material) {
  static const int corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  static const uint8_t shade[4] = {178, 204, 232, 190};
  for (int side = 0; side < 4; side++) {
    int next = (side + 1) & 3;
    AddFace(model, material, shade[side],
            Point(x + corner[side][0] * r0, y + corner[side][1] * r0, z0),
            Point(x + corner[next][0] * r0, y + corner[next][1] * r0, z0),
            Point(x + corner[next][0] * r1, y + corner[next][1] * r1, z1),
            Point(x + corner[side][0] * r1, y + corner[side][1] * r1, z1));
  }
}

static void AddRoofedBox(SimBackgroundVoxelModel *model,
                         float x0, float y0, float z0,
                         float x1, float y1, float z1,
                         SimBackgroundVoxelMaterial material) {
  /* The roof completely covers this cap. Do not submit buried coincident
   * geometry: it wastes fragments and makes the result dependent on the
   * equal-depth rule at roof boundaries. */
  AddBox(model, x0, y0, z0, x1, y1, z1, material,
         kBoxFace_AllVisible & ~kBoxFace_Top);
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

/* A roof truss is four extruded sides; its ends meet the wall posts and
 * ridge beam. Keeping these as surfaces avoids buried box caps. */
static void AddRafter(SimBackgroundVoxelModel *model, float x0, float z0,
                       float x1, float z1, float y, bool across_y) {
  float dx = x1 - x0, dz = z1 - z0;
  float length = sqrtf(dx * dx + dz * dz);
  float nx = -dz / length * .38f, nz = dx / length * .38f;
  SimBackgroundVoxelModelPoint p[8];
  for (int side = 0; side < 2; side++) {
    p[side * 4] = Point(x0 - nx, y + (side ? .4f : -.4f), z0 - nz);
    p[side * 4 + 1] = Point(x1 - nx, y + (side ? .4f : -.4f), z1 - nz);
    p[side * 4 + 2] = Point(x1 + nx, y + (side ? .4f : -.4f), z1 + nz);
    p[side * 4 + 3] = Point(x0 + nx, y + (side ? .4f : -.4f), z0 + nz);
  }
  if (across_y) for (int v = 0; v < 8; v++) {
    float tmp = p[v].x; p[v].x = p[v].y; p[v].y = tmp;
  }
  AddFace(model, kSimVoxelMaterial_Wood, 232, p[0], p[1], p[2], p[3]);
  AddFace(model, kSimVoxelMaterial_Wood, 200, p[4], p[7], p[6], p[5]);
  AddFace(model, kSimVoxelMaterial_Wood, 245, p[3], p[2], p[6], p[7]);
  AddFace(model, kSimVoxelMaterial_Wood, 180, p[0], p[4], p[5], p[1]);
}

static void AddGableFrame(SimBackgroundVoxelModel *model, float x0, float x1,
                           float y, float eave, float ridge, bool across_y) {
  for (int side = 0; side < 2; side++) {
    float x = side ? x1 : x0;
    if (across_y)
      AddRoofedBox(model, y - .45f, x - .45f, 0, y + .45f, x + .45f, eave,
                   kSimVoxelMaterial_Wood);
    else
      AddRoofedBox(model, x - .45f, y - .45f, 0, x + .45f, y + .45f, eave,
                   kSimVoxelMaterial_Wood);
    AddRafter(model, x, eave, (x0 + x1) * .5f, ridge, y, across_y);
  }
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
  /* Models retain their original south-facing presentation throughout the
   * supported orbit range. The rear gable is always hidden by the roof, so
   * omitting it avoids a redundant coincident surface at the ridge. */
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

static void AddFrontBand(SimBackgroundVoxelModel *model, float x0, float x1,
                          float y, float z0, float z1,
                          SimBackgroundVoxelMaterial material) {
  AddFace(model, material, 232, Point(x0, y, z0), Point(x1, y, z0),
          Point(x1, y, z1), Point(x0, y, z1));
}

/* Four walls and an inward-bevelled rim meet a recessed dark flue. No
 * facade window masquerades as a chimney opening. */
static void AddOpenChimney(SimBackgroundVoxelModel *model,
    float x, float y, float width, float z0, float top,
    SimBackgroundVoxelMaterial material) {
  uint16_t first = model->face_count;
  /* Solid occupancy stops at the flue floor, or the surface compiler would
   * discard the recessed opening as a face buried inside a filled box. */
  AddRoofedBox(model, x, y, z0, x + width, y + width, top-.3f, material);
  for (uint16_t f=first; f<model->face_count; f++)
    for (int v=0; v<4; v++)
      if (model->faces[f].points[v].z == top-.3f) model->faces[f].points[v].z = top;
  const float inset = width * .22f;
  SimBackgroundVoxelModelPoint outer[4] = {
    {x,y,top}, {x+width,y,top}, {x+width,y+width,top}, {x,y+width,top}};
  SimBackgroundVoxelModelPoint inner[4] = {
    {x+inset,y+inset,top-.3f}, {x+width-inset,y+inset,top-.3f},
    {x+width-inset,y+width-inset,top-.3f}, {x+inset,y+width-inset,top-.3f}};
  for (int edge = 0; edge < 4; edge++) {
    int next = (edge + 1) & 3;
    AddFace(model, kSimVoxelMaterial_Trim, 244, outer[edge], outer[next], inner[next], inner[edge]);
  }
  AddFace(model, kSimVoxelMaterial_Dark, 170, inner[0], inner[1], inner[2], inner[3]);
}

static void BuildFillmoreHouse(SimBackgroundVoxelDetail detail,
                               SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 1.5f, 2.0f, 0.0f, 14.5f, 15.0f, 2.0f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.5f, 3.0f, 2.0f, 13.5f, 14.5f, 10.0f,
               kSimVoxelMaterial_Wall);
  /* A four-pixel rise keeps the gable recognizable without letting the roof
   * dominate the finished house. The older six-pixel rise was the remaining
   * source of the too-tall silhouette even after presentation scaling. */
  AddGableRoofX(model, 1.0f, 15.0f, 2.0f, 15.0f, 10.0f, 14.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 10.5f, 4.5f, 11.5f, 12.3f, 6.5f, 15.1f,
                 kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 10.2f, 4.2f, 15.1f, 12.6f, 6.8f, 15.55f,
                 kSimVoxelMaterial_Trim);
  AddFace(model, kSimVoxelMaterial_Dark, 170,
      Point(10.7f, 4.7f, 15.6f), Point(12.1f, 4.7f, 15.6f),
      Point(12.1f, 6.3f, 15.6f), Point(10.7f, 6.3f, 15.6f));
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
      AddStandardBox(model, x0 - 0.4f, 14.0f, 5.0f,
                     x0, 15.4f, 8.0f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x1, 14.0f, 5.0f,
                     x1 + 0.4f, 15.4f, 8.0f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x0 - 0.4f, 14.0f, 8.0f,
                     x1 + 0.4f, 15.4f, 8.4f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x0 - 0.4f, 14.0f, 4.6f,
                     x1 + 0.4f, 15.4f, 5.0f, kSimVoxelMaterial_Trim);
    }
    AddStandardBox(model, 6.5f, 14.0f, 2.0f, 7.0f, 15.5f, 7.5f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 10.0f, 14.0f, 2.0f, 10.5f, 15.5f, 7.5f,
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

/* A small closed pole follows its endpoints. End caps are retained because
 * shelter frames and exposed tips can be viewed from either side. */
static void AddShelterRod(SimBackgroundVoxelModel *model,
                            SimBackgroundVoxelModelPoint a,
                            SimBackgroundVoxelModelPoint b, float radius, bool caps) {
  float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
  float length = sqrtf(dx * dx + dy * dy + dz * dz);
  float horizontal = sqrtf(dx * dx + dy * dy);
  float ux = horizontal > .001f ? -dy / horizontal : 1;
  float uy = horizontal > .001f ? dx / horizontal : 0;
  float vx = -dz * uy / length, vy = dz * ux / length;
  float vz = (dx * uy - dy * ux) / length;
  SimBackgroundVoxelModelPoint ends[2][4];
  for (int end = 0; end < 2; end++) {
    SimBackgroundVoxelModelPoint center = end ? b : a;
    for (int corner = 0; corner < 4; corner++) {
      float u = corner == 0 || corner == 3 ? -radius : radius;
      float v = corner < 2 ? -radius : radius;
      ends[end][corner] = Point(center.x + u * ux + v * vx,
          center.y + u * uy + v * vy, fmaxf(0, center.z + v * vz));
    }
  }
  for (int side = 0; side < 4; side++) {
    int next = (side + 1) & 3;
    AddFace(model, kSimVoxelMaterial_Wood, 205 + side * 12,
        ends[0][side], ends[0][next], ends[1][next], ends[1][side]);
  }
  if (!caps) return;
  AddFace(model, kSimVoxelMaterial_Wood, 180,
      ends[0][3], ends[0][2], ends[0][1], ends[0][0]);
  AddFace(model, kSimVoxelMaterial_Wood, 240,
      ends[1][0], ends[1][1], ends[1][2], ends[1][3]);
}

static void AddShelterPole(SimBackgroundVoxelModel *model,
                            SimBackgroundVoxelModelPoint a,
                            SimBackgroundVoxelModelPoint b, float radius) {
  AddShelterRod(model, a, b, radius, true);
}

static SimBackgroundVoxelModelPoint StrawPoint(int sector, float z, float lift, bool alternate) {
  float angle = 1.178097245f + sector * .785398163f;
  float shoulder = alternate ? 7.0f : 9.0f, top = alternate ? 7.8f : 13.5f;
  float shoulder_radius = alternate ? 4.8f : 3.4f, top_radius = alternate ? 3.8f : .5f;
  float radius = z <= shoulder ?
      6.2f + (shoulder_radius - 6.2f) * (z - 5.5f) / (shoulder - 5.5f) :
      shoulder_radius + (top_radius - shoulder_radius) * (z - shoulder) / (top - shoulder);
  float droop = .18f * (sector & 1) * (top - z) / (top - 5.5f);
  return Point(8 + cosf(angle) * (radius + lift),
      8 + sinf(angle) * (radius + lift), z - droop);
}

static SimBackgroundVoxelModelPoint StrawWallPoint(int sector, float z, float lift) {
  float angle = 1.178097245f + sector * .785398163f;
  float radius = 5.65f - .2f * z / 5.8f + lift;
  return Point(8 + cosf(angle) * radius, 8 + sinf(angle) * radius, z);
}

static SimBackgroundVoxelModelPoint StrawCrestPoint(float x, float t, bool back) {
  float top = fabsf(x) > 4.3f ? 12.8f : 12.0f;
  return Point(8 + x * (.59615385f + .40384615f * t),
      8 + (back ? -1 : 1) * (1.3f - .95f * t),
      7.2f + (top - 7.2f) * t);
}

static void AddStrawCrest(SimBackgroundVoxelModel *model, bool partial) {
  /* A broad, thick thatch crest rises out of the hut's shoulder. Raised
   * outer tips and ochre bundle ends carry the native headdress silhouette;
   * this is covered straw, not a flat yurt cap or exposed timber frame. */
  static const float edges[] = {-5.2f, -4.2f, 4.2f, 5.2f};
  for (int panel = 0; panel < 3; panel++) {
    float x0 = edges[panel], x1 = edges[panel + 1];
    SimBackgroundVoxelModelPoint a = StrawCrestPoint(x0, 0, false);
    SimBackgroundVoxelModelPoint b = StrawCrestPoint(x1, 0, false);
    SimBackgroundVoxelModelPoint c = StrawCrestPoint(x1, 1, false);
    SimBackgroundVoxelModelPoint d = StrawCrestPoint(x0, 1, false);
    SimBackgroundVoxelModelPoint ar = StrawCrestPoint(x0, 0, true);
    SimBackgroundVoxelModelPoint br = StrawCrestPoint(x1, 0, true);
    SimBackgroundVoxelModelPoint cr = StrawCrestPoint(x1, 1, true);
    SimBackgroundVoxelModelPoint dr = StrawCrestPoint(x0, 1, true);
    AddFace(model, kSimVoxelMaterial_Wall, 218, br, ar, dr, cr);
    if (partial) continue;
    AddFace(model, kSimVoxelMaterial_WallLight,
        228, a, b, c, d);
    AddFace(model, kSimVoxelMaterial_Wall, 215, d, c, cr, dr);
    if (panel == 0) AddFace(model, kSimVoxelMaterial_Wall, 210, ar, a, d, dr);
    if (panel == 2) AddFace(model, kSimVoxelMaterial_Wall, 210, b, br, cr, c);
  }
  if (partial) return;
  for (int bundle = 0; bundle < 5; bundle++) {
    float x = -3.7f + bundle * 1.85f;
    SimBackgroundVoxelModelPoint a = StrawCrestPoint(x - .35f, .60f, false);
    SimBackgroundVoxelModelPoint b = StrawCrestPoint(x + .35f, .60f, false);
    SimBackgroundVoxelModelPoint c = StrawCrestPoint(x + .35f, 1, false);
    SimBackgroundVoxelModelPoint d = StrawCrestPoint(x - .35f, 1, false);
    a.y += .025f; b.y += .025f; c.y += .025f; d.y += .025f;
    AddFace(model, kSimVoxelMaterial_Trim, 215, a, b, c, d);
  }
}

static void AddStrawRoofPanel(SimBackgroundVoxelModel *model, int sector,
                               bool alternate) {
  float heights[] = {5.5f, alternate ? 7.0f : 9.0f, alternate ? 7.8f : 13.5f};
  for (int row = 0; row < 2; row++)
    AddFace(model, kSimVoxelMaterial_Roof, 226 + (sector % 3) * 5,
        StrawPoint(sector, heights[row], 0, alternate),
        StrawPoint(sector + 1, heights[row], 0, alternate),
        StrawPoint(sector + 1, heights[row + 1], 0, alternate),
        StrawPoint(sector, heights[row + 1], 0, alternate));
}

static void AddStrawBundleMark(SimBackgroundVoxelModel *model, int sector,
                               float z0, float z1, float t, bool roof, bool alternate) {
  SimBackgroundVoxelModelPoint a = roof ? StrawPoint(sector, z0, .035f, alternate) : StrawWallPoint(sector, z0, .025f);
  SimBackgroundVoxelModelPoint b = roof ? StrawPoint(sector + 1, z0, .035f, alternate) : StrawWallPoint(sector + 1, z0, .025f);
  SimBackgroundVoxelModelPoint c = roof ? StrawPoint(sector, z1, .035f, alternate) : StrawWallPoint(sector, z1, .025f);
  SimBackgroundVoxelModelPoint d = roof ? StrawPoint(sector + 1, z1, .035f, alternate) : StrawWallPoint(sector + 1, z1, .025f);
  float u = t + (roof ? .065f : .045f);
  AddFace(model, roof ? kSimVoxelMaterial_RoofLight : kSimVoxelMaterial_WallLight, 222,
      Point(a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t),
      Point(a.x + (b.x-a.x)*u, a.y + (b.y-a.y)*u, a.z + (b.z-a.z)*u),
      Point(c.x + (d.x-c.x)*u, c.y + (d.y-c.y)*u, c.z + (d.z-c.z)*u),
      Point(c.x + (d.x-c.x)*t, c.y + (d.y-c.y)*t, c.z + (d.z-c.z)*t));
}

static void BuildStrawShelter(SimBackgroundVoxelDetail detail, bool alternate,
                               int construction, SimBackgroundVoxelModel *model) {
  float height = alternate ? 7.8f : 13.5f;
  if (construction >= 0) {
    /* Short wall posts carry an eave binding and inward rafters. Thatch
     * fills out and overhangs this frame instead of continuing to ground. */
    for (int sector = 0; sector < 8; sector++) {
      SimBackgroundVoxelModelPoint eave = StrawWallPoint(sector, 5.8f, 0);
      if (!(sector & 1)) {
        AddShelterRod(model, StrawWallPoint(sector, 0, 0), eave, .17f, false);
        AddShelterRod(model, eave, StrawPoint(sector, height, 0, alternate), .17f, false);
      }
      AddFace(model, kSimVoxelMaterial_Wood, 210, eave,
          StrawWallPoint(sector + 1, 5.8f, 0),
          StrawWallPoint(sector + 1, 5.8f, -.23f), StrawWallPoint(sector, 5.8f, -.23f));
    }
    if (alternate) {
      /* Seat the crest on the rafters at the crown, with its crossbar
       * connected at both ends. */
      for (int side = 0; side < 2; side++) {
        int sector = side ? 6 : 2;
        SimBackgroundVoxelModelPoint foot = StrawPoint(sector, height, 0, true);
        AddShelterRod(model, foot, Point(side ? 12 : 4, 8, 11.8f), .15f, false);
      }
      AddShelterRod(model, Point(4, 8, 11.8f), Point(12, 8, 11.8f), .15f, false);
    }
    if (construction == 0) return;
    for (int sector = 2; sector < 6; sector += 2) {
      AddFace(model, kSimVoxelMaterial_Wall, 218,
          StrawWallPoint(sector, 0, .02f), StrawWallPoint(sector + 1, 0, .02f),
          StrawWallPoint(sector + 1, 5.8f, .02f), StrawWallPoint(sector, 5.8f, .02f));
      AddStrawRoofPanel(model, sector, alternate);
    }
    if (alternate) AddStrawCrest(model, true);
    return;
  }
  for (int sector = 0; sector < 8; sector++) {
    SimBackgroundVoxelModelPoint a = StrawWallPoint(sector, 0, 0);
    SimBackgroundVoxelModelPoint b = StrawWallPoint(sector + 1, 0, 0);
    SimBackgroundVoxelModelPoint ta = StrawWallPoint(sector, 5.8f, 0);
    SimBackgroundVoxelModelPoint tb = StrawWallPoint(sector + 1, 5.8f, 0);
    if (!sector) {
      float entry_y = StrawWallPoint(0, 3.7f, 0).y;
      SimBackgroundVoxelModelPoint left = Point(6.5f, entry_y, 3.7f);
      SimBackgroundVoxelModelPoint right = Point(9.5f, entry_y, 3.7f);
      AddFace(model, kSimVoxelMaterial_Wall, 228, a, Point(9.5f, a.y, 0), right, ta);
      AddFace(model, kSimVoxelMaterial_Wall, 224, Point(6.5f, b.y, 0), b, tb, left);
      AddFace(model, kSimVoxelMaterial_WallLight, 228, left, right, ta, tb);
      AddFace(model, kSimVoxelMaterial_Dark, 170,
          Point(6.5f, b.y-.04f, 0), Point(9.5f, a.y-.04f, 0),
          Point(9.5f, entry_y-.04f, 3.7f), Point(6.5f, entry_y-.04f, 3.7f));
      for (int side = 0; side < 2; side++) {
        float sign = side ? 1 : -1;
        AddFace(model, kSimVoxelMaterial_Trim, 230,
            Point(8+sign*1.5f, a.y+.025f, 0), Point(8+sign*1.7f, a.y+.025f, 0),
            Point(8+sign*1.7f, entry_y+.025f, 3.7f), Point(8+sign*1.5f, entry_y+.025f, 3.7f));
      }
      AddFace(model, kSimVoxelMaterial_Trim, 235,
          Point(6.5f, entry_y+.025f, 3.7f), Point(9.5f, entry_y+.025f, 3.7f),
          Point(9.5f, StrawWallPoint(0, 4, .025f).y, 4),
          Point(6.5f, StrawWallPoint(0, 4, .025f).y, 4));
    } else {
      AddFace(model, kSimVoxelMaterial_Wall, 218 + sector % 3 * 6, a, b, tb, ta);
    }
    /* Close the overhanging eave back to the wall, including the underside.
     * Alternating lower tips give the thick thatch a cut-bundle edge. */
    AddOutwardFace(model, kSimVoxelMaterial_Roof, 195, ta, tb,
        StrawPoint(sector + 1, 5.5f, 0, alternate), StrawPoint(sector, 5.5f, 0, alternate));
    AddStrawRoofPanel(model, sector, alternate);
    if (alternate) {
      AddFace(model, kSimVoxelMaterial_Roof, 224,
          StrawPoint(sector, height, 0, true), StrawPoint(sector + 1, height, 0, true),
          Point(8, 8, height), Point(8, 8, height));
    } else {
      /* A bound straw tuft replaces the exposed crossed teepee poles. */
      float angle0 = 1.178097245f + sector * .785398163f;
      float angle1 = angle0 + .785398163f;
      SimBackgroundVoxelModelPoint u = Point(8+cosf(angle0)*.7f, 8+sinf(angle0)*.7f, 14.15f);
      SimBackgroundVoxelModelPoint v = Point(8+cosf(angle1)*.7f, 8+sinf(angle1)*.7f, 14.15f);
      AddFace(model, kSimVoxelMaterial_Trim, 225,
          StrawPoint(sector, height, 0, false), StrawPoint(sector + 1, height, 0, false), v, u);
      AddFace(model, kSimVoxelMaterial_RoofLight, 222, u, v,
          Point(8.1f, 8, 14.6f), Point(8.1f, 8, 14.6f));
    }
  }
  if (alternate) AddStrawCrest(model, false);
  int rows = DetailChoice(detail, 0, 1, 2, 3);
  for (int row = 0; row < rows; row++) {
    float z = alternate ? 5.9f + row*.58f : 6.0f + row*1.5f;
    for (int sector = 0; sector < 8; sector++)
      AddFace(model, kSimVoxelMaterial_Roof, 175,
          StrawPoint(sector, z, .035f, alternate), StrawPoint(sector + 1, z, .035f, alternate),
          StrawPoint(sector + 1, z+.14f, .035f, alternate), StrawPoint(sector, z+.14f, .035f, alternate));
  }
  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    int bundles = DetailChoice(detail, 0, 1, 2, 3);
    for (int sector = 0; sector < 8; sector++)
      for (int bundle = 0; bundle < bundles; bundle++) {
        float z0 = alternate ? 5.75f + bundle*.6f : 5.8f + bundle*2.5f;
        float shoulder = alternate ? 7.0f : 9.0f;
        float z1 = fminf(z0 + (alternate ? .5f : 1.15f), z0 < shoulder ? shoulder : height);
        AddStrawBundleMark(model, sector, z0, z1, .2f + (bundle & 1)*.35f, true, alternate);
        AddStrawBundleMark(model, sector, z0+(z1-z0)*.15f, z1, bundle & 1 ? .12f : .68f, true, alternate);
        if (sector)
          AddStrawBundleMark(model, sector, .6f, 5.2f, .18f+bundle*.23f, false, alternate);
      }
  }
}

static void BuildCanvasShelter(SimBackgroundVoxelDetail detail, bool alternate,
                                int construction, SimBackgroundVoxelModel *model) {
  const float eave = 5.7f, peak = 11.4f;
  if (construction >= 0) {
    /* A standing pavilion frame: upright side posts, sloped rafters and a
     * ridge. Finished canvas covers the framing rather than exposing A-bars. */
    for (int end = 0; end < 2; end++) {
      float y = end ? 13.9f : 2.9f;
      for (int side = 0; side < 2; side++) {
        float x = side ? 14.4f : 1.6f;
        AddShelterPole(model, Point(x,y,0), Point(x,y,eave), .15f);
        AddShelterPole(model, Point(x,y,eave), Point(8,y,peak-.2f), .15f);
      }
    }
    AddShelterRod(model, Point(8,2.9f,peak-.2f), Point(8,13.9f,peak-.2f), .12f, false);
    if (construction == 0) return;
  }
  for (int side = 0; side < (construction == 1 ? 1 : 2); side++) {
    float x = side ? 14.5f : 1.5f;
    for (int panel = 0; panel < 2; panel++) {
      float y0 = panel ? 8.4f : 2.8f, y1 = panel ? 14.0f : 8.4f;
      float ridge0 = panel ? peak-.4f : peak, ridge1 = panel ? peak : peak-.4f;
      AddFace(model, side ? kSimVoxelMaterial_Wall : kSimVoxelMaterial_WallLight, 225,
          Point(x,y0,0), Point(x,y1,0), Point(x,y1,eave), Point(x,y0,eave));
      AddFace(model, side ? kSimVoxelMaterial_Roof : kSimVoxelMaterial_RoofLight, 240,
          Point(x-.18f*(side?-1:1),y0,eave), Point(x-.18f*(side?-1:1),y1,eave),
          Point(8,y1,ridge1), Point(8,y0,ridge0));
    }
  }
  AddFrontBand(model, 1.5f, 14.5f, 2.8f, 0, eave, kSimVoxelMaterial_Wall);
  AddFace(model, kSimVoxelMaterial_WallLight, 205,
      Point(1.32f,2.8f,eave), Point(14.68f,2.8f,eave), Point(8,2.8f,peak), Point(8,2.8f,peak));
  if (construction == 1) return;
  /* Split vertical entrance curtains beneath a full-height peaked end. */
  AddFace(model, kSimVoxelMaterial_WallLight, 235,
      Point(1.5f,14,0), Point(5.6f,14.1f,0), Point(7.1f,14.02f,5.3f), Point(1.5f,14,eave));
  AddFace(model, kSimVoxelMaterial_Wall, 225,
      Point(10.4f,14.1f,0), Point(14.5f,14,0), Point(14.5f,14,eave), Point(8.9f,14.02f,5.3f));
  AddFrontBand(model, 5.6f, 10.4f, 13.98f, 0, 5.7f, kSimVoxelMaterial_Dark);
  AddFace(model, kSimVoxelMaterial_WallLight, 236,
      Point(1.32f,14,eave), Point(14.68f,14,eave), Point(8,14,peak), Point(8,14,peak));
  AddFace(model, kSimVoxelMaterial_WallLight, 235,
      Point(1.5f,14,eave), Point(7.1f,14.02f,5.3f),
      Point(8.9f,14.02f,5.3f), Point(14.5f,14,eave));
  if (alternate) {
    for (int side = 0; side < 2; side++) {
      float x = side ? 10.8f : 5.2f;
      AddShelterPole(model, Point(x,15.3f,0), Point(x,15.3f,5.3f), .16f);
    }
    AddFace(model, kSimVoxelMaterial_RoofLight, 240,
        Point(5,15.5f,5.3f), Point(11,15.5f,5.3f), Point(11,14,5.7f), Point(5,14,5.7f));
    AddFrontBand(model, 5, 11, 15.51f, 5, 5.3f, kSimVoxelMaterial_WallLight);
  }
  if (detail >= kSimBackgroundVoxelDetail_Balanced)
    for (int side = 0; side < 2; side++) {
      float x = side ? 15.4f : .6f;
      AddShelterRod(model, Point(x,14.9f,0), Point(side?14.5f:1.5f,13.2f,eave), .045f, false);
    }
  /* Subtle upright cloth seams distinguish the walls from solid masonry. */
  for (int seam = 0; seam < (int)detail; seam++) {
    float y = 5.1f + seam * 2.6f;
    for (int side = 0; side < 2; side++) {
      float x = side ? 14.52f : 1.48f;
      AddFace(model, kSimVoxelMaterial_WallLight, 205,
          Point(x,y,.2f), Point(x,y+.10f,.2f), Point(x,y+.10f,eave), Point(x,y,eave));
    }
  }
}

static void BuildTimberHouse(SimBackgroundVoxelDetail detail,
                             SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 1.7f, 2.5f, 0.0f, 14.3f, 15.0f, 1.4f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.8f, 3.5f, 1.4f, 13.2f, 14.5f, 7.5f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 1.5f, 14.5f, 2.5f, 15.0f, 7.5f, 11.5f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddFrontBand(model, 6.5f, 9.5f, 14.60f, 1.4f, 6.2f, kSimVoxelMaterial_Dark);
  AddFrontBand(model, 2.8f, 6.4f, 14.54f, 3.0f, 3.65f, kSimVoxelMaterial_Wood);
  AddFrontBand(model, 9.6f, 13.2f, 14.54f, 3.0f, 3.65f, kSimVoxelMaterial_Wood);
  AddFrontBand(model, 2.8f, 3.5f, 14.56f, 1.4f, 7.5f, kSimVoxelMaterial_Wood);
  AddFrontBand(model, 12.5f, 13.2f, 14.56f, 1.4f, 7.5f, kSimVoxelMaterial_Wood);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddFrontBand(model, 3.5f, 5.8f, 14.60f, 3.4f, 5.9f, kSimVoxelMaterial_Dark);
  AddFrontBand(model, 10.2f, 12.5f, 14.60f, 3.4f, 5.9f, kSimVoxelMaterial_Dark);
  AddFrontBand(model, 2.8f, 13.2f, 14.54f, 6.9f, 7.5f, kSimVoxelMaterial_Wood);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddFrontBand(model, 3.5f, 5.8f, 14.62f, 3.15f, 3.4f, kSimVoxelMaterial_Wood);
    AddFrontBand(model, 10.2f, 12.5f, 14.62f, 3.15f, 3.4f, kSimVoxelMaterial_Wood);
  }
}

static void BuildBloodpoolHouse(SimBackgroundVoxelDetail detail,
                                SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 1.2f, 2.0f, 0.0f, 14.8f, 15.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.0f, 3.0f, 1.5f, 14.0f, 14.5f, 9.0f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 0.8f, 15.2f, 2.0f, 15.0f, 9.0f, 14.5f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 6.5f, 14.1f, 1.5f, 10.0f, 15.4f, 7.4f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 10.8f, 14.0f, 4.0f, 13.0f, 15.2f, 7.0f,
                 kSimVoxelMaterial_Dark);
  if (detail >= kSimBackgroundVoxelDetail_High)
    AddStandardBox(model, 7.2f, 5.0f, 13.7f, 8.8f, 7.0f, 15.0f,
                   kSimVoxelMaterial_Dark);
}

static void BuildAdobeHouse(SimBackgroundVoxelDetail detail,
                            SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 1.0f, 2.0f, 0.0f, 15.0f, 15.0f, 1.3f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 2.0f, 3.0f, 1.3f, 14.0f, 14.5f, 9.3f,
                 kSimVoxelMaterial_Wall);
  AddStandardBox(model, 1.5f, 2.5f, 9.3f, 14.5f, 15.0f, 10.2f,
                 kSimVoxelMaterial_Roof);
  AddStandardBox(model, 2.0f, 3.0f, 10.2f, 14.0f, 4.0f, 11.0f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 2.0f, 13.5f, 10.2f, 14.0f, 14.5f, 11.0f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 2.0f, 4.0f, 10.2f, 3.0f, 13.5f, 11.0f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 13.0f, 4.0f, 10.2f, 14.0f, 13.5f, 11.0f,
                 kSimVoxelMaterial_Trim);
  AddFrontBand(model, 6.2f, 9.8f, 14.60f, 1.3f, 7.2f, kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddFrontBand(model, 3.2f, 5.2f, 14.60f, 4.0f, 6.4f, kSimVoxelMaterial_Dark);
  AddFrontBand(model, 10.8f, 12.8f, 14.60f, 4.0f, 6.4f, kSimVoxelMaterial_Dark);
  if (detail >= kSimBackgroundVoxelDetail_High)
    AddStandardBox(model, 5.6f, 4.0f, 10.1f, 10.4f, 8.0f, 10.8f,
                   kSimVoxelMaterial_WallLight);
}

static void BuildStoneHouse(SimBackgroundVoxelDetail detail,
                            SimBackgroundVoxelModel *model) {
  /* Northwall and Kasandora both select native family $0A: the same flat
   * terrace, with the regional palette and snow pass providing the climate. */
  BuildAdobeHouse(detail, model);
}

static void BuildAitosHouse(SimBackgroundVoxelDetail detail,
                            SimBackgroundVoxelModel *model) {
  /* Aitos' final stone dwelling is a squat, flat-roofed highland house. The
   * earlier model borrowed a pair of Fillmore-style gables, changing the
   * source silhouette into a peaked chalet. Keep the roof slab broad and the
   * parapet low so it reads as the original masonry terrace at town scale. */
  AddStandardBox(model, 1.0f, 2.0f, 0.0f, 15.0f, 15.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 1.8f, 3.0f, 1.5f, 14.2f, 14.5f, 8.7f,
               kSimVoxelMaterial_Wall);
  AddStandardBox(model, 0.8f, 2.0f, 8.7f, 15.2f, 15.0f, 9.5f,
                 kSimVoxelMaterial_Roof);
  /* Four independent parapet runs leave the terrace centre visibly flat
   * instead of disguising another solid upper storey as a roof. */
  AddStandardBox(model, 0.8f, 2.0f, 9.5f, 15.2f, 3.2f, 10.5f,
                 kSimVoxelMaterial_RoofLight);
  AddStandardBox(model, 0.8f, 13.8f, 9.5f, 15.2f, 15.0f, 10.5f,
                 kSimVoxelMaterial_RoofLight);
  AddStandardBox(model, 0.8f, 3.2f, 9.5f, 2.0f, 13.8f, 10.5f,
                 kSimVoxelMaterial_Roof);
  AddStandardBox(model, 14.0f, 3.2f, 9.5f, 15.2f, 13.8f, 10.5f,
                 kSimVoxelMaterial_RoofLight);
  AddFrontBand(model, 5.8f, 9.2f, 14.60f, 1.5f, 6.7f, kSimVoxelMaterial_Dark);
  for (int beam = 0; beam < 4; beam++) {
    float x = 3.0f + beam * 3.0f;
    AddFace(model, kSimVoxelMaterial_Wood, 225,
            Point(x, 3.2f, 9.54f), Point(x + .6f, 3.2f, 9.54f),
            Point(x + .6f, 13.8f, 9.54f), Point(x, 13.8f, 9.54f));
  }
  AddFrontBand(model, 1.8f, 5.7f, 14.54f, 3.1f, 3.7f, kSimVoxelMaterial_Wood);
  AddFrontBand(model, 9.3f, 14.2f, 14.54f, 3.1f, 3.7f, kSimVoxelMaterial_Wood);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddFrontBand(model, 2.8f, 4.8f, 14.60f, 3.8f, 6.2f, kSimVoxelMaterial_Dark);
  AddFrontBand(model, 11.2f, 13.2f, 14.60f, 3.8f, 6.2f, kSimVoxelMaterial_Dark);
  AddFrontBand(model, 1.8f, 14.2f, 14.54f, 7.9f, 8.7f, kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int course = 0; course < 3; course++) {
      float z = 2.5f + course * 1.8f;
      AddFrontBand(model, 1.8f, 5.0f, 14.53f, z, z + 0.35f,
                     kSimVoxelMaterial_WallLight);
      AddFrontBand(model, 11.0f, 14.2f, 14.53f, z, z + 0.35f,
                     kSimVoxelMaterial_WallLight);
    }
    /* A low square roof hatch adds close-range depth without introducing a
     * ridge, pitch, or triangular end face. */
    AddStandardBox(model, 10.4f, 5.0f, 9.5f, 13.0f, 8.0f, 10.3f,
                   kSimVoxelMaterial_WallLight);
  }
}

static void AddStiltStair(SimBackgroundVoxelModel *model, SimBackgroundVoxelDetail detail) {
  int steps = DetailChoice(detail, 3, 5, 6, 6);
  const float x0 = 2, x1 = 7.6f, y0 = 14.85f, y1 = 16.6f, floor = 5.3f;
  for (int step = 0; step < steps; step++) {
    float a = x0 + (x1-x0)*step/steps, b = x0 + (x1-x0)*(step+1)/steps;
    float z0 = floor*step/steps, z1 = floor*(step+1)/steps;
    AddFace(model, kSimVoxelMaterial_Wood, 220,
        Point(a,y0,z0), Point(a,y1,z0), Point(a,y1,z1), Point(a,y0,z1));
    AddFace(model, kSimVoxelMaterial_Trim, 245,
        Point(a,y0,z1), Point(b,y0,z1), Point(b,y1,z1), Point(a,y1,z1));
  }
  AddFace(model, kSimVoxelMaterial_Trim, 245,
      Point(x1,y0,floor), Point(9.7f,y0,floor), Point(9.7f,y1,floor), Point(x1,y1,floor));
  for (int side = 0; side < 2; side++) {
    float y = side ? y1 : y0;
    AddShelterRod(model, Point(x0,y,.1f), Point(x1,y,4.8f), .18f, false);
  }
}

static void BuildMarahnaStiltHouse(SimBackgroundVoxelDetail detail,
                                   SimBackgroundVoxelModel *model) {
  for (int post = 0; post < 4; post++) {
    float x = post & 1 ? 12.5f : 2.5f;
    float y = post & 2 ? 13.5f : 4.0f;
    AddStandardBox(model, x, y, 0.0f, x + 1.0f, y + 1.0f, 4.8f,
                   kSimVoxelMaterial_Wood);
  }
  AddStandardBox(model, 1.5f, 3.0f, 4.3f, 14.5f, 15.0f, 5.3f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.5f, 4.0f, 5.3f, 13.5f, 14.5f, 9.0f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 0.8f, 15.2f, 2.5f, 15.0f, 9.0f, 12.5f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddFrontBand(model, 6.3f, 9.7f, 14.60f, 5.3f, 8.3f, kSimVoxelMaterial_Dark);
  AddStiltStair(model, detail);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 3.2f, 14.0f, 6.1f, 5.4f, 15.2f, 8.1f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 10.6f, 14.0f, 6.1f, 12.8f, 15.2f, 8.1f,
                 kSimVoxelMaterial_Dark);
  if (detail >= kSimBackgroundVoxelDetail_High)
    AddStandardBox(model, 1.0f, 14.2f, 8.6f, 15.0f, 15.5f, 9.3f,
                   kSimVoxelMaterial_Wood);
}

static void BuildMarahnaLogCabin(SimBackgroundVoxelDetail detail,
                                 SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 1.4f, 2.5f, 0.0f, 14.6f, 15.0f, 1.2f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.2f, 3.5f, 1.2f, 13.8f, 14.5f, 8.0f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 0.8f, 15.2f, 2.2f, 15.0f, 8.0f, 12.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 6.3f, 14.0f, 1.2f, 9.7f, 15.3f, 6.8f,
                 kSimVoxelMaterial_Dark);
  for (int course = 0; course < 3; course++) {
    float z = 2.1f + course * 1.9f;
    AddFrontBand(model, 2.2f, 6.2f, 14.55f, z, z + .65f, kSimVoxelMaterial_Wood);
    AddFrontBand(model, 9.8f, 13.8f, 14.55f, z, z + .65f, kSimVoxelMaterial_Wood);
    AddFace(model, kSimVoxelMaterial_Wood, 205,
            Point(13.85f, 3.5f, z), Point(13.85f, 14.5f, z),
            Point(13.85f, 14.5f, z + .65f), Point(13.85f, 3.5f, z + .65f));
  }
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 3.2f, 14.0f, 3.7f, 5.2f, 15.3f, 6.1f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 10.8f, 14.0f, 3.7f, 12.8f, 15.3f, 6.1f,
                 kSimVoxelMaterial_Dark);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddStandardBox(model, 1.5f, 14.5f, 7.4f, 14.5f, 15.7f, 8.2f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 2.0f, 3.0f, 7.2f, 3.0f, 14.5f, 8.3f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 13.0f, 3.0f, 7.2f, 14.0f, 14.5f, 8.3f,
                   kSimVoxelMaterial_Wood);
  }
}

static int RoofSlice(const SimBackgroundVoxelModelFace *face, float z,
                       SimBackgroundVoxelModelPoint out[2]) {
  int count = 0;
  for (int edge = 0; edge < 4; edge++) {
    const SimBackgroundVoxelModelPoint a = face->points[edge];
    const SimBackgroundVoxelModelPoint b = face->points[(edge + 1) & 3];
    if ((a.z < z && b.z > z) || (b.z < z && a.z > z)) {
      float t = (z - a.z) / (b.z - a.z);
      if (count < 2) out[count] = Point(a.x + (b.x - a.x) * t,
                                       a.y + (b.y - a.y) * t, z + .015f);
      count++;
    }
  }
  return count;
}

static void AddHouseRoofCourses(SimBackgroundVoxelModel *model,
                                 SimBackgroundVoxelDetail detail, uint16_t first_face) {
  uint16_t original_faces = model->face_count;
  int courses = DetailChoice(detail, 2, 3, 4, 5);
  for (uint16_t f = first_face; f < original_faces; f++) {
    SimBackgroundVoxelModelFace roof = model->faces[f];
    if (roof.material != kSimVoxelMaterial_Roof && roof.material != kSimVoxelMaterial_RoofLight)
      continue;
    float low = FLT_MAX, high = -FLT_MAX, area = 0;
    for (int v = 0; v < 4; v++) {
      low = fminf(low, roof.points[v].z); high = fmaxf(high, roof.points[v].z);
      int next = (v + 1) & 3;
      area += roof.points[v].x * roof.points[next].y - roof.points[next].x * roof.points[v].y;
    }
    if (high - low < 2.0f || fabsf(area) < .01f) continue;
    for (int course = 1; course <= courses; course++) {
      float z = low + (high - low) * course / (courses + 1);
      SimBackgroundVoxelModelPoint a[2], b[2];
      if (RoofSlice(&roof, z, a) != 2 || RoofSlice(&roof, z + (high - low) * .045f, b) != 2)
        continue;
      /* Stagger the short bundles/tiles at close range. Continuous narrow
       * strips alone read as loose bright lines on the native palette. */
      int patches = DetailChoice(detail, 1, 2, 4, 6);
      for (int patch = 0; patch < patches; patch++) {
        float start = patches == 1 ? 0 : (patch + .1f + .35f * (course & 1)) / patches;
        float end = patches == 1 ? 1 : fminf(.99f, start + .68f / patches);
        SimBackgroundVoxelModelPoint p[4];
        for (int row = 0; row < 2; row++) {
          SimBackgroundVoxelModelPoint *edge = row ? b : a;
          p[row * 2] = Point(edge[0].x + (edge[1].x - edge[0].x) * start,
              edge[0].y + (edge[1].y - edge[0].y) * start, edge[0].z);
          p[row * 2 + 1] = Point(edge[0].x + (edge[1].x - edge[0].x) * end,
              edge[0].y + (edge[1].y - edge[0].y) * end, edge[0].z);
        }
        AddFace(model, kSimVoxelMaterial_RoofLight, 172, p[0], p[1], p[3], p[2]);
      }
    }
  }
}

static void AddTerraceCourses(const SimBackgroundVoxelObject *object,
                               SimBackgroundVoxelDetail detail,
                               SimBackgroundVoxelModel *model) {
  SimBackgroundVoxelHouseStyle style = SimBackgroundVoxelRegion_ObjectHouseStyle(object);
  if (style != kSimBackgroundHouseStyle_Adobe && style != kSimBackgroundHouseStyle_Stone &&
      style != kSimBackgroundHouseStyle_Aitos) return;
  bool timber = style == kSimBackgroundHouseStyle_Aitos;
  int rows = DetailChoice(detail, 2, 3, 4, 5);
  float z = timber ? 9.56f : 10.23f;
  for (int row = 1; row <= rows; row++) {
    float y = 4.0f + 9.0f * row / (rows + 1);
    AddFace(model, timber ? kSimVoxelMaterial_Wood : kSimVoxelMaterial_WallLight,
            timber ? 225 : 210, Point(3.0f, y, z), Point(13.0f, y, z),
            Point(13.0f, y + .23f, z), Point(3.0f, y + .23f, z));
  }
}

static void BuildHouse(const SimBackgroundVoxelObject *object,
                       SimBackgroundVoxelDetail detail,
                       SimBackgroundVoxelModel *model) {
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    int phase = object->animation_phase > 0 ? 1 : 0;
    SimBackgroundVoxelHouseStyle family = SimBackgroundVoxelRegion_ObjectHouseStyle(object);
    if (family == kSimBackgroundHouseStyle_Yurt) {
      BuildStrawShelter(detail, (object->flags & kSimBackgroundVoxel_AlternateFacing) != 0, phase, model);
      return;
    }
    if (family == kSimBackgroundHouseStyle_Tent || family == kSimBackgroundHouseStyle_WhiteTent) {
      BuildCanvasShelter(detail, (object->flags & kSimBackgroundVoxel_AlternateFacing) != 0, phase, model);
      return;
    }
    float wall_height = phase ? 5.5f : 2.5f;
    float eave = phase ? 8.0f : 5.0f;
    float ridge = phase ? 12.0f : 8.0f;
    AddStandardBox(model, 1.5f, 2.5f, 0, 14.5f, 15.0f, .8f, kSimVoxelMaterial_Trim);
    AddStandardBox(model, 2, 3, .8f, 14, 4, wall_height, kSimVoxelMaterial_Wall);
    AddStandardBox(model, 2, 13.5f, .8f, 6, 14.5f, wall_height, kSimVoxelMaterial_Wall);
    AddStandardBox(model, 10, 13.5f, .8f, 14, 14.5f, wall_height, kSimVoxelMaterial_Wall);
    AddGableFrame(model, 2.5f, 13.5f, 3.5f, eave, ridge, false);
    AddGableFrame(model, 2.5f, 13.5f, 14.0f, eave, ridge, false);
    AddStandardBox(model, 7.55f, 3.0f, ridge - .45f, 8.45f, 14.5f,
                   ridge + .45f, kSimVoxelMaterial_Wood);
    return;
  }
  switch (SimBackgroundVoxelRegion_ObjectHouseStyle(object)) {
    case kSimBackgroundHouseStyle_Tent:
      BuildCanvasShelter(detail, (object->flags & kSimBackgroundVoxel_AlternateFacing) != 0, -1, model);
      break;
    case kSimBackgroundHouseStyle_Timber:
      BuildTimberHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Fillmore:
      BuildFillmoreHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Bloodpool:
      BuildBloodpoolHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Yurt:
      BuildStrawShelter(detail, (object->flags & kSimBackgroundVoxel_AlternateFacing) != 0, -1, model);
      break;
    case kSimBackgroundHouseStyle_WhiteTent:
      BuildCanvasShelter(detail, (object->flags & kSimBackgroundVoxel_AlternateFacing) != 0, -1, model);
      break;
    case kSimBackgroundHouseStyle_Adobe:
      BuildAdobeHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Stone:
      BuildStoneHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Aitos:
      BuildAitosHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_MarahnaStilt:
      BuildMarahnaStiltHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_MarahnaLogCabin:
      BuildMarahnaLogCabin(detail, model);
      break;
    case kSimBackgroundHouseStyle_Count:
      BuildFillmoreHouse(detail, model);
      break;
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

  int columns = DetailChoice(detail, 4, 6, 6, 8);
  for (int i = 0; i < columns; i++) {
    int half = columns / 2;
    float x = (half == 2 ? 4.0f : 3.5f) +
        (half == 2 ? 6.0f : 7.5f) * (i % half) / (half - 1) +
        (i >= half ? 16.0f : 0.0f);
    float width = detail == kSimBackgroundVoxelDetail_Low ? 2.0f :
        detail == kSimBackgroundVoxelDetail_Ultra ? 1.1f : 1.4f;
    AddStandardBox(model, x, 28.8f, 4.0f,
                   x + width, 31.0f, 15.7f,
                   kSimVoxelMaterial_WallLight);
    if (detail != kSimBackgroundVoxelDetail_Low) {
      AddStandardBox(model, x - 0.4f, 28.5f, 3.0f,
                     x + width + .4f, 31.3f, 4.3f, kSimVoxelMaterial_Trim);
      AddStandardBox(model, x - 0.4f, 28.5f, 15.0f,
                     x + width + .4f, 31.3f, 16.3f, kSimVoxelMaterial_Trim);
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
  }
}

/* A swept, flared sail. The purple seam is part of its surface, following
 * the same curve on both sides rather than a straight spar across the rotor. */
static SimBackgroundVoxelModelPoint BladePoint(float cx, float cy, float cz,
    float dx, float dz, float t, float across) {
  float radius = 2.2f + 7.8f*t;
  float width = .42f + 1.40f*t;
  float sweep = 1.15f*t*t;
  float offset = sweep + across*width;
  return Point(cx+dx*radius-dz*offset, cy, cz+dz*radius+dx*offset);
}

static void AddBlade(SimBackgroundVoxelModel *model, SimBackgroundVoxelDetail detail,
                     float cx, float cy, float cz, float dx, float dz) {
  int segments = DetailChoice(detail, 1, 2, 2, 3);
  for (int segment = 0; segment < segments; segment++) {
    float t0 = (float)segment/segments, t1 = (float)(segment+1)/segments;
    SimBackgroundVoxelModelPoint front[4] = {
      BladePoint(cx,cy+.6f,cz,dx,dz,t0,-1), BladePoint(cx,cy+.6f,cz,dx,dz,t1,-1),
      BladePoint(cx,cy+.6f,cz,dx,dz,t1,1), BladePoint(cx,cy+.6f,cz,dx,dz,t0,1)};
    SimBackgroundVoxelModelPoint back[4];
    for (int v=0; v<4; v++) {back[v]=front[v]; back[v].y=cy;}
    AddFace(model,kSimVoxelMaterial_Blade,255,front[0],front[1],front[2],front[3]);
    AddFace(model,kSimVoxelMaterial_Blade,205,back[3],back[2],back[1],back[0]);
    AddFace(model,kSimVoxelMaterial_Blade,218,back[0],back[1],front[1],front[0]);
    AddFace(model,kSimVoxelMaterial_Blade,232,back[2],back[3],front[3],front[2]);
    for (int side=0; side<2; side++) {
      float y = side ? cy+.63f : cy-.03f;
      AddFace(model,kSimVoxelMaterial_BladeStripe,245,
          BladePoint(cx,y,cz,dx,dz,t0,.40f), BladePoint(cx,y,cz,dx,dz,t1,.40f),
          BladePoint(cx,y,cz,dx,dz,t1,.70f), BladePoint(cx,y,cz,dx,dz,t0,.70f));
    }
    if (segment+1 == segments)
      AddFace(model,kSimVoxelMaterial_Blade,232,back[1],back[2],front[2],front[1]);
  }
}

enum {
  /* $DBF1 / $DBFE / $DC0B for the built mill, $DBBD / $DBCA / $DBD7 while it
   * is going up. Both cycles are three frames long; see the windmill note on
   * SimBackgroundVoxelObject::animation_phase. */
  kWindmillFrameCount = 3,
};

/* Depth of the rotor plane. A blade is allowed to overhang the one-cell plot:
 * forcing its front face inside y=16 left less than half a model pixel between
 * it and the timber fascia. At native resolution that gap rasterized as the
 * frame winning isolated blade pixels. The authored overhang is bounded and
 * culling already carries a projected-object margin, so physical separation
 * is both cheaper and more stable than a per-material depth bias. */
static const float kWindmillBladePlane = 15.8f;
static const float kWindmillHubCapCover = 1.0f;

/* Windmill bodies use a shallow round plan within their native 2x1 plot.
 * Scale the octagon's depth without turning its walls into a square frame. */
static void AddWindmillRoundSection(SimBackgroundVoxelModel *model,
                                    float r0, float r1, float z0, float z1,
                                    SimBackgroundVoxelMaterial material) {
  uint16_t first = model->face_count;
  AddOctagonalFrustum(model, 16.0f, 7.0f, r0, r1, z0, z1, material);
  for (uint16_t face = first; face < model->face_count; face++)
    for (int vertex = 0; vertex < 4; vertex++) {
      SimBackgroundVoxelModelPoint *p = &model->faces[face].points[vertex];
      p->y = 7.0f + (p->y - 7.0f) * 0.75f;
    }
}

static SimBackgroundVoxelModelPoint WindmillWallPoint(float x, float z) {
  float radius = 7.5f - (z - 2.0f) * (1.1f / 20.0f);
  return Point(x, 7.0f + radius * 0.75f -
               fabsf(x - 16.0f) * (0.75f * 0.2929f / 0.7071f) + 0.04f, z);
}

static void BuildWindmill(const SimBackgroundVoxelObject *object,
                          SimBackgroundVoxelDetail detail,
                          SimBackgroundVoxelModel *model) {
  int phase = object->animation_phase % kWindmillFrameCount;
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    float height = phase == 0 ? 7.0f : phase == 1 ? 16.0f : 22.0f;
    float radius = 7.5f - (height - 2.0f) * (1.1f / 20.0f);
    AddWindmillRoundSection(model, 8, 7.5f, 0, 2, kSimVoxelMaterial_Trim);
    AddWindmillRoundSection(model, 7.5f, radius, 2, height, kSimVoxelMaterial_Wall);
    if (phase == 2) {
      AddWindmillRoundSection(model, 7.2f, 0, 22, 31, kSimVoxelMaterial_Roof);
      AddRoofedBox(model, 12.7f, 11.4f, 0, 19.3f, 16.4f, 5.8f, kSimVoxelMaterial_WallLight);
      AddGableRoofX(model, 12.2f, 19.8f, 11.2f, 16.6f, 5.8f, 8.3f,
                    kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
      AddFrontBand(model, 14, 18, 16.65f, 0, 5.4f, kSimVoxelMaterial_Dark);
    } else {
      AddWindmillRoundSection(model, radius, 0, height, height, kSimVoxelMaterial_WallLight);
      /* Scaffolding hugs the round tower; no rectangular foundation slab. */
      for (int side = 0; side < 2; side++) {
        float x = side ? 24.0f : 7.0f;
        AddStandardBox(model, x, 9.5f, 0, x + 1, 10.5f, height + 3, kSimVoxelMaterial_Wood);
      }
      AddStandardBox(model, 7, 9.5f, height + 1, 25, 10.5f, height + 2, kSimVoxelMaterial_Wood);
    }
    return;
  }

  if (detail != kSimBackgroundVoxelDetail_Low)
    AddWindmillRoundSection(model, 8.0f, 7.5f, 0.0f, 2.0f, kSimVoxelMaterial_Trim);
  AddWindmillRoundSection(model, detail == kSimBackgroundVoxelDetail_Low ? 8.0f : 7.5f,
      6.4f, detail == kSimBackgroundVoxelDetail_Low ? 0.0f : 2.0f, 22.0f, kSimVoxelMaterial_Wall);
  AddWindmillRoundSection(model, 7.2f, 0.0f, 22.0f, 31.0f,
                          kSimVoxelMaterial_Roof);
  /* The source has a low entrance projecting in front of the round tower.
   * It stays below the swept rotor, including its landing and gabled hood. */
  AddRoofedBox(model, 12.7f, 11.4f, 0.0f, 19.3f, 16.4f, 5.8f,
               kSimVoxelMaterial_WallLight);
  AddGableRoofX(model, 12.2f, 19.8f, 11.2f, 16.6f, 5.8f, 8.3f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddFace(model, kSimVoxelMaterial_Dark, 232,
          Point(14.0f, 16.65f, 0.0f), Point(18.0f, 16.65f, 0.0f),
          Point(18.0f, 16.65f, 5.4f), Point(14.0f, 16.65f, 5.4f));
  /* A narrow axle joins the tower to the hub without entering the blades'
   * swept annulus. Its front overlaps the hub's back by 0.05 units. */
  AddStandardBox(model, 15.3f, 11.0f, 20.3f, 16.7f, 16.2f, 21.7f,
                 kSimVoxelMaterial_Wood);

  /* The wheel is four-fold symmetric, so its whole visual period is the 90
   * degrees the authentic art divides into three steps. The per-record offset
   * that used to be the only variation stays as a fixed phase difference
   * between neighbouring mills. */
  const float quarter_turn = 1.57079633f;
  const bool alternate = (object->record_slot & 1u) != 0;
  float angle = (alternate ? quarter_turn * 0.5f : 0.0f) +
      quarter_turn * (float)phase / (float)kWindmillFrameCount;
  const float dx = cosf(angle);
  const float dz = sinf(angle);
  /* The blades span y = plane .. plane + 0.6. The round wall, attached
   * markings and entrance remain behind or below that swept volume. */
  AddBlade(model, detail, 16.0f, kWindmillBladePlane, 21.0f, dx, dz);
  AddBlade(model, detail, 16.0f, kWindmillBladePlane, 21.0f, -dz, dx);
  AddBlade(model, detail, 16.0f, kWindmillBladePlane, 21.0f, -dx, -dz);
  AddBlade(model, detail, 16.0f, kWindmillBladePlane, 21.0f, dz, -dx);
  AddStandardBox(model, 14.1f, kWindmillBladePlane + 0.35f, 19.1f, 17.9f,
                 kWindmillBladePlane + kWindmillHubCapCover, 22.7f,
                 kSimVoxelMaterial_Wood);

  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    AddStandardBox(model, 12.5f, 15.6f, 0.0f, 19.5f, 16.75f, 0.8f,
                   kSimVoxelMaterial_Paving);
    AddWindmillRoundSection(model, 6.76f, 6.70f, 17.4f, 18.4f,
                            kSimVoxelMaterial_Trim);
  }
  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int side = 0; side < 2; side++) {
      float x = side ? 18.4f : 12.0f;
      AddFace(model, kSimVoxelMaterial_Dark, 232,
              WindmillWallPoint(x, 10.0f), WindmillWallPoint(x + 1.6f, 10.0f),
              WindmillWallPoint(x + 1.6f, 14.0f), WindmillWallPoint(x, 14.0f));
    }
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int level = 0; level < 4; level++) {
      float z = 3.0f + level * 3.5f;
      for (int side = 0; side < 2; side++) {
        float x = side ? 18.8f : 11.2f;
        AddFace(model, kSimVoxelMaterial_Trim, 214,
                WindmillWallPoint(x, z), WindmillWallPoint(x + 1.1f, z),
                WindmillWallPoint(x + 1.1f, z + 0.5f), WindmillWallPoint(x, z + 0.5f));
      }
    }
  }
}

static void BuildFactory(const SimBackgroundVoxelObject *object,
                         SimBackgroundVoxelDetail detail,
                         SimBackgroundVoxelModel *model) {
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    /* The same open U-plan as the finished factory, with partial masonry
     * and a roof truss on each arm. Ground remains visible in the courtyard. */
    AddStandardBox(model, 1, 1, 0, 21, 8.5f, 3, kSimVoxelMaterial_Wall);
    AddStandardBox(model, 1, 23.5f, 0, 21, 31, 3, kSimVoxelMaterial_Wall);
    AddStandardBox(model, 21, 1, 0, 31, 31, 3, kSimVoxelMaterial_Wall);
    AddGableFrame(model, 2, 20, 8, 8, 11.5f, false);
    AddGableFrame(model, 2, 20, 30.5f, 8, 11.5f, false);
    for (int arm = 0; arm < 2; arm++) {
      float y = arm ? 23.5f : 1.0f;
      AddRoofedBox(model, 10.55f, y, 3, 11.45f, y + .9f, 11.5f,
                   kSimVoxelMaterial_Wood);
      AddBox(model, 10.55f, y, 11.05f, 11.45f, y + 7.5f, 11.95f,
             kSimVoxelMaterial_Wood, kBoxFace_East | kBoxFace_West | kBoxFace_Top);
    }
    return;
  }

  /* One U-shaped wall perimeter and one joined pitched roof. The six roof
   * planes share their hip/valley edges: no flat spine or intersecting slabs. */
  /* Occupancy for surface culling/AO follows the same U, with no courtyard cap. */
  AddBox(model,1,1,0,21,9,9,kSimVoxelMaterial_Wall,0);
  AddBox(model,1,23,0,21,31,9,kSimVoxelMaterial_Wall,0);
  AddBox(model,21,1,0,31,31,9,kSimVoxelMaterial_Wall,0);
  const float plan[8][2] = {{1,1},{31,1},{31,31},{1,31},{1,23},{21,23},{21,9},{1,9}};
  for (int edge=0; edge<8; edge++) {
    int next=(edge+1)&7;
    AddOutwardFace(model, edge==1 || edge==5 ? kSimVoxelMaterial_Wall : kSimVoxelMaterial_WallLight,
        220, Point(plan[edge][0],plan[edge][1],0), Point(plan[next][0],plan[next][1],0),
        Point(plan[next][0],plan[next][1],9), Point(plan[edge][0],plan[edge][1],9));
  }
  const SimBackgroundVoxelModelPoint roofs[6][4] = {
    {{.8f,.8f,9},{31.2f,.8f,9},{26,5,13},{.8f,5,13}},
    {{31.2f,.8f,9},{31.2f,31.2f,9},{26,27,13},{26,5,13}},
    {{31.2f,31.2f,9},{.8f,31.2f,9},{.8f,27,13},{26,27,13}},
    {{.8f,5,13},{26,5,13},{20.8f,9.2f,9},{.8f,9.2f,9}},
    {{26,5,13},{26,27,13},{20.8f,22.8f,9},{20.8f,9.2f,9}},
    {{.8f,22.8f,9},{20.8f,22.8f,9},{26,27,13},{.8f,27,13}},
  };
  for (int plane=0; plane<6; plane++)
    AddFace(model,kSimVoxelMaterial_Roof,plane==0?195:240,
        roofs[plane][0],roofs[plane][1],roofs[plane][2],roofs[plane][3]);
  for (int arm=0; arm<2; arm++) {
    float y=arm ? 27 : 5;
    AddFace(model,kSimVoxelMaterial_WallLight,205,
        Point(.8f,y-4.2f,9),Point(.8f,y+4.2f,9),Point(.8f,y,13),Point(.8f,y,13));
  }
  /* Exactly two stacks, both on the connecting right-hand wing. */
  AddOpenChimney(model,24.6f,10.4f,2.8f,11.5f,17,kSimVoxelMaterial_WallLight);
  AddOpenChimney(model,24.6f,18.0f,2.8f,11.5f,17,kSimVoxelMaterial_WallLight);
  /* One peaked window on the rear arm and two on the front arm. Their lower
   * walls embed in the slope; their windows clear it at every detail level. */
  const float dormers[3][2]={{10,5},{10,27},{26,27}};
  for (int i=0; i<3; i++) {
    float x=dormers[i][0], y=dormers[i][1];
    AddRoofedBox(model,x-2.5f,y+.7f,9,x+2.5f,y+3.7f,12.8f,kSimVoxelMaterial_WallLight);
    AddGableRoofX(model,x-2.8f,x+2.8f,y+.5f,y+3.9f,12.8f,15,
        kSimVoxelMaterial_Roof,kSimVoxelMaterial_WallLight);
    AddFrontBand(model,x-1.5f,x+1.5f,y+3.74f,10.5f,12.4f,kSimVoxelMaterial_Dark);
    if (detail >= kSimBackgroundVoxelDetail_Balanced)
      AddFrontBand(model,x-.22f,x+.22f,y+3.76f,10.5f,12.4f,kSimVoxelMaterial_Trim);
  }
  AddFrontBand(model,6,12,31.04f,.1f,6.5f,kSimVoxelMaterial_Dark);
  AddFrontBand(model,23,28,31.04f,.1f,6.5f,kSimVoxelMaterial_Dark);
  AddFrontBand(model,6,12,9.04f,.1f,6.5f,kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Ultra)
    for (int stack=0; stack<2; stack++)
      for (int course=0; course<3; course++)
        AddFrontBand(model,24.6f,27.4f,(stack ? 20.8f : 13.2f)+.025f,
            13.6f+course*.85f,13.73f+course*.85f,kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High)
    for (int arm=0; arm<2; arm++) {
      float y=arm ? 31.07f : 9.07f;
      AddFrontBand(model,1.2f,20.8f,y,7.8f,8.4f,kSimVoxelMaterial_Trim);
      for (int pane=0; pane<2; pane++)
        AddFrontBand(model,14+pane*3,16+pane*3,y,3,5.5f,kSimVoxelMaterial_Glass);
    }
}

/* Foliage is an authored surface, not a filled grid. Its major masses remain
 * at every LOD; extra samples refine the contour and connected colour patches. */
static float RoundCrownRadius(float height, float waist, float scale) {
  float t = (height - waist) / (height >= waist ? 1.0f - waist : waist);
  float squared = 1.0f - t * t;
  return squared <= 0.0f ? 0.0f : sqrtf(squared) * scale;
}

static uint32_t FoliageSeed(const SimBackgroundVoxelObject *object) {
  return (uint32_t)object->cell_x * 0x45D9F3Bu ^
      (uint32_t)object->cell_y * 0x119DE1F3u ^
      (uint32_t)object->group * 0x3449u;
}

/* Tapered branch with open ends embedded in the adjoining trunk/crown. The
 * section is perpendicular to the branch, so diagonal forks are real volumes. */
static void AddBranch(SimBackgroundVoxelModel *model,
                       SimBackgroundVoxelModelPoint start,
                       SimBackgroundVoxelModelPoint end,
                       float radius0, float radius1, int sides) {
  float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
  float length = sqrtf(dx * dx + dy * dy + dz * dz);
  if (length < 0.01f) return;
  dx /= length; dy /= length; dz /= length;
  float ux = dz, uy = 0.0f, uz = -dx;
  float ul = sqrtf(ux * ux + uz * uz);
  if (ul < 0.01f) { ux = 1; uz = 0; ul = 1; }
  ux /= ul; uz /= ul;
  float vx = dy * uz, vy = dz * ux - dx * uz, vz = -dy * ux;
  for (int side = 0; side < sides; side++) {
    SimBackgroundVoxelModelPoint p[4];
    for (int corner = 0; corner < 4; corner++) {
      int after = corner == 1 || corner == 2;
      bool top = corner >= 2;
      float angle = (side + after) * 6.2831853f / sides;
      float c = cosf(angle), s = sinf(angle), radius = top ? radius1 : radius0;
      SimBackgroundVoxelModelPoint center = top ? end : start;
      p[corner] = Point(center.x + radius * (c * ux + s * vx),
                        center.y + radius * (c * uy + s * vy),
                        center.z + radius * (c * uz + s * vz));
      if (!top && start.z == 0.0f) p[corner].z = 0.0f;
    }
    AddOutwardFace(model, kSimVoxelMaterial_Trunk, 230, p[0], p[1], p[2], p[3]);
  }
}

static void AddFoliagePatch(SimBackgroundVoxelModel *model,
                            SimBackgroundVoxelModelPoint a,
                            SimBackgroundVoxelModelPoint b,
                            SimBackgroundVoxelModelPoint c,
                            SimBackgroundVoxelModelPoint d,
                            SimBackgroundVoxelMaterial material) {
  /* At either pole, place the collapsed edge away from a/b/d so lighting
   * still receives a valid triangle normal. */
  if (fabsf(a.x - b.x) + fabsf(a.y - b.y) + fabsf(a.z - b.z) < 0.0001f)
    AddOutwardFace(model, material, 232, c, d, a, a);
  else
    AddOutwardFace(model, material, 232, a, b, c, d);
}

static const float kTreeCrownZ[] = {0, .09f, .26f, .34f, .51f, .59f, .75f, .82f, 1};
static const float kTreeCrownRadius[] = {0, 1, .34f, .78f, .22f, .55f, .12f, .32f, 0};

typedef struct TreeProfile {
  uint32_t seed;
  SimBackgroundVoxelTreeStyle style;
  float cx, cy, height, radius;
} TreeProfile;

static TreeProfile ResolveTreeProfile(const SimBackgroundVoxelObject *object) {
  uint32_t seed = FoliageSeed(object);
  SimBackgroundVoxelTreeStyle style = SimBackgroundVoxelRegion_TreeStyle(object->town);
  float radius = style == kSimBackgroundTreeStyle_Dryland ? 5.3f : 5.9f;
  if (style == kSimBackgroundTreeStyle_Tropical) radius += .25f;
  return (TreeProfile){seed, style,
      8.0f + ((int)(seed & 3u) - 1.5f) * .15f,
      8.0f + ((int)((seed >> 2) & 3u) - 1.5f) * .15f,
      style == kSimBackgroundTreeStyle_SnowFir ? 19.0f : 18.0f,
      radius - (seed % 4u) * .10f};
}

static SimBackgroundVoxelModelPoint TreeCrownPoint(
    const TreeProfile *profile, float h, float radial, float angle) {
  float ripple = 1.0f + .11f * cosf(angle * 6 + (profile->seed % 4u));
  return Point(profile->cx + cosf(angle) * profile->radius * radial * ripple,
               profile->cy + sinf(angle) * profile->radius * radial * ripple,
               3.5f + (profile->height - 3.5f) * h - .35f * radial * cosf(angle * 6 + (profile->seed % 4u)));
}

uint16_t SimBackgroundVoxelModel_TreeShadowVariant(const SimBackgroundVoxelObject *object) {
  /* Crown geometry uses the seed's low four bits; higher bits only identify
   * the object. Keep this identity beside the shared profile definition. */
  return object ? (uint16_t)((object->town << 4) | (FoliageSeed(object) & 15u)) : 0;
}

bool SimBackgroundVoxelModel_CastsShadow(const SimBackgroundVoxelObject *object) {
  return object && object->kind < kSimBackgroundVoxelKindCount &&
      object->kind != kSimBackgroundVoxel_Boulder && object->kind != kSimBackgroundVoxel_Rocks &&
      object->kind != kSimBackgroundVoxel_Bridge;
}

static int CompareShadowPoint(const void *left, const void *right) {
  const SimBackgroundVoxelModelPoint *a = left, *b = right;
  if (a->x != b->x) return a->x < b->x ? -1 : 1;
  return a->y == b->y ? 0 : a->y < b->y ? -1 : 1;
}

static float ShadowCross(SimBackgroundVoxelModelPoint a,
                         SimBackgroundVoxelModelPoint b,
                         SimBackgroundVoxelModelPoint c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

int SimBackgroundVoxelModel_TreeShadowHull(
    const SimBackgroundVoxelObject *object, float cast_x, float cast_y,
    SimBackgroundVoxelModelPoint out[kSimBackgroundVoxelTreeShadowMaxPoints]) {
  if (!object || !out || object->kind != kSimBackgroundVoxel_Tree ||
      !isfinite(cast_x) || !isfinite(cast_y)) return 0;
  TreeProfile profile = ResolveTreeProfile(object);
  /* Only the outer crown rings, tip and trunk foot can be silhouette extrema.
   * Reusing the model profile avoids a second hand-tuned shadow radius/height.
   * This is 101 points and a small hull, rather than reprojecting hundreds of
   * detail faces per tree into the shadow mask every frame. */
  enum { kRingSides = 24, kSamples = 4 * kRingSides + 5 };
  SimBackgroundVoxelModelPoint points[kSamples], hull[kSamples * 2];
  int count = 0;
  for (int ring = 1; ring < 8; ring += 2)
    for (int side = 0; side < kRingSides; side++)
      points[count++] = TreeCrownPoint(&profile, kTreeCrownZ[ring], kTreeCrownRadius[ring],
          side * 6.2831853f / kRingSides);
  points[count++] = TreeCrownPoint(&profile, 1, 0, 0);
  for (int corner = 0; corner < 4; corner++)
    points[count++] = Point(corner & 1 ? 8.95f : 7.05f, corner & 2 ? 8.95f : 7.05f, 0);
  for (int i = 0; i < count; i++) {
    points[i].x += points[i].z * cast_x;
    points[i].y += points[i].z * cast_y;
    points[i].z = 0;
  }
  qsort(points, count, sizeof(points[0]), CompareShadowPoint);
  int n = 0;
  for (int i = 0; i < count; i++) {
    while (n >= 2 && ShadowCross(hull[n - 2], hull[n - 1], points[i]) <= 0) n--;
    hull[n++] = points[i];
  }
  int upper = n + 1;
  for (int i = count - 2; i >= 0; i--) {
    while (n >= upper && ShadowCross(hull[n - 2], hull[n - 1], points[i]) <= 0) n--;
    hull[n++] = points[i];
  }
  n--; /* The first vertex is repeated at the end. */
  if (n < 3 || n > kSimBackgroundVoxelTreeShadowMaxPoints) return 0;
  memcpy(out, hull, n * sizeof(*out));
  return n;
}

static void BuildTree(const SimBackgroundVoxelObject *object,
                      SimBackgroundVoxelDetail detail,
                      SimBackgroundVoxelModel *model) {
  TreeProfile profile = ResolveTreeProfile(object);
  uint32_t seed = profile.seed;
  SimBackgroundVoxelTreeStyle style = profile.style;
  AddBranch(model, Point(8, 8, 0), Point(profile.cx, profile.cy, 5.0f), 0.95f, 0.60f,
            DetailChoice(detail, 5, 6, 8, 8));
  int sides = DetailChoice(detail, 7, 10, 14, 20);
  int subdivisions = detail >= kSimBackgroundVoxelDetail_High ? 2 : 1;
  for (int tier = 0; tier < 8; tier++)
    for (int sub = 0; sub < subdivisions; sub++)
      for (int side = 0; side < sides; side++) {
        SimBackgroundVoxelModelPoint p[4];
        for (int corner = 0; corner < 4; corner++) {
          bool top = corner >= 2;
          int after = corner == 1 || corner == 2;
          float t = (float)(sub + top) / subdivisions;
          float h = kTreeCrownZ[tier] + (kTreeCrownZ[tier + 1] - kTreeCrownZ[tier]) * t;
          float rad = kTreeCrownRadius[tier] + (kTreeCrownRadius[tier + 1] - kTreeCrownRadius[tier]) * t;
          float angle = (side + after) * 6.2831853f / sides;
          p[corner] = TreeCrownPoint(&profile, h, rad, angle);
        }
        float angle = (side + .5f) * 6.2831853f / sides;
        float patch = cosf(angle * 3 + (seed % 4u) * .5f);
        SimBackgroundVoxelMaterial material = (tier & 1) == 0
            ? kSimVoxelMaterial_LeavesDark
            : sub == 0 && patch > .1f ? kSimVoxelMaterial_LeavesLight : kSimVoxelMaterial_Leaves;
        if (style == kSimBackgroundTreeStyle_SnowFir && (tier & 1) && patch > -.5f)
          material = kSimVoxelMaterial_Snow;
        AddFoliagePatch(model, p[0], p[1], p[2], p[3], material);
      }
}

static SimBackgroundVoxelModelPoint BroadCrownPoint(
    float cx, float cy, float radius_x, float radius_y,
    float base_z, float height, float angle, float t, uint32_t seed) {
  float radius = RoundCrownRadius(t, .38f, 1.0f);
  /* Broad, gently irregular foliage clusters keep a chunky contour. */
  float lobes = 1.0f + .10f * cosf(angle * 3.0f + (seed & 3u)) * sinf(t * 3.14159265f);
  return Point(cx + cosf(angle) * radius_x * radius * lobes,
                cy + sinf(angle) * radius_y * radius * lobes,
                base_z + height * t);
}

static void AddBroadCrown(SimBackgroundVoxelModel *model,
                          SimBackgroundVoxelDetail detail, uint32_t seed,
                          float cx, float cy, float radius_x, float radius_y,
                          float base_z, float height, bool snow) {
  int sides = snow ? DetailChoice(detail, 6, 8, 10, 12)
                   : DetailChoice(detail, 6, 8, 12, 14);
  int rings = snow ? DetailChoice(detail, 4, 5, 6, 7)
                   : DetailChoice(detail, 3, 5, 6, 8);
  for (int ring = 0; ring < rings; ring++)
    for (int side = 0; side < sides; side++) {
      float t0 = .5f - .5f * cosf(3.14159265f * ring / rings);
      float t1 = .5f - .5f * cosf(3.14159265f * (ring + 1) / rings);
      float a0 = side * 6.2831853f / sides, a1 = (side + 1) * 6.2831853f / sides;
      float t = (t0 + t1) * .5f, a = (a0 + a1) * .5f;
      float patch = sinf(a * 3 + t * 8 + (seed & 7u)) * cosf(t * 11 - a);
      SimBackgroundVoxelMaterial mat = t < .26f || patch < -.25f
          ? kSimVoxelMaterial_LeavesDark
          : patch > .35f ? kSimVoxelMaterial_LeavesLight : kSimVoxelMaterial_Leaves;
      if (snow) mat = t > .48f || (t > .30f && patch > .3f)
          ? kSimVoxelMaterial_Snow : t < .19f
          ? kSimVoxelMaterial_LeavesDark : kSimVoxelMaterial_LeavesLight;
      AddFoliagePatch(model,
          BroadCrownPoint(cx, cy, radius_x, radius_y, base_z, height, a0, t0, seed),
          BroadCrownPoint(cx, cy, radius_x, radius_y, base_z, height, a1, t0, seed),
          BroadCrownPoint(cx, cy, radius_x, radius_y, base_z, height, a1, t1, seed),
          BroadCrownPoint(cx, cy, radius_x, radius_y, base_z, height, a0, t1, seed), mat);
    }
}

/* Three overlapping leaf clusters share a forked trunk. Large lobes are
 * silhouette features at Low too; fine subdivisions only soften their edges. */
static void BuildBranchingCrown(SimBackgroundVoxelModel *model,
                                 SimBackgroundVoxelDetail detail,
                                 uint32_t seed, float scale, bool snow) {
  float cx = 8.0f * scale, cy = 7.4f * scale;
  float sway = ((int)(seed & 3u) - 1.5f) * .12f;
  AddBranch(model, Point(cx, 10.0f * scale, 0),
            Point(cx + sway, cy, 9.0f * scale),
            1.25f * scale, .55f * scale, DetailChoice(detail, 4, 6, 8, 8));
  for (int side = 0; side < 2; side++) {
    float x = cx + (side ? 3.0f : -3.0f) * scale + sway;
    AddBranch(model, Point(cx, 9.0f * scale, 3.8f * scale),
              Point(x, 7.8f * scale, 8.2f * scale),
              .72f * scale, .35f * scale, detail == kSimBackgroundVoxelDetail_Low ? 3 : 4);
    AddBroadCrown(model, detail, seed + side, x, 7.5f * scale,
                  3.8f * scale, 3.9f * scale,
                  (5.8f + side * .6f) * scale, 6.3f * scale, snow);
  }
  AddBroadCrown(model, detail, seed + 3u, cx + sway, 4.7f * scale,
                4.0f * scale, 3.5f * scale, 8.0f * scale, 6.0f * scale, snow);
  if (snow)
    AddBroadCrown(model, detail, seed + 5u, cx + sway, 8.2f * scale,
        4.7f * scale, 4.4f * scale, 6.7f * scale, 6.8f * scale, true);
}

static void BuildBroadTree(const SimBackgroundVoxelObject *object,
                           SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  BuildBranchingCrown(model, detail, FoliageSeed(object), 1.0f, false);
}

static SimBackgroundVoxelModelPoint ShrubPoint(float angle, float t) {
  /* One closed, gently scalloped crown. Its widest section sits low, with a
   * rounded shoulder and a tiny exposed stem, as in the native $01 art. */
  float radius = RoundCrownRadius(t, 0.40f, 1.0f);
  float clump = 1.0f + 0.06f * sinf(angle * 5.0f + t * 4.0f);
  return Point(8.0f + cosf(angle) * 6.3f * radius * clump,
               8.0f + sinf(angle) * 5.4f * radius * clump,
               1.8f + 11.8f * t);
}

static void BuildShrub(const SimBackgroundVoxelObject *object,
                       SimBackgroundVoxelDetail detail,
                       SimBackgroundVoxelModel *model) {
  uint32_t seed = FoliageSeed(object);
  AddStandardBox(model, 7.0f, 7.0f, 0.0f, 9.0f, 9.0f, 3.0f,
                 kSimVoxelMaterial_Trunk);
  int sides = DetailChoice(detail, 8, 12, 16, 20);
  int rings = DetailChoice(detail, 5, 7, 9, 11);
  for (int ring = 0; ring < rings; ring++) {
    float t0 = (1.0f - cosf(3.14159265f * ring / rings)) * 0.5f;
    float t1 = (1.0f - cosf(3.14159265f * (ring + 1) / rings)) * 0.5f;
    for (int side = 0; side < sides; side++) {
      float a0 = side * 6.2831853f / sides;
      float a1 = (side + 1) * 6.2831853f / sides;
      /* Broad connected highlights follow leaf clumps, with a dark skirt.
       * These are surface facets, not overlapping cubes or floating leaves. */
      SimBackgroundVoxelModelPoint sample = ShrubPoint((a0 + a1) * 0.5f,
                                                       (t0 + t1) * 0.5f);
      float patch = sinf(sample.x * 0.95f + sample.z * 0.7f + (seed & 3u)) *
          cosf(sample.y * 1.3f - sample.z * 0.9f);
      SimBackgroundVoxelMaterial material = t1 < 0.30f || patch < -0.35f
          ? kSimVoxelMaterial_LeavesDark
          : patch > 0.50f ? kSimVoxelMaterial_LeavesLight : kSimVoxelMaterial_Leaves;
      if (ring == 0) {
        /* Start the closing triangle on its non-degenerate edge so the
         * normal calculator sees the skirt, not two copies of its pole. */
        AddOutwardFace(model, material, 210,
                ShrubPoint(a1, t1), ShrubPoint(a0, t1),
                ShrubPoint(a0, t0), ShrubPoint(a0, t0));
      } else {
        AddOutwardFace(model, material, 232,
                ShrubPoint(a0, t0), ShrubPoint(a1, t0),
                ShrubPoint(a1, t1), ShrubPoint(a0, t1));
      }
    }
  }
}

static void BuildRocks(const SimBackgroundVoxelObject *object,
                       SimBackgroundVoxelModel *model) {
  const SimBackgroundRockShape *rocks;
  int count = RockShapes(object, &rocks);
  static const float outline[8][2] = {
    {-0.65f, -0.85f}, {0.35f, -1.0f}, {0.90f, -0.55f}, {1.0f, 0.40f},
    {0.50f, 1.0f}, {-0.45f, 0.88f}, {-1.0f, 0.35f}, {-0.95f, -0.40f},
  };
  static const float shoulder[8] = {0.85f, 0.92f, 0.95f, 0.70f,
                                     0.68f, 0.73f, 0.80f, 0.87f};
  for (int at = 0; at < count; at++) {
    const SimBackgroundRockShape *rock = &rocks[at];
    SimBackgroundVoxelModelPoint base[8], top[8];
    for (int corner = 0; corner < 8; corner++) {
      float x = outline[corner][0] * rock->radius_x;
      float y = outline[corner][1] * rock->radius_y;
      base[corner] = Point(rock->x + x, rock->y + y, 0.0f);
      top[corner] = Point(rock->x + x * 0.80f - rock->radius_x * 0.08f,
                          rock->y + y * 0.74f, rock->height * shoulder[corner]);
    }
    SimBackgroundVoxelModelPoint peak = Point(
        rock->x - rock->radius_x * 0.24f, rock->y - rock->radius_y * 0.12f,
        rock->height);
    for (int side = 0; side < 8; side++) {
      int next = (side + 1) & 7;
      AddFace(model, kSimVoxelMaterial_Wall, 210,
              base[side], base[next], top[next], top[side]);
      AddFace(model, side < 3 ? kSimVoxelMaterial_WallLight : kSimVoxelMaterial_Wall,
              242, top[side], top[next], peak, peak);
    }
  }
}

typedef struct PalmFrondSection {
  SimBackgroundVoxelModelPoint left, ridge, right;
} PalmFrondSection;

static PalmFrondSection PalmFrondAt(
    float center_x, float center_y, float angle, float length,
    float arch, float droop, float sweep, int segment, int segments,
    bool feathered) {
  const float pi = 3.14159265f;
  float t = (float)segment / segments;
  float bend = sinf(pi * t);
  float dx = cosf(angle), dy = sinf(angle);
  float radius = 0.28f + length * t;
  float x = center_x + dx * radius - dy * sweep * bend;
  float y = center_y + dy * radius + dx * sweep * bend;
  float z = 12.0f + arch * bend - droop * t * t;
  float width = 0.14f * (1.0f - t) + 0.95f * bend;
  float left_width = width, right_width = width;
  if (feathered && segment > 0 && segment < segments) {
    /* Alternating inset margins suggest paired leaflets. These are the
     * actual surface edges, never floating stripes or overlapping fins. */
    left_width *= (segment & 1) ? 1.0f : 0.52f;
    right_width *= (segment & 1) ? 0.52f : 1.0f;
  }
  if (segment == segments) left_width = right_width = 0.0f;
  float fold = 0.30f * bend + 0.04f * (1.0f - t);
  if (segment == segments) fold = 0.0f;
  return (PalmFrondSection){
    .left = Point(x - dy * left_width, y + dx * left_width, z - fold),
    .ridge = Point(x, y, z),
    .right = Point(x + dy * right_width, y - dx * right_width, z - fold),
  };
}

static void AddPalmFrond(
    SimBackgroundVoxelModel *model, float center_x, float center_y,
    float angle, float length, float arch, float droop, float sweep,
    int segments, bool feathered, int index) {
  PalmFrondSection previous = PalmFrondAt(
      center_x, center_y, angle, length, arch, droop, sweep, 0, segments,
      feathered);
  for (int at = 1; at <= segments; at++) {
    PalmFrondSection next = PalmFrondAt(
        center_x, center_y, angle, length, arch, droop, sweep, at, segments,
        feathered);
    /* A folded, closed ribbon: two lit upper planes and one underside.
     * Adjacent sections share their exact three vertices. The tip converges
     * to a point and the narrow root terminates inside the crownshaft. */
    AddOutwardFace(model,
        index % 3 == 0 ? kSimVoxelMaterial_Leaves : kSimVoxelMaterial_LeavesLight,
        242, previous.ridge, next.ridge, next.left, previous.left);
    AddOutwardFace(model, kSimVoxelMaterial_Leaves, 224,
        previous.right, next.right, next.ridge, previous.ridge);
    AddOutwardFace(model, kSimVoxelMaterial_LeavesDark, 195,
        previous.left, next.left, next.right, previous.right);
    previous = next;
  }
}

static void BuildPalm(const SimBackgroundVoxelObject *object,
                      SimBackgroundVoxelDetail detail,
                      SimBackgroundVoxelModel *model) {
  const float pi = 3.14159265f;
  uint32_t seed = (uint32_t)object->cell_x * 0x45D9F3Bu ^
      (uint32_t)object->cell_y * 0x119DE1F3u ^
      (uint32_t)object->group * 0x3449u;
  float lean_x = (seed & 1u) ? 0.60f : -0.60f;
  float lean_y = (seed & 2u) ? 0.38f : -0.38f;
  float center_x = 8.0f + lean_x, center_y = 8.0f + lean_y;
  float rotation = 0.15f + ((seed >> 2) & 3u) * 0.055f;
  static const int frond_segments[] = {2, 4, 8, 12};
  static const int trunk_sides[] = {5, 6, 6, 8};
  static const float low_heights[] = {0.0f, 5.7f, 11.8f};
  static const float balanced_heights[] = {0.0f, 2.2f, 5.7f, 9.1f, 11.8f};
  static const float high_heights[] = {
    0.0f, 1.2f, 4.0f, 4.18f, 7.6f, 7.78f, 10.4f, 11.8f,
  };
  static const float ultra_heights[] = {
    0.0f, 1.2f, 3.2f, 3.38f, 5.8f, 5.98f, 8.5f, 8.68f, 10.4f, 11.8f,
  };
  const float *heights = detail == kSimBackgroundVoxelDetail_Low ? low_heights
      : detail == kSimBackgroundVoxelDetail_Balanced ? balanced_heights
      : detail == kSimBackgroundVoxelDetail_High ? high_heights : ultra_heights;
  const int sections[] = {2, 4, 7, 9};
  int sides = trunk_sides[detail];
  SimBackgroundVoxelModelPoint previous[8];
  for (int ring = 0; ring <= sections[detail]; ring++) {
    float z = heights[ring];
    float t = z / 11.8f;
    float radius = 0.60f + 0.43f * (1.0f - t) * (1.0f - t);
    float x = 8.0f + lean_x * t * t, y = 8.0f + lean_y * t * t;
    SimBackgroundVoxelModelPoint next[8];
    for (int side = 0; side < sides; side++) {
      float angle = rotation + side * 2.0f * pi / sides;
      next[side] = Point(x + radius * cosf(angle), y + radius * sinf(angle), z);
    }
    if (ring > 0) {
      bool scar = z - heights[ring - 1] < 0.3f;
      for (int side = 0; side < sides; side++) {
        int after = (side + 1) % sides;
        AddOutwardFace(model, scar ? kSimVoxelMaterial_Wood : kSimVoxelMaterial_Trunk,
            scar ? 210 : 238, previous[side], previous[after], next[after], next[side]);
      }
    }
    memcpy(previous, next, sides * sizeof(next[0]));
  }
  /* The stem and small green growing point share a ring. There is no buried
   * cap, cubic hub, root cross or separate collar to widen the silhouette. */
  SimBackgroundVoxelModelPoint shoot = Point(center_x, center_y, 13.05f);
  for (int side = 0; side < sides; side++) {
    int after = (side + 1) % sides;
    AddOutwardFace(model, kSimVoxelMaterial_Leaves, 235,
        previous[side], previous[after], shoot, shoot);
  }
  for (int frond = 0; frond < 8; frond++) {
    float angle = rotation + frond * pi * 0.25f;
    float length = 6.1f + ((frond + (seed >> 4)) % 3u) * 0.20f;
    float arch = 1.8f + ((frond * 3 + (seed >> 6)) % 4u) * 0.20f;
    float droop = 2.5f + ((frond + (seed >> 8)) % 3u) * 0.40f;
    float sweep = (frond & 1) ? 0.30f : -0.30f;
    AddPalmFrond(model, center_x, center_y, angle, length, arch, droop, sweep,
        frond_segments[detail], detail >= kSimBackgroundVoxelDetail_High, frond);
  }
}

static void BuildStoryTree(SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  BuildBranchingCrown(model, detail, 2u, 2.05f, true);
  /* Centre the wider ancient tree within its two-cell plot. */
  for (uint16_t f = 0; f < model->face_count; f++)
    for (int v = 0; v < 4; v++) {
      model->faces[f].points[v].x -= .4f;
      model->faces[f].points[v].y += 1.5f;
      model->faces[f].points[v].z *= 1.035f;
    }
  /* Four broad roots anchor the landmark. Their open lower rings terminate
   * in the terrain, and upper ends join the trunk rather than floating. */
  for (int root = 0; root < 4; root++) {
    float angle = root * 1.57079633f + .35f;
    AddBranch(model, Point(16 + cosf(angle) * 3.8f, 22 + sinf(angle) * 3.8f, 0),
              Point(16, 21.5f, 5.0f), 1.1f, .8f, 4);
  }
}

static void AddCastleGatehouse(SimBackgroundVoxelModel *model) {
  /* Projecting stone piers and a heavy flat coping surround a genuine arch
   * passage. Only the piers need solid-box metadata; the head is an extruded
   * profile so its opening and interior reveals cannot be buried by a box. */
  const float back = 26.5f, front = 32.0f, top = 14.2f;
  AddBox(model, 10.5f, back, 0.0f, 13.5f, front, top,
         kSimVoxelMaterial_WallLight, kBoxFace_North | kBoxFace_South | kBoxFace_West);
  AddBox(model, 18.5f, back, 0.0f, 21.5f, front, top,
         kSimVoxelMaterial_WallLight, kBoxFace_North | kBoxFace_South | kBoxFace_East);
  AddFace(model, kSimVoxelMaterial_Dark, 178,
          Point(13.5f, front, 0.0f), Point(13.5f, back, 0.0f),
          Point(13.5f, back, 5.0f), Point(13.5f, front, 5.0f));
  AddFace(model, kSimVoxelMaterial_Dark, 178,
          Point(18.5f, back, 0.0f), Point(18.5f, front, 0.0f),
          Point(18.5f, front, 5.0f), Point(18.5f, back, 5.0f));
  static const float arch[][2] = {
    {13.5f, 5.0f}, {14.25f, 6.4f}, {15.2f, 7.2f},
    {16.8f, 7.2f}, {17.75f, 6.4f}, {18.5f, 5.0f},
  };
  for (int segment = 0; segment < 5; segment++) {
    float x0 = arch[segment][0], z0 = arch[segment][1];
    float x1 = arch[segment + 1][0], z1 = arch[segment + 1][1];
    AddFace(model, kSimVoxelMaterial_WallLight, 232,
            Point(x0, front, z0), Point(x1, front, z1),
            Point(x1, front, top), Point(x0, front, top));
    AddFace(model, kSimVoxelMaterial_WallLight, 178,
            Point(x1, back, z1), Point(x0, back, z0),
            Point(x0, back, top), Point(x1, back, top));
    AddFace(model, kSimVoxelMaterial_Dark, 178,
            Point(x0, front, z0), Point(x1, front, z1),
            Point(x1, back, z1), Point(x0, back, z0));
  }
  AddStandardBox(model, 10.0f, 26.0f, top, 22.0f, front, 15.2f,
                 kSimVoxelMaterial_Trim);
  /* A shaded threshold is confined to the passage, leaving the recessed wall
   * bays and the courtyard on their own town ground. */
  AddFace(model, kSimVoxelMaterial_Dark, 178,
          Point(13.5f, back, 0.03f), Point(18.5f, back, 0.03f),
          Point(18.5f, front, 0.03f), Point(13.5f, front, 0.03f));
}

static void BuildBloodpoolCastle(SimBackgroundVoxelDetail detail,
                                 SimBackgroundVoxelModel *model) {
  /* Separate curtain walls surround a smaller keep. There is deliberately no
   * slab beneath the whole plot: town ground must show through the courtyard.
   * Six spires define the native silhouette: four ground-level corners and
   * a taller pair attached to the keep. Curtain ends terminate inside the
   * corner shafts, never across their fronts. Only buried end caps are omitted. */
  const uint8_t across = kBoxFace_North | kBoxFace_South | kBoxFace_Top;
  const uint8_t along = kBoxFace_East | kBoxFace_West | kBoxFace_Top;
  AddBox(model, 7.0f, 27.0f, 0.0f, 10.5f, 29.5f, 9.0f,
         kSimVoxelMaterial_WallLight, across);
  AddBox(model, 21.5f, 27.0f, 0.0f, 25.0f, 29.5f, 9.0f,
         kSimVoxelMaterial_WallLight, across);
  AddBox(model, 7.0f, 4.0f, 0.0f, 25.0f, 7.0f, 12.0f,
         kSimVoxelMaterial_Wall, across);
  AddBox(model, 2.0f, 9.0f, 0.0f, 5.0f, 27.0f, 12.0f,
         kSimVoxelMaterial_Wall, along);
  AddBox(model, 27.0f, 9.0f, 0.0f, 30.0f, 27.0f, 12.0f,
         kSimVoxelMaterial_Wall, along);
  AddRoofedBox(model, 11.0f, 10.0f, 0.0f, 21.0f, 22.0f, 20.0f,
               kSimVoxelMaterial_WallLight);
  /* The native keep has a short horizontal ridge between its turrets and a
   * modest roof pitch above a tall front facade. Four closed hips meet it. */
  const SimBackgroundVoxelModelPoint eaves[4] = {
    {10.5f, 9.0f, 20.0f}, {21.5f, 9.0f, 20.0f},
    {21.5f, 23.0f, 20.0f}, {10.5f, 23.0f, 20.0f},
  };
  const SimBackgroundVoxelModelPoint ridge[4] = {
    {14.0f, 13.0f, 26.0f}, {18.0f, 13.0f, 26.0f},
    {18.0f, 13.0f, 26.0f}, {14.0f, 13.0f, 26.0f},
  };
  const uint8_t roof_shade[4] = {178, 204, 242, 190};
  for (int side = 0; side < 4; side++) {
    int next = (side + 1) & 3;
    AddFace(model, kSimVoxelMaterial_Roof, roof_shade[side],
            eaves[side], eaves[next], ridge[next], ridge[side]);
  }
  for (int tower = 0; tower < 6; tower++) {
    bool attached = tower >= 4;
    float x = attached ? (tower & 1 ? 19.0f : 9.0f)
                       : (tower & 1 ? 25.0f : 2.0f);
    float y = attached ? 7.0f : (tower & 2 ? 27.0f : 4.0f);
    float width = attached ? 4.0f : 5.0f;
    float eave = attached ? 25.0f : (tower & 2 ? 13.0f : 21.0f);
    AddRoofedBox(model, x, y, 0.0f, x + width, y + width, eave,
                 kSimVoxelMaterial_WallLight);
    /* Keep the cap outline at Low too; the landmark allowance pays for these
     * facets, while density controls the facade details below. */
    AddOctagonalFrustum(model, x + width * 0.5f, y + width * 0.5f, width * 0.72f, 0.0f,
                        eave, eave + 7.0f, kSimVoxelMaterial_Gold);
  }
  AddCastleGatehouse(model);
  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    for (int side = 0; side < 2; side++) {
      float wall_x = side ? 21.5f : 7.0f;
      AddBox(model, wall_x, 28.5f, 9.0f, wall_x + 3.5f, 29.7f, 10.0f,
             kSimVoxelMaterial_Trim, across);
      float x = side ? 19.4f : 11.4f;
      AddFace(model, kSimVoxelMaterial_Dark, 232,
              Point(x, 32.05f, 8.5f), Point(x + 1.2f, 32.05f, 8.5f),
              Point(x + 1.2f, 32.05f, 11.5f), Point(x, 32.05f, 11.5f));
    }
  }
  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int corner = 0; corner < 2; corner++) {
      float x = corner ? 26.7f : 3.7f;
      float y = 9.05f;
      AddFace(model, kSimVoxelMaterial_Dark, 232,
              Point(x, y, 12.0f), Point(x + 1.6f, y, 12.0f),
              Point(x + 1.6f, y, 16.0f), Point(x, y, 16.0f));
    }
    for (int side = 0; side < 2; side++) {
      float x = side ? 20.2f : 10.2f;
      AddFace(model, kSimVoxelMaterial_Dark, 232,
              Point(x, 11.05f, 21.0f), Point(x + 1.6f, 11.05f, 21.0f),
              Point(x + 1.6f, 11.05f, 24.0f), Point(x, 11.05f, 24.0f));
    }
    for (int slot = 0; slot < 4; slot++) {
      float x = 11.9f + slot * 2.3f;
      AddFace(model, kSimVoxelMaterial_Dark, 232,
              Point(x, 22.05f, 16.0f), Point(x + 1.2f, 22.05f, 16.0f),
              Point(x + 1.2f, 22.05f, 19.0f), Point(x, 22.05f, 19.0f));
    }
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    /* Restrained native stone courses, not invented curtain crenellations. */
    for (int course = 0; course < 2; course++) {
      float z = 11.5f + course * 3.0f;
      AddFace(model, kSimVoxelMaterial_Trim, 232,
              Point(11.0f, 22.06f, z), Point(21.0f, 22.06f, z),
              Point(21.0f, 22.06f, z + 0.6f), Point(11.0f, 22.06f, z + 0.6f));
      for (int side = 0; side < 2; side++) {
        float x = side ? 25.0f : 2.0f;
        float tower_z = 2.5f + course * 4.0f;
        AddFace(model, kSimVoxelMaterial_Trim, 232,
                Point(x, 32.05f, tower_z), Point(x + 5.0f, 32.05f, tower_z),
                Point(x + 5.0f, 32.05f, tower_z + 0.6f), Point(x, 32.05f, tower_z + 0.6f));
      }
    }
  }
}

static SimBackgroundVoxelModelPoint PyramidEyePoint(float x, float z,
                                                    float relief) {
  /* Follow the actual south casing slope, with a deliberate outward offset. */
  return Point(x, 16.0f + 15.5f * (1.0f - z / 28.0f) + relief, z);
}

static void AddPyramidEye(SimBackgroundVoxelModel *model) {
  static const float outline[8][2] = {
    {-4.0f, 0.0f}, {-2.5f, -1.4f}, {0.0f, -1.8f}, {2.5f, -1.4f},
    {4.0f, 0.0f}, {2.5f, 1.4f}, {0.0f, 1.8f}, {-2.5f, 1.4f},
  };
  for (int edge = 0; edge < 8; edge++) {
    int next = (edge + 1) & 7;
    SimBackgroundVoxelModelPoint a = PyramidEyePoint(
        16.0f + outline[edge][0], 20.3f + outline[edge][1], 0.12f);
    SimBackgroundVoxelModelPoint b = PyramidEyePoint(
        16.0f + outline[next][0], 20.3f + outline[next][1], 0.12f);
    SimBackgroundVoxelModelPoint c = PyramidEyePoint(
        16.0f + outline[next][0] * 0.78f,
        20.3f + outline[next][1] * 0.65f, 0.12f);
    SimBackgroundVoxelModelPoint d = PyramidEyePoint(
        16.0f + outline[edge][0] * 0.78f,
        20.3f + outline[edge][1] * 0.65f, 0.12f);
    AddFace(model, kSimVoxelMaterial_Dark, 255, a, b, c, d);
    SimBackgroundVoxelModelPoint center = PyramidEyePoint(16.0f, 20.3f, 0.12f);
    AddFace(model, kSimVoxelMaterial_Glass, 255, d, c, center, center);
  }
  AddFace(model, kSimVoxelMaterial_Dark, 255,
          PyramidEyePoint(15.1f, 19.2f, 0.18f),
          PyramidEyePoint(16.9f, 19.2f, 0.18f),
          PyramidEyePoint(16.9f, 21.4f, 0.18f),
          PyramidEyePoint(15.1f, 21.4f, 0.18f));
}

/* All masonry is a flat surface marking on the same Egyptian slope. The
 * tiny normal offset prevents ties with the casing; it never forms a terrace. */
static SimBackgroundVoxelModelPoint PyramidFacePoint(int side, float across,
                                                       float z, float relief) {
  float edge = 15.5f * (1.0f - z / 28.0f) + relief;
  switch (side) {
    case 0: return Point(16.0f + across, 16.0f + edge, z);
    case 1: return Point(16.0f + edge, 16.0f - across, z);
    case 2: return Point(16.0f - across, 16.0f - edge, z);
    default: return Point(16.0f - edge, 16.0f + across, z);
  }
}

static void BuildPyramid(const SimBackgroundVoxelObject *object,
                         SimBackgroundVoxelDetail detail,
                         SimBackgroundVoxelModel *model) {
  /* Kasandora's native pyramid has continuous faces with masonry courses on
   * the lower half and pale casing above. Subdivisions preserve the renderer's
   * vertex lighting without changing the plane or introducing horizontal tops. */
  int strips = DetailChoice(detail, 2, 3, 5, 6);
  for (int strip = 0; strip < strips; strip++) {
    float z0 = 28.0f * strip / strips, z1 = 28.0f * (strip + 1) / strips;
    uint16_t first = model->face_count;
    AddSquareFrustum(model, 16.0f, 16.0f, 15.5f * (1.0f - z0 / 28.0f),
                     15.5f * (1.0f - z1 / 28.0f), z0, z1,
                     kSimVoxelMaterial_WallLight);
    /* Distinguish the broad lit front from its shaded flanks, as in the
     * source sprite, even when palette quantization flattens directional light. */
    for (uint16_t face = first; face < model->face_count; face++)
      if (face - first != 2) model->faces[face].material = kSimVoxelMaterial_Wall;
  }
  int courses = DetailChoice(detail, 2, 4, 5, 6);
  for (int side = 0; side < 4; side++) {
    for (int row = 0; row < courses; row++) {
      float z0 = 15.5f * (row + 1) / courses;
      float z1 = z0 + 0.30f;
      float half0 = 15.5f * (1.0f - z0 / 28.0f);
      float half1 = 15.5f * (1.0f - z1 / 28.0f);
      AddFace(model, kSimVoxelMaterial_Trim, 222,
              PyramidFacePoint(side, -half0, z0, 0.18f),
              PyramidFacePoint(side, half0, z0, 0.18f),
              PyramidFacePoint(side, half1, z1, 0.18f),
              PyramidFacePoint(side, -half1, z1, 0.18f));
      if (detail == kSimBackgroundVoxelDetail_Low) continue;
      float bottom = 15.5f * row / courses + 0.35f;
      int joints = DetailChoice(detail, 0, 2, 3, 4);
      for (int joint = 0; joint < joints; joint++) {
        float x = (joint - (joints - 1) * 0.5f + (row & 1 ? 0.25f : -0.25f)) *
                  (2.0f * half0 / (joints + 1));
        AddFace(model, kSimVoxelMaterial_Trim, 222,
                PyramidFacePoint(side, x - 0.15f, bottom, 0.18f),
                PyramidFacePoint(side, x + 0.15f, bottom, 0.18f),
                PyramidFacePoint(side, x + 0.15f, z0, 0.18f),
                PyramidFacePoint(side, x - 0.15f, z0, 0.18f));
      }
    }
  }
  if (object->flags & kSimBackgroundVoxel_PyramidEye) AddPyramidEye(model);
}

static void AddMarahnaCrown(SimBackgroundVoxelDetail detail,
                            SimBackgroundVoxelModel *model,
                            float x, float y, float radius,
                            float eave, float peak, bool round) {
  if (detail == kSimBackgroundVoxelDetail_Low) {
    if (round) {
      AddOctagonalFrustum(model, x, y, radius, 0.0f, eave, peak,
                          kSimVoxelMaterial_Gold);
    } else {
      AddSquareFrustum(model, x, y, radius, radius * 0.6f,
                       eave, eave + (peak - eave) * 0.6f, kSimVoxelMaterial_Gold);
      AddSquareFrustum(model, x, y, radius * 0.6f, 0.0f,
                       eave + (peak - eave) * 0.6f, peak, kSimVoxelMaterial_Gold);
    }
    return;
  }
  /* Corbelled decoration belongs to the tips; the side mounds below retain
   * their rounded taper. The central prang keeps its broader chamfered plan. */
  static const float balanced[][2] = {
    {1, 0}, {.80f, .28f}, {.84f, .32f}, {.42f, .70f}, {0, 1}};
  static const float high[][2] = {
    {1, 0}, {.78f, .22f}, {.82f, .26f}, {.50f, .50f},
    {.54f, .54f}, {.24f, .79f}, {0, 1}};
  static const float ultra[][2] = {
    {1, 0}, {.80f, .15f}, {.84f, .19f}, {.59f, .38f},
    {.63f, .42f}, {.38f, .63f}, {.42f, .67f}, {.18f, .86f}, {0, 1}};
  const float (*profile)[2] = detail == kSimBackgroundVoxelDetail_Ultra
      ? ultra : detail == kSimBackgroundVoxelDetail_High ? high : balanced;
  int tiers = DetailChoice(detail, 2, 4, 6, 8);
  static const float corner[8][2] = {
    {-1, -.6f}, {-.6f, -1}, {.6f, -1}, {1, -.6f},
    {1, .6f}, {.6f, 1}, {-.6f, 1}, {-1, .6f}};
  static const uint8_t shade[8] = {178, 178, 190, 204, 218, 232, 218, 190};
  for (int tier = 0; tier < tiers; tier++) {
    float r0 = radius * profile[tier][0], r1 = radius * profile[tier + 1][0];
    float z0 = eave + (peak - eave) * profile[tier][1];
    float z1 = eave + (peak - eave) * profile[tier + 1][1];
    SimBackgroundVoxelMaterial material = r1 > r0
        ? kSimVoxelMaterial_RoofLight : kSimVoxelMaterial_Gold;
    if (round) {
      AddOctagonalFrustum(model, x, y, r0, r1, z0, z1, material);
      continue;
    }
    for (int side = 0; side < 8; side++) {
      int next = (side + 1) & 7;
      AddFace(model, material, shade[side],
              Point(x + corner[side][0] * r0, y + corner[side][1] * r0, z0),
              Point(x + corner[next][0] * r0, y + corner[next][1] * r0, z0),
              Point(x + corner[next][0] * r1, y + corner[next][1] * r1, z1),
              Point(x + corner[side][0] * r1, y + corner[side][1] * r1, z1));
    }
  }
}

static void BuildMarahnaTemple(SimBackgroundVoxelDetail detail,
                               SimBackgroundVoxelModel *model) {
  /* Native $EF landmark: the entrance prang is flanked by two unwindowed
   * conical mounds, with projecting entrance pillars and a continuous outer
   * enclosure around open ground. The ordinary $C2 cathedral is independent. */
  const bool coarse = detail == kSimBackgroundVoxelDetail_Low;
  AddStandardBox(model, 10.5f, 8.5f, 0.0f, 21.5f, 19.5f, 3.0f,
                 kSimVoxelMaterial_Trim);
  AddBox(model, 10.5f, 8.5f, 3.0f, 21.5f, 19.5f, 11.0f,
         kSimVoxelMaterial_WallLight,
         coarse ? kBoxFace_AllVisible & ~kBoxFace_Top : kBoxFace_AllVisible);
  AddMarahnaCrown(detail, model, 16.0f, 14.0f, 5.5f, 11.0f, 24.0f, false);
  for (int side = 0; side < 2; side++) {
    float x = side ? 26.5f : 5.5f;
    /* The sprite has a full, rounded belly before the taper to the decorated
     * tip. Keep a small ground footprint and a continuous faceted skin, with
     * no vertical box shaft, window, or horizontal terrace between rings. */
    float belly_z = coarse ? 5.5f : 3.8f;
    AddOctagonalFrustum(model, x, 12.0f, 4.5f, 4.9f, 0.0f, belly_z,
                        kSimVoxelMaterial_Gold);
    if (coarse) {
      AddOctagonalFrustum(model, x, 12.0f, 4.9f, 2.7f, belly_z, 11.5f,
                          kSimVoxelMaterial_Gold);
    } else {
      AddOctagonalFrustum(model, x, 12.0f, 4.9f, 4.1f, belly_z, 8.5f,
                          kSimVoxelMaterial_Gold);
      AddOctagonalFrustum(model, x, 12.0f, 4.1f, 2.7f, 8.5f, 11.5f,
                          kSimVoxelMaterial_Gold);
    }
    AddMarahnaCrown(detail, model, x, 12.0f, 2.7f, 11.5f, 19.0f, true);
  }

  /* Pillars are full-height supports in front of the doorway. Their north
   * faces and tops meet the central mass and lintel, so no buried caps are
   * emitted. The opening stays behind the frame, with room for the steps. */
  for (int side = 0; side < 2; side++) {
    float x = side ? 19.6f : 10.5f;
    AddBox(model, x, 19.5f, 0.0f, x + 1.9f, 22.5f, 10.0f,
           kSimVoxelMaterial_WallLight,
           kBoxFace_East | kBoxFace_South | kBoxFace_West);
  }
  AddBox(model, 10.5f, 19.5f, 10.0f, 21.5f, 22.5f, 11.0f,
         kSimVoxelMaterial_Gold, kBoxFace_AllVisible & ~kBoxFace_North);
  AddFace(model, kSimVoxelMaterial_Dark, 232,
          Point(13.0f, 19.6f, 3.0f), Point(19.0f, 19.6f, 3.0f),
          Point(19.0f, 19.6f, 10.0f), Point(13.0f, 19.6f, 10.0f));
  AddBox(model, 14.0f, 19.5f, 0.0f, 18.0f, 32.0f, 0.6f,
         kSimVoxelMaterial_Paving, kBoxFace_AllVisible & ~kBoxFace_North);
  int steps = coarse ? 1 : 4;
  for (int step = 0; step < steps; step++) {
    float t = (float)step / steps;
    AddBox(model, 13.4f, 19.5f + 3.6f * (1.0f - (float)(step + 1) / steps), 0.6f,
           18.6f, 19.5f + 3.6f * (1.0f - t), 0.6f + 2.4f * (step + 1) / steps,
           kSimVoxelMaterial_WallLight, kBoxFace_AllVisible & ~kBoxFace_North);
  }

  /* Thick outer walls wrap into the front gateposts. Leave the native gaps
   * between those posts and the doorway: a rail there would block court access.
   * Shared wall ends meet without overlapping tops at every detail level. */
  for (int side = 0; side < 2; side++) {
    float outer = side ? 30.0f : 0.0f;
    float gate = side ? 19.5f : 9.5f;
    float front = side ? 22.5f : 2.0f;
    AddStandardBox(model, outer, 12.0f, 0.0f, outer + 2.0f, 32.0f, 2.8f,
                   kSimVoxelMaterial_Trim);
    AddRoofedBox(model, gate, 29.0f, 0.0f, gate + 3.0f, 32.0f, 3.2f,
                 kSimVoxelMaterial_Trim);
    AddSquareFrustum(model, gate + 1.5f, 30.5f, 1.5f, 0.0f, 3.2f, 5.2f,
                     kSimVoxelMaterial_Gold);
    AddBox(model, front, 29.0f, 0.0f, front + 7.5f, 32.0f, 2.8f,
           kSimVoxelMaterial_Trim, kBoxFace_North | kBoxFace_South | kBoxFace_Top);

    /* The native front pair are freestanding pillars inside the courts,
     * separate from the doorway jambs. Their plinths never fill the gardens. */
    float x = side ? 26.0f : 6.0f;
    AddStandardBox(model, x - 2.4f, 23.1f, 0.0f, x + 2.4f, 27.9f, 1.2f,
                   kSimVoxelMaterial_Trim);
    AddRoofedBox(model, x - 1.8f, 23.7f, 1.2f, x + 1.8f, 27.3f, 6.0f,
                 kSimVoxelMaterial_Gold);
    if (coarse) {
      AddSquareFrustum(model, x, 25.5f, 1.8f, 0.0f, 6.0f, 9.5f,
                       kSimVoxelMaterial_Gold);
    } else {
      AddSquareFrustum(model, x, 25.5f, 2.1f, 1.2f, 6.0f, 7.5f,
                       kSimVoxelMaterial_Gold);
      AddSquareFrustum(model, x, 25.5f, 1.2f, 0.0f, 7.5f, 9.5f,
                       kSimVoxelMaterial_Gold);
    }
  }
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
  if (!face->outward_winding && (axis != 2 || outward < 0.0f))
    outward = -outward;
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
  static const uint8_t visibility[] = {255, 236, 220, 204};
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

static void BuildAlternateFacingHouse(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *model) {
  SimBackgroundVoxelHouseStyle house_style =
      SimBackgroundVoxelRegion_ObjectHouseStyle(object);
  /* Shelter builders own their alternate crown/awning geometry. */
  if (house_style == kSimBackgroundHouseStyle_Yurt ||
      house_style == kSimBackgroundHouseStyle_Tent ||
      house_style == kSimBackgroundHouseStyle_WhiteTent) return;

  if (house_style == kSimBackgroundHouseStyle_Adobe || house_style == kSimBackgroundHouseStyle_Stone) {
    AddRoofedBox(model, 2.8f, 4.4f, 10.2f, 7.5f, 8.5f, 13.4f, kSimVoxelMaterial_WallLight);
    AddStandardBox(model, 2.4f, 4.0f, 13.4f, 7.9f, 9.0f, 14.2f, kSimVoxelMaterial_RoofLight);
    AddFrontBand(model, 3.6f, 6.7f, 8.54f, 11.0f, 12.6f, kSimVoxelMaterial_Dark);
    if (detail >= kSimBackgroundVoxelDetail_High)
      AddFrontBand(model, 3.4f, 6.9f, 8.55f, 10.8f, 11.0f, kSimVoxelMaterial_Trim);
    return;
  }
  if (house_style == kSimBackgroundHouseStyle_Aitos) {
    AddOpenChimney(model, 2.4f, 3.3f, 3.2f, 9.5f, 13.6f, kSimVoxelMaterial_WallLight);
    return;
  }

  if (house_style == kSimBackgroundHouseStyle_Timber ||
      house_style == kSimBackgroundHouseStyle_Bloodpool ||
      house_style == kSimBackgroundHouseStyle_MarahnaStilt ||
      house_style == kSimBackgroundHouseStyle_MarahnaLogCabin) {
    float eave = house_style == kSimBackgroundHouseStyle_Timber ? 7.5f :
        house_style == kSimBackgroundHouseStyle_MarahnaLogCabin ? 7.8f : 9.0f;
    float rise = house_style == kSimBackgroundHouseStyle_Bloodpool ? 5.5f :
        house_style == kSimBackgroundHouseStyle_MarahnaStilt ? 3.5f :
        house_style == kSimBackgroundHouseStyle_MarahnaLogCabin ? 4.2f : 4.0f;
    float low = eave + rise * .84f, high = eave + rise * .99f;
    /* Native alternates have a little dormer in the left roof plane. The
     * lower shell intersects that roof; the window sits wholly above it. */
    AddRoofedBox(model, 1.6f, 8.1f, eave - .2f, 5.4f, 12.0f, low,
                 kSimVoxelMaterial_WallLight);
    AddShedRoofX(model, 1.6f, 5.4f, 8.1f, 12.0f, low, high,
                 kSimVoxelMaterial_RoofLight, kSimVoxelMaterial_WallLight);
    AddFace(model, kSimVoxelMaterial_WallLight, 190,
        Point(5.4f, 8.1f, low), Point(5.4f, 12.0f, low),
        Point(5.4f, 12.0f, high), Point(5.4f, 8.1f, high));
    AddFrontBand(model, 2.1f, 3.9f, 12.04f, eave + rise * .64f,
        eave + rise * .82f, kSimVoxelMaterial_Dark);
    if (detail >= kSimBackgroundVoxelDetail_High)
      AddFrontBand(model, 1.95f, 4.05f, 12.05f, eave + rise * .59f,
          eave + rise * .64f, kSimVoxelMaterial_Trim);
    return;
  }

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

  uint16_t wing_first = model->face_count;
  AddStandardBox(model, 0.7f, 4.0f, 0.0f, 6.4f, 15.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 1.1f, 5.0f, 1.5f, 6.0f, 14.3f, 6.5f,
                 kSimVoxelMaterial_WallLight);
  AddShedRoofX(model, 0.6f, 6.6f, 4.2f, 14.8f, 6.5f, 8.5f,
               kSimVoxelMaterial_RoofLight, kSimVoxelMaterial_WallLight);
  if (detail != kSimBackgroundVoxelDetail_Low) {
    AddFrontBand(model, 2.2f, 4.6f, 14.34f, 2.8f, 5.5f, kSimVoxelMaterial_Dark);
    AddFrontBand(model, 2.0f, 4.8f, 14.35f, 5.5f, 5.8f, kSimVoxelMaterial_Trim);
  }
  AddHouseRoofCourses(model, detail, wing_first);
}

static void AddCathedralFacadeDecorations(
    SimBackgroundVoxelModel *model,
    SimBackgroundVoxelDetail detail) {
  (void)detail;
  const float y = 30.53f;
  /* Shallow relief mounted on the main pediment. Every vertex lies inside
   * its triangle; the wings and body touch and share the backing plane. */
  AddFace(model, kSimVoxelMaterial_Gold, 240,
          Point(15.3f, y, 17.5f), Point(16.7f, y, 17.5f),
          Point(16.7f, y, 21.0f), Point(15.3f, y, 21.0f));
  AddFace(model, kSimVoxelMaterial_Gold, 240,
          Point(15.3f, y, 18.0f), Point(15.3f, y, 19.3f),
          Point(10.0f, y, 20.0f), Point(11.0f, y, 18.7f));
  AddFace(model, kSimVoxelMaterial_Gold, 240,
          Point(16.7f, y, 18.0f), Point(21.0f, y, 18.7f),
          Point(22.0f, y, 20.0f), Point(16.7f, y, 19.3f));
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
      /* Regional builders own their attached trim. Generic eave boxes do not
       * follow canvas, tapered walls or the different regional roof heights. */
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
      AddCathedralFacadeDecorations(model, detail);
      break;
    case kSimBackgroundVoxel_Windmill:
      /* A band follows the taper; rectangular corner posts would undo the
       * round tower silhouette and put trim back into the rotor sweep. */
      AddWindmillRoundSection(model, 7.25f, 7.20f, 8.0f, 8.8f,
                              kSimVoxelMaterial_Trim);
      break;
    case kSimBackgroundVoxel_Factory:
      for (int arm=0; arm<2; arm++)
        AddFrontBand(model,1,20.8f,arm ? 31.03f : 9.03f,8.45f,8.9f,kSimVoxelMaterial_Trim);
      break;
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Shrub:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_Boulder:
    case kSimBackgroundVoxel_Rocks:
      /* Natural silhouettes are complete in their own builders. */
      break;
    case kSimBackgroundVoxel_StoryTree:
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
    case kSimBackgroundVoxel_Pyramid:
      /* Landmark silhouettes carry their own authored trim. */
      break;
    case kSimBackgroundVoxel_Bridge:
      /* Native masonry already has its complete silhouette. */
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

static void AddHouseFacadeVariation(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *model,
    uint32_t seed) {
  SimBackgroundVoxelHouseStyle house_style =
      SimBackgroundVoxelRegion_ObjectHouseStyle(object);
  uint32_t variant_count =
      detail == kSimBackgroundVoxelDetail_Ultra ? 6u : 4u;
  uint32_t variant = seed % variant_count;

  /* Varied is deliberately a surface-detail tier. It may change the facade
   * rhythm, but it must not alter a source building's footprint, eave, ridge,
   * or authored height. That keeps High/Ultra useful without inventing bays,
   * dormers, porches, chimneys, and rooftop masses absent from the ROM art. */
  SimBackgroundVoxelMaterial accent = kSimVoxelMaterial_Trim;
  if (house_style == kSimBackgroundHouseStyle_Yurt) {
    /* Colour a connected straw panel. This adds no geometry or hidden faces. */
    int panel = (int)(variant % 8u);
    int at = 0;
    for (uint16_t f = 0; f < model->face_count; f++)
      if (model->faces[f].material == kSimVoxelMaterial_Wall && at++ == panel)
        model->faces[f].material = kSimVoxelMaterial_WallLight;
    return;
  }
  if (house_style == kSimBackgroundHouseStyle_Tent ||
      house_style == kSimBackgroundHouseStyle_WhiteTent) {
    for (uint16_t f = 0; f < model->face_count; f++)
      if (model->faces[f].material == kSimVoxelMaterial_Roof) {
        if ((variant & 1u) == 0) model->faces[f].material = kSimVoxelMaterial_RoofLight;
        break;
      }
    return;
  }
  /* A raised dwelling's accents belong above its floor. */
  if (house_style == kSimBackgroundHouseStyle_MarahnaStilt) {
    AddFrontBand(model, variant & 1u ? 10.0f : 3.0f,
                  variant & 1u ? 12.8f : 5.8f, 14.56f,
                  5.5f + .35f * (variant % 3u), 6.0f + .35f * (variant % 3u), accent);
    return;
  }
  /* Subtle material variation stays on the authored wall/roof surfaces.
   * The old generic facade boxes could lie in a window plane, float above a
   * shorter regional wall, or cover the very timber courses being shown. */
  for (uint16_t f = 0; f < model->face_count; f++) {
    SimBackgroundVoxelModelFace *face = &model->faces[f];
    if (face->material == kSimVoxelMaterial_Wall ||
        face->material == kSimVoxelMaterial_WallLight)
      face->brightness = (uint8_t)(210 + variant * 6);
    else if (face->material == kSimVoxelMaterial_Roof && face->brightness == 172)
      face->brightness = (uint8_t)(155 + variant * 5);
  }
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
      AddHouseFacadeVariation(object, detail, model, seed);
      break;
    }
    case kSimBackgroundVoxel_Factory: {
      uint8_t shade = (uint8_t)(202 + (seed % 3u)*14);
      for (int face=0; face<model->face_count; face++)
        if (model->faces[face].material == kSimVoxelMaterial_WallLight)
          model->faces[face].brightness = shade;
      break;
    }
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_Boulder:
    case kSimBackgroundVoxel_Rocks:
    case kSimBackgroundVoxel_Shrub:
    case kSimBackgroundVoxel_BroadTree:
      /* Seeded crown profiles already vary the outline. Appending cuboids at
       * this stage produced detached branch blocks on only a few sides. */
      break;
    case kSimBackgroundVoxel_Cathedral:
    case kSimBackgroundVoxel_Windmill:
    case kSimBackgroundVoxel_StoryTree:
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
    case kSimBackgroundVoxel_Pyramid:
      /* Unique town landmarks do not need random silhouettes. */
      break;
    case kSimBackgroundVoxel_Bridge:
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

static void BuildAuthoredModel(
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
  out->face_budget = SimBackgroundVoxelModel_ObjectFaceBudget(object, detail);
  out->min_x = out->min_y = out->min_z = FLT_MAX;
  out->max_x = out->max_y = out->max_z = -FLT_MAX;
  if (!object) return;

  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      BuildHouse(object, detail, out);
      if (!(object->flags & kSimBackgroundVoxel_UnderConstruction)) {
        SimBackgroundVoxelHouseStyle family = SimBackgroundVoxelRegion_ObjectHouseStyle(object);
        if (family != kSimBackgroundHouseStyle_Yurt && family != kSimBackgroundHouseStyle_Tent &&
            family != kSimBackgroundHouseStyle_WhiteTent)
          AddHouseRoofCourses(out, detail, 0);
        AddTerraceCourses(object, detail, out);
      }
      /* Apply surface variation before alternate-facing houses compress and
       * shift the complete assembly, so every attached detail follows it. */
      if (style >= kSimBackgroundVoxelStyle_Varied)
        BuildDeterministicVariation(object, detail, out);
      if (!(object->flags & kSimBackgroundVoxel_UnderConstruction) &&
          (object->flags & kSimBackgroundVoxel_AlternateFacing))
        BuildAlternateFacingHouse(object, detail, out);
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
    case kSimBackgroundVoxel_BroadTree:
      BuildBroadTree(object, detail, out);
      break;
    case kSimBackgroundVoxel_Palm:
      BuildPalm(object, detail, out);
      break;
    case kSimBackgroundVoxel_Shrub:
      BuildShrub(object, detail, out);
      break;
    case kSimBackgroundVoxel_Boulder:
    case kSimBackgroundVoxel_Rocks:
      BuildRocks(object, out);
      break;
    case kSimBackgroundVoxel_StoryTree:
      BuildStoryTree(detail, out);
      break;
    case kSimBackgroundVoxel_BloodpoolCastle:
      BuildBloodpoolCastle(detail, out);
      break;
    case kSimBackgroundVoxel_MarahnaTemple:
      BuildMarahnaTemple(detail, out);
      break;
    case kSimBackgroundVoxel_Pyramid:
      BuildPyramid(object, detail, out);
      break;
    case kSimBackgroundVoxel_Bridge:
      BuildStoneBridge(object, detail, out);
      break;
  }
  if (object->kind != kSimBackgroundVoxel_Bridge &&
      style >= kSimBackgroundVoxelStyle_Trim)
    BuildSilhouetteTrim(object, detail, out);
  if (object->kind != kSimBackgroundVoxel_Bridge &&
      style >= kSimBackgroundVoxelStyle_Architectural)
    BuildFactoryCourtyardDetails(object, detail, out);
  if (style >= kSimBackgroundVoxelStyle_Varied &&
      object->kind != kSimBackgroundVoxel_House &&
      object->kind != kSimBackgroundVoxel_Bridge)
    BuildDeterministicVariation(object, detail, out);
  if (object->kind == kSimBackgroundVoxel_House) {
    /* Shorter roof depth exposes the facade without inflating every house's
     * world height. Transform the full assembly and its occlusion metadata. */
    float scale = HouseDepthScale(object);
    for (uint16_t f = 0; f < out->face_count; f++)
      for (int v = 0; v < 4; v++)
        out->faces[f].points[v].y = HouseDepth(out->faces[f].points[v].y, scale);
    for (uint16_t b = 0; b < out->box_count; b++) {
      out->boxes[b].y0 = HouseDepth(out->boxes[b].y0, scale);
      out->boxes[b].y1 = HouseDepth(out->boxes[b].y1, scale);
    }
  }

  RecomputeModelBounds(out);

}

void SimBackgroundVoxelModel_BuildStyled(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    SimBackgroundVoxelModel *out) {
  BuildAuthoredModel(object, detail, style, out);
  if (out && object) FinalizeModelSurface(out);
}

float SimBackgroundVoxelModel_HeightBound(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail maximum_detail,
    SimBackgroundVoxelStyle style) {
  SimBackgroundVoxelModelBounds bounds;
  return SimBackgroundVoxelModel_MeasureBounds(object, maximum_detail, style, &bounds)
      ? fmaxf(0.0f, bounds.max_z) : 0.0f;
}

bool SimBackgroundVoxelModel_MeasureBounds(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail maximum_detail,
    SimBackgroundVoxelStyle style,
    SimBackgroundVoxelModelBounds *out) {
  if (!out) return false;
  *out = (SimBackgroundVoxelModelBounds){0};
  if (!object || object->kind >= kSimBackgroundVoxelKindCount) return false;
  if (maximum_detail < kSimBackgroundVoxelDetail_Low ||
      maximum_detail >= kSimBackgroundVoxelDetail_Count)
    maximum_detail = kSimBackgroundVoxelDetail_High;
  SimBackgroundVoxelObject pose = *object;
  SimBackgroundVoxelModel model;
  bool measured = false;
  const int phases = object->kind == kSimBackgroundVoxel_Windmill ? 3 : 1;
  for (int phase = 0; phase < phases; phase++) {
    if (phases > 1) pose.animation_phase = (uint8_t)phase;
    for (int detail = kSimBackgroundVoxelDetail_Low;
         detail <= (int)maximum_detail; detail++) {
      BuildAuthoredModel(&pose, (SimBackgroundVoxelDetail)detail, style, &model);
      /* Surface finalization only removes faces and writes shading. Measure
       * its input envelope without paying for duplicate removal/corner AO or
       * filling the shared render cache with offscreen LODs. */
      if (!model.face_count) continue;
      if (!measured) {
        *out = (SimBackgroundVoxelModelBounds){model.min_x, model.min_y, model.min_z,
            model.max_x, model.max_y, model.max_z};
        measured = true;
      } else {
        out->min_x = fminf(out->min_x, model.min_x);
        out->min_y = fminf(out->min_y, model.min_y);
        out->min_z = fminf(out->min_z, model.min_z);
        out->max_x = fmaxf(out->max_x, model.max_x);
        out->max_y = fmaxf(out->max_y, model.max_y);
        out->max_z = fmaxf(out->max_z, model.max_z);
      }
    }
  }
  return measured;
}
