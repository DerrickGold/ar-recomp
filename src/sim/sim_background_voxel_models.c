#include "sim_background_voxel_models.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "sim_background_bridge.h"
#include "sim_background_voxel_region.h"

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
  /* A fixed octagon is enough to give yurts a round identity while retaining
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

static void BuildFillmoreHouse(const SimBackgroundVoxelObject *object,
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
  AddRoofedBox(model, 2.5f, 3.0f, 2.0f, 13.5f, 14.5f, 10.0f,
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

static void BuildTentHouse(SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 2.0f, 3.0f, 0.0f, 14.0f, 15.0f, 1.2f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 3.0f, 4.0f, 1.2f, 13.0f, 14.5f, 5.2f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 1.5f, 14.5f, 2.8f, 15.0f, 5.2f, 9.5f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 6.4f, 14.2f, 1.2f, 9.6f, 15.3f, 5.0f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 7.6f, 3.0f, 8.8f, 8.4f, 15.2f, 9.5f,
                 kSimVoxelMaterial_Wood);
  AddStandardBox(model, 2.0f, 14.0f, 4.8f, 14.0f, 15.2f, 5.4f,
                 kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddStandardBox(model, 1.2f, 2.2f, 0.0f, 2.0f, 3.0f, 2.0f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 14.0f, 2.2f, 0.0f, 14.8f, 3.0f, 2.0f,
                   kSimVoxelMaterial_Wood);
  }
}

static void BuildWhiteTentHouse(SimBackgroundVoxelDetail detail,
                                SimBackgroundVoxelModel *model) {
  /* Kasandora's middle-stage dwelling is a broad white canvas A-frame, not a
   * palette swap of the starter tent. A low eave and long uninterrupted roof
   * planes make that identity survive at the authentic town resolution. */
  AddStandardBox(model, 1.2f, 2.8f, 0.0f, 14.8f, 15.0f, 0.8f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.2f, 3.6f, 0.8f, 13.8f, 14.7f, 3.2f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 0.8f, 15.2f, 2.0f, 15.2f, 3.2f, 10.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  /* The dark opening is a split canvas flap climbing into the front gable;
   * keeping it broad prevents the tent from reading as a house with a door. */
  AddStandardBox(model, 6.1f, 14.8f, 0.8f, 9.9f, 15.35f, 5.8f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 7.65f, 14.75f, 3.0f, 8.35f, 15.5f, 9.6f,
                 kSimVoxelMaterial_Trim);
  AddStandardBox(model, 1.0f, 14.7f, 2.8f, 15.0f, 15.45f, 3.4f,
                 kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    /* Two visible tie-downs add authored construction detail while remaining
     * below the canvas silhouette and inside the one-cell footprint. */
    AddStandardBox(model, 1.0f, 2.2f, 0.0f, 1.7f, 3.0f, 1.8f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 14.3f, 2.2f, 0.0f, 15.0f, 3.0f, 1.8f,
                   kSimVoxelMaterial_Wood);
  }
}

static void BuildYurtHouse(SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 2.0f, 2.0f, 0.0f, 14.0f, 14.0f, 0.8f,
                 kSimVoxelMaterial_Trim);
  AddOctagonalFrustum(model, 8.0f, 8.0f, 6.0f, 5.6f, 0.8f, 4.6f,
                      kSimVoxelMaterial_Wall);
  AddOctagonalFrustum(model, 8.0f, 8.0f, 6.1f, 0.9f, 4.6f, 8.6f,
                      kSimVoxelMaterial_Roof);
  AddStandardBox(model, 7.25f, 7.25f, 8.35f, 8.75f, 8.75f, 8.8f,
                 kSimVoxelMaterial_RoofLight);
  AddStandardBox(model, 6.5f, 13.15f, 0.8f, 9.5f, 14.25f, 4.2f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 6.1f, 13.0f, 4.0f, 9.9f, 14.4f, 4.6f,
                 kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddStandardBox(model, 3.0f, 10.8f, 2.0f, 4.8f, 12.5f, 3.6f,
                   kSimVoxelMaterial_WallLight);
    AddStandardBox(model, 11.2f, 10.8f, 2.0f, 13.0f, 12.5f, 3.6f,
                   kSimVoxelMaterial_WallLight);
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
  AddStandardBox(model, 6.5f, 14.0f, 1.4f, 9.5f, 15.3f, 6.2f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 3.5f, 14.0f, 3.4f, 5.8f, 15.2f, 5.9f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 10.2f, 14.0f, 3.4f, 12.5f, 15.2f, 5.9f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 2.6f, 14.0f, 6.9f, 13.4f, 15.2f, 7.7f,
                 kSimVoxelMaterial_Wood);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddStandardBox(model, 2.7f, 14.1f, 1.5f, 3.3f, 15.4f, 7.1f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 12.7f, 14.1f, 1.5f, 13.3f, 15.4f, 7.1f,
                   kSimVoxelMaterial_Wood);
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
  AddRoofedBox(model, 1.0f, 7.0f, 1.0f, 4.5f, 14.8f, 6.5f,
               kSimVoxelMaterial_WallLight);
  AddShedRoofX(model, 0.5f, 5.0f, 6.5f, 15.0f, 6.5f, 8.0f,
               kSimVoxelMaterial_RoofLight,
               kSimVoxelMaterial_WallLight);
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
  AddStandardBox(model, 6.2f, 14.0f, 1.3f, 9.8f, 15.3f, 7.2f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 3.2f, 14.0f, 4.0f, 5.2f, 15.2f, 6.4f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 10.8f, 14.0f, 4.0f, 12.8f, 15.2f, 6.4f,
                 kSimVoxelMaterial_Dark);
  if (detail >= kSimBackgroundVoxelDetail_High)
    AddStandardBox(model, 5.6f, 4.0f, 10.1f, 10.4f, 8.0f, 10.8f,
                   kSimVoxelMaterial_WallLight);
}

static void BuildStoneHouse(SimBackgroundVoxelDetail detail,
                            SimBackgroundVoxelModel *model) {
  AddStandardBox(model, 1.0f, 2.0f, 0.0f, 15.0f, 15.0f, 1.5f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.0f, 3.0f, 1.5f, 14.0f, 14.5f, 10.0f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 1.0f, 15.0f, 2.0f, 15.0f, 10.0f, 13.5f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 6.2f, 14.0f, 1.5f, 9.8f, 15.3f, 7.5f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  for (int side = 0; side < 2; side++) {
    float x0 = side ? 10.7f : 3.2f;
    AddStandardBox(model, x0, 14.0f, 4.2f, x0 + 2.1f, 15.2f, 7.0f,
                   kSimVoxelMaterial_Dark);
  }
  AddStandardBox(model, 2.0f, 14.0f, 8.9f, 14.0f, 15.2f, 10.1f,
                 kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int course = 0; course < 3; course++) {
      float z = 2.3f + course * 2.4f;
      AddStandardBox(model, 2.0f, 14.3f, z, 5.2f, 15.1f, z + 0.45f,
                     kSimVoxelMaterial_WallLight);
      AddStandardBox(model, 10.8f, 14.3f, z, 14.0f, 15.1f, z + 0.45f,
                     kSimVoxelMaterial_WallLight);
    }
  }
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
  AddStandardBox(model, 5.8f, 14.0f, 1.5f, 9.2f, 15.3f, 6.7f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 2.8f, 14.0f, 3.8f, 4.8f, 15.2f, 6.2f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 11.2f, 14.0f, 3.8f, 13.2f, 15.2f, 6.2f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 1.8f, 14.0f, 7.9f, 14.2f, 15.2f, 8.8f,
                 kSimVoxelMaterial_Trim);
  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int course = 0; course < 3; course++) {
      float z = 2.5f + course * 1.8f;
      AddStandardBox(model, 1.8f, 14.2f, z, 5.0f, 15.1f, z + 0.35f,
                     kSimVoxelMaterial_WallLight);
      AddStandardBox(model, 11.0f, 14.2f, z, 14.2f, 15.1f, z + 0.35f,
                     kSimVoxelMaterial_WallLight);
    }
    /* A low square roof hatch adds close-range depth without introducing a
     * ridge, pitch, or triangular end face. */
    AddStandardBox(model, 10.4f, 5.0f, 9.5f, 13.0f, 8.0f, 10.3f,
                   kSimVoxelMaterial_WallLight);
  }
}

static void BuildMarahnaStiltHouse(SimBackgroundVoxelDetail detail,
                                   SimBackgroundVoxelModel *model) {
  for (int post = 0; post < 4; post++) {
    float x = post & 1 ? 12.5f : 2.5f;
    float y = post & 2 ? 13.5f : 4.0f;
    AddStandardBox(model, x, y, 0.0f, x + 1.0f, y + 1.0f, 3.0f,
                   kSimVoxelMaterial_Wood);
  }
  AddStandardBox(model, 1.5f, 3.0f, 2.5f, 14.5f, 15.0f, 4.0f,
                 kSimVoxelMaterial_Trim);
  AddRoofedBox(model, 2.5f, 4.0f, 4.0f, 13.5f, 14.5f, 8.0f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 0.8f, 15.2f, 2.5f, 15.0f, 8.0f, 12.5f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 6.3f, 14.0f, 4.0f, 9.7f, 15.3f, 7.5f,
                 kSimVoxelMaterial_Dark);
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  AddStandardBox(model, 3.2f, 14.0f, 5.0f, 5.4f, 15.2f, 7.0f,
                 kSimVoxelMaterial_Dark);
  AddStandardBox(model, 10.6f, 14.0f, 5.0f, 12.8f, 15.2f, 7.0f,
                 kSimVoxelMaterial_Dark);
  if (detail >= kSimBackgroundVoxelDetail_High)
    AddStandardBox(model, 1.0f, 14.2f, 7.6f, 15.0f, 15.5f, 8.3f,
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
  if (detail == kSimBackgroundVoxelDetail_Low) return;
  for (int course = 0; course < 3; course++) {
    float z = 2.1f + course * 1.9f;
    AddStandardBox(model, 2.0f, 14.0f, z, 6.0f, 15.25f, z + 0.55f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 10.0f, 14.0f, z, 14.0f, 15.25f, z + 0.55f,
                   kSimVoxelMaterial_Wood);
  }
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

static void BuildHouse(const SimBackgroundVoxelObject *object,
                       SimBackgroundVoxelDetail detail,
                       SimBackgroundVoxelModel *model) {
  switch (SimBackgroundVoxelRegion_HouseStyle(
      object->town, object->development_level)) {
    case kSimBackgroundHouseStyle_Tent:
      BuildTentHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Timber:
      BuildTimberHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Fillmore:
      BuildFillmoreHouse(object, detail, model);
      break;
    case kSimBackgroundHouseStyle_Bloodpool:
      BuildBloodpoolHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_Yurt:
      BuildYurtHouse(detail, model);
      break;
    case kSimBackgroundHouseStyle_WhiteTent:
      BuildWhiteTentHouse(detail, model);
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
      BuildFillmoreHouse(object, detail, model);
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

/* The sail's spar, laid along the blade it decorates.
 *
 * It is drawn in the BLADE material at the blade's own back-face shade, not in
 * Wood, and that is the whole point of it. The strip is 0.56 model units wide;
 * across the entire size band in which it is authored to appear it measures
 * 0.4 to 0.9 SCREEN pixels, against a blade 1.7 to 3.5 pixels wide (High
 * enters at a projected model height of 24px, Ultra at 42px -- see
 * SimBackgroundVoxelLod_Resolve). A sub-pixel quad does not thin out; it
 * claims every whole pixel whose centre it crosses. In Wood it claimed those
 * pixels in the mill's own dark brown, so a run of them read as the frame
 * showing THROUGH the blade -- the rotor looked severed wherever it crossed
 * the gable or the hub. Kept in the blade's own ramp, the same lost pixels
 * are a shading line: the silhouette stays continuous at every size, and the
 * spar still reads as a spar once the camera is close enough to resolve it. */
static void AddBladeInlay(SimBackgroundVoxelModel *model,
                          float cx, float cy, float cz,
                          float dx, float dz) {
  const float inner = 3.2f, outer = 9.2f, half_width = 0.28f;
  const float px = -dz, pz = dx;
  AddFace(model, kSimVoxelMaterial_Blade, 190,
          Point(cx + dx * inner + px * half_width, cy + 0.64f,
                cz + dz * inner + pz * half_width),
          Point(cx + dx * outer + px * half_width, cy + 0.64f,
                cz + dz * outer + pz * half_width),
          Point(cx + dx * outer - px * half_width, cy + 0.64f,
                cz + dz * outer - pz * half_width),
          Point(cx + dx * inner - px * half_width, cy + 0.64f,
                cz + dz * inner - pz * half_width));
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
/* Front face of the wall detail the blades sweep past. It is more than one
 * model unit behind the rotor's back face at every phase. */
static const float kWindmillWallDetailFront = 14.6f;

static void BuildWindmill(const SimBackgroundVoxelObject *object,
                          SimBackgroundVoxelDetail detail,
                          SimBackgroundVoxelModel *model) {
  int phase = object->animation_phase % kWindmillFrameCount;
  if (object->flags & kSimBackgroundVoxel_UnderConstruction) {
    /* The authentic scaffold grows across its three frames rather than
     * standing at full height from the first one. */
    float progress = (float)(phase + 1) / (float)kWindmillFrameCount;
    AddStandardBox(model, 8.0f, 3.0f, 0.0f, 24.0f, 15.0f,
                   4.0f + 8.0f * progress, kSimVoxelMaterial_Wall);
    BuildConstructionFrame(model, 32.0f, 16.0f, 8.0f + 16.0f * progress);
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
  AddStandardBox(model, 14.0f, 14.1f, 18.0f, 18.0f,
                 kWindmillWallDetailFront, 22.0f, kSimVoxelMaterial_Wood);

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
  /* The blades span y = plane .. plane + 0.6. What they were cutting through
   * is the wall detail that used to reach y = 15.2 -- the hub frame over
   * x 14-18, z 18-22, wider than the blades' 2.2 inner radius, and the two
   * upper windows at a radius the blade TIPS sweep. Both are now held back to
   * kWindmillWallDetailFront so the rotor passes cleanly in front. */
  AddBlade(model, 16.0f, kWindmillBladePlane, 21.0f, dx, dz);
  AddBlade(model, 16.0f, kWindmillBladePlane, 21.0f, -dz, dx);
  AddBlade(model, 16.0f, kWindmillBladePlane, 21.0f, -dx, -dz);
  AddBlade(model, 16.0f, kWindmillBladePlane, 21.0f, dz, -dx);
  AddStandardBox(model, 14.1f, kWindmillBladePlane + 0.35f, 19.1f, 17.9f,
                 kWindmillBladePlane + kWindmillHubCapCover, 22.7f,
                 kSimVoxelMaterial_Wood);

  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    /* Balanced gained nothing over Low here: the mill's only step between the
     * two came from the Trim style, so a player on Basic style saw the same
     * model at both quality levels. The stage door and its landing are the
     * lowest-frequency thing the mill can add. */
    AddStandardBox(model, 12.5f, 13.6f, 2.0f, 19.5f, 15.6f, 2.9f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 8.5f, 3.2f, 17.4f, 23.5f, 14.6f, 18.4f,
                   kSimVoxelMaterial_Trim);
  }
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddBladeInlay(model, 16.0f, kWindmillBladePlane, 21.0f, dx, dz);
    AddBladeInlay(model, 16.0f, kWindmillBladePlane, 21.0f, -dz, dx);
    AddBladeInlay(model, 16.0f, kWindmillBladePlane, 21.0f, -dx, -dz);
    AddBladeInlay(model, 16.0f, kWindmillBladePlane, 21.0f, dz, -dx);
    AddStandardBox(model, 9.5f, 14.0f, 10.0f, 12.5f,
                   kWindmillWallDetailFront, 14.0f, kSimVoxelMaterial_Dark);
    AddStandardBox(model, 19.5f, 14.0f, 10.0f, 22.5f,
                   kWindmillWallDetailFront, 14.0f, kSimVoxelMaterial_Dark);
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

/* Every canopy in the town is the same stepped cube crown; only its profile,
 * scale and colour ramp change. Keeping one builder is what makes a bush, a
 * mangrove and the ancient tree read as the same authored vegetation style
 * while staying individually recognizable. */
typedef enum SimBackgroundCrownShape {
  /* Pointed and tiered. */
  kCrownShape_Evergreen,
  /* Squat sphere, widest below the middle: the $01 bush. */
  kCrownShape_Bush,
  /* Broad rounded canopy with a flatter underside: the mangrove family and,
   * at landmark scale, Northwall's ancient tree. */
  kCrownShape_Broad,
} SimBackgroundCrownShape;

typedef struct SimBackgroundCrownGeometry {
  /* Half-open span the crown occupies in model X/Y, and its Z extent. */
  float center_x, center_y, span, base_z, height;
} SimBackgroundCrownGeometry;

/* Shadow, mid and highlight, bottom-up. The story tree passes snow as its
 * highlight so its sunlit faces are white without recolouring the underside. */
typedef struct SimBackgroundCrownRamp {
  SimBackgroundVoxelMaterial dark, mid, light;
} SimBackgroundCrownRamp;

static bool EvergreenCrownVoxel(float nx, float ny, float height,
                                int z, int resolution, uint32_t profile,
                                SimBackgroundVoxelTreeStyle style) {
  if (profile == 1u) nx -= height > 0.35f ? 0.10f : -0.04f;
  if (profile == 2u) ny += height > 0.55f ? 0.12f : -0.05f;
  float radius = 1.02f - height * (profile == 3u ? 0.96f : 0.90f);
  if (style == kSimBackgroundTreeStyle_Dryland) radius -= 0.10f;
  if (style == kSimBackgroundTreeStyle_Highland) radius -= 0.04f;
  if (style == kSimBackgroundTreeStyle_Wetland) radius += 0.04f;
  if (style == kSimBackgroundTreeStyle_Tropical && height < 0.58f)
    radius += 0.10f;
  if (style == kSimBackgroundTreeStyle_SnowFir && height < 0.72f)
    radius += 0.08f;
  if (profile == 2u && height < 0.38f) radius += 0.08f;
  /* A small flare at branch-tier boundaries keeps the evergreen readable as
   * layered voxel foliage while preserving one unmistakable pointed crown. */
  int tier = resolution >= 7 ? 3 : 2;
  int tier_offset = (int)(profile & 1u);
  if (z + 1 < resolution && (z + tier_offset) % tier == 0)
    radius += profile == 3u ? 0.13f : 0.08f;
  return nx * nx + ny * ny <= radius * radius;
}

/* Half-width of a unit sphere sliced at `height`, with the widest ring at
 * `waist` and a vertical squash so the shape stays a ball rather than a cone.
 * `scale` below 1 is what keeps the widest ring inside the grid corners; at 1
 * the crown fills its whole cell and reads as a cube however round the maths
 * is. */
static float RoundCrownRadius(float height, float waist, float scale) {
  float t = (height - waist) / (height >= waist ? 1.0f - waist : waist);
  float squared = 1.0f - t * t;
  return squared <= 0.0f ? 0.0f : sqrtf(squared) * scale;
}

static bool CrownVoxel(SimBackgroundCrownShape shape,
                       int x, int y, int z, int resolution,
                       uint32_t seed,
                       SimBackgroundVoxelTreeStyle style) {
  float nx = ((x + 0.5f) * 2.0f / resolution) - 1.0f;
  float ny = ((y + 0.5f) * 2.0f / resolution) - 1.0f;
  float height = (z + 0.5f) / resolution;
  uint32_t profile = seed % 4u;
  if (shape == kCrownShape_Evergreen) {
    /* One unmistakable apex voxel. Rounded crowns must not take it, or a bush
     * grows a spike. */
    if (z + 1 == resolution)
      return x == resolution / 2 && y == resolution / 2;
    return EvergreenCrownVoxel(
        nx, ny, height, z, resolution, profile, style);
  }
  float radius = shape == kCrownShape_Bush
      ? RoundCrownRadius(height, 0.46f, 0.90f)
      : RoundCrownRadius(height, 0.36f, 0.97f);
  /* Seeded lean and a lumpy shoulder: the source art is a cluster of rounded
   * clumps, not a billiard ball. */
  if (profile == 1u) nx -= 0.08f;
  if (profile == 2u) ny += 0.08f;
  if (profile == 3u) radius -= 0.05f;
  int tier = resolution >= 7 ? 3 : 2;
  if (z + 1 < resolution && (z + (int)(profile & 1u)) % tier == 0)
    radius += 0.07f;
  return nx * nx + ny * ny <= radius * radius;
}

static SimBackgroundVoxelMaterial CrownMaterial(
    const SimBackgroundCrownRamp *ramp,
    uint32_t seed, int x, int y, int z, int resolution,
    SimBackgroundVoxelDetail detail) {
  if (z * 4 < resolution) return ramp->dark;
  if (z * 3 >= resolution * 2) return ramp->light;
  if (detail >= kSimBackgroundVoxelDetail_High) {
    uint32_t patch = seed ^ (uint32_t)x * 0x9E37u ^
        (uint32_t)y * 0x7F4Au ^ (uint32_t)z * 0x45D9u;
    if (patch % 13u == 0) return ramp->light;
    if (patch % 17u == 0) return ramp->dark;
  }
  return ramp->mid;
}

static void BuildVoxelCrown(SimBackgroundVoxelDetail detail,
                            SimBackgroundVoxelModel *model, uint32_t seed,
                            SimBackgroundCrownShape shape,
                            const SimBackgroundCrownGeometry *geometry,
                            const SimBackgroundCrownRamp *ramp,
                            SimBackgroundVoxelTreeStyle style) {
  bool occupied[kTreeCrownMaxResolution][kTreeCrownMaxResolution]
               [kTreeCrownMaxResolution] = {{{false}}};
  /* A cone fills about a third of its grid and a ball more than half, so the
   * round shapes reach the same authored budget at a coarser step. Pushing
   * them to the evergreen's ladder only overflows the box table, which
   * truncates the crown rather than refining it. */
  int resolution = shape == kCrownShape_Evergreen
      ? DetailChoice(detail, 3, 5, 7, 9)
      : DetailChoice(detail, 3, 5, 6, 7);
  for (int z = 0; z < resolution; z++)
    for (int y = 0; y < resolution; y++)
      for (int x = 0; x < resolution; x++)
        occupied[x][y][z] = CrownVoxel(
            shape, x, y, z, resolution, seed, style);

  const float crown_x0 = geometry->center_x - geometry->span * 0.5f;
  const float crown_y0 = geometry->center_y - geometry->span * 0.5f;
  const float voxel_xy = geometry->span / resolution;
  const float voxel_z = geometry->height / resolution;
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
               geometry->base_z + z * voxel_z,
               crown_x0 + (x + 1) * voxel_xy,
               crown_y0 + (y + 1) * voxel_xy,
               geometry->base_z + (z + 1) * voxel_z,
               CrownMaterial(ramp, seed, x, y, z, resolution, detail),
               faces);
      }

  /* Neighbour metadata is deliberately not turned into geometry. Earlier
   * connector cuboids made only some sides of a tree sprout square foliage
   * bars, especially at the edge of a forest. Adjacent crowns may overlap
   * naturally after projection without changing either silhouette. */
}

static const SimBackgroundCrownRamp kLeafRamp = {
  kSimVoxelMaterial_LeavesDark,
  kSimVoxelMaterial_Leaves,
  kSimVoxelMaterial_LeavesLight,
};

static uint32_t FoliageSeed(const SimBackgroundVoxelObject *object) {
  return (uint32_t)object->cell_x * 0x45D9F3Bu ^
      (uint32_t)object->cell_y * 0x119DE1F3u ^
      (uint32_t)object->group * 0x3449u;
}

static void BuildTree(const SimBackgroundVoxelObject *object,
                      SimBackgroundVoxelDetail detail,
                      SimBackgroundVoxelModel *model) {
  uint32_t seed = FoliageSeed(object);
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
  SimBackgroundVoxelTreeStyle tree_style =
      SimBackgroundVoxelRegion_TreeStyle(object->town);
  SimBackgroundCrownGeometry crown = {
    .center_x = 8.0f + offset_x,
    .center_y = 8.0f + offset_y,
    .span = 14.0f,
    .base_z = 3.5f,
    .height = tree_style == kSimBackgroundTreeStyle_SnowFir ? 12.5f : 11.5f,
  };
  BuildVoxelCrown(detail, model, seed, kCrownShape_Evergreen,
                  &crown, &kLeafRamp, tree_style);
}

static void BuildBroadTree(const SimBackgroundVoxelObject *object,
                           SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  /* The broad canopy family ($05-$07/$0D-$0F/$15-$17/$1D-$1F/$26/$27) is a
   * mass of rounded clumps over a short, visibly forked trunk - Marahna's
   * mangroves. Same cube crown as the evergreen so the two sit together as one
   * style, but round rather than pointed, and lower and wider. */
  uint32_t seed = FoliageSeed(object);
  float offset_x = ((int)(seed & 3u) - 1.5f) * 0.16f;
  float offset_y = ((int)((seed >> 2) & 3u) - 1.5f) * 0.16f;

  AddStandardBox(model, 6.5f, 6.5f, 0.0f, 9.5f, 9.5f, 4.6f,
                 kSimVoxelMaterial_Trunk);
  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    /* The exposed roots the source art shows below the canopy. */
    AddStandardBox(model, 4.4f, 7.0f, 0.0f, 6.7f, 9.0f, 2.4f,
                   kSimVoxelMaterial_Trunk);
    AddStandardBox(model, 9.3f, 7.0f, 0.0f, 11.6f, 9.0f, 2.6f,
                   kSimVoxelMaterial_Trunk);
  }
  /* Broader and flatter than the bush, and taller, so the two round crowns
   * are never mistaken for each other at map scale. */
  SimBackgroundCrownGeometry crown = {
    .center_x = 8.0f + offset_x,
    .center_y = 8.0f + offset_y,
    .span = 14.5f,
    .base_z = 3.4f,
    .height = 10.6f,
  };
  BuildVoxelCrown(detail, model, seed, kCrownShape_Broad,
                  &crown, &kLeafRamp,
                  SimBackgroundVoxelRegion_TreeStyle(object->town));
}

static void BuildShrub(const SimBackgroundVoxelObject *object,
                       SimBackgroundVoxelDetail detail,
                       SimBackgroundVoxelModel *model) {
  /* Metatile $01: one bright round crown 13 source pixels across, standing on
   * a short trunk the art draws as a 5x2 shadowed stub with a single brown
   * pixel. It uses the evergreen's cube crown so the town reads as one
   * vegetation style, and stays distinct by being round, low and bright. */
  uint32_t seed = FoliageSeed(object);
  float offset_x = ((int)(seed & 3u) - 1.5f) * 0.22f;
  float offset_y = ((int)((seed >> 2) & 3u) - 1.5f) * 0.22f;
  float center_x = 8.0f + offset_x;
  float center_y = 8.0f + offset_y;

  AddStandardBox(model, center_x - 1.3f, center_y - 1.3f, 0.0f,
                 center_x + 1.3f, center_y + 1.3f, 4.0f,
                 kSimVoxelMaterial_Trunk);
  if (detail >= kSimBackgroundVoxelDetail_High)
    AddStandardBox(model, center_x - 1.9f, center_y - 1.9f, 0.0f,
                   center_x + 1.9f, center_y + 1.9f, 0.9f,
                   kSimVoxelMaterial_Trunk);

  /* Taller than wide, like the 13x15 source sprite, and lifted clear of the
   * ground so the trunk is actually visible under it. */
  SimBackgroundCrownGeometry crown = {
    .center_x = center_x,
    .center_y = center_y,
    .span = 12.0f,
    .base_z = 3.0f,
    .height = 9.0f,
  };
  BuildVoxelCrown(detail, model, seed, kCrownShape_Bush,
                  &crown, &kLeafRamp,
                  SimBackgroundVoxelRegion_TreeStyle(object->town));
}

static void AddPalmFrondSegment(
    SimBackgroundVoxelModel *model,
    float start_x, float start_y, float start_z, float start_half_width,
    float end_x, float end_y, float end_z, float end_half_width,
    SimBackgroundVoxelMaterial material) {
  float dx = end_x - start_x, dy = end_y - start_y;
  float length = sqrtf(dx * dx + dy * dy);
  if (length <= 0.01f) return;
  float perpendicular_x = -dy / length;
  float perpendicular_y = dx / length;
  const float thickness = 0.42f;
  SimBackgroundVoxelModelPoint start_left = Point(
      start_x + perpendicular_x * start_half_width,
      start_y + perpendicular_y * start_half_width, start_z);
  SimBackgroundVoxelModelPoint start_right = Point(
      start_x - perpendicular_x * start_half_width,
      start_y - perpendicular_y * start_half_width, start_z);
  SimBackgroundVoxelModelPoint end_left = Point(
      end_x + perpendicular_x * end_half_width,
      end_y + perpendicular_y * end_half_width, end_z);
  SimBackgroundVoxelModelPoint end_right = Point(
      end_x - perpendicular_x * end_half_width,
      end_y - perpendicular_y * end_half_width, end_z);
  SimBackgroundVoxelModelPoint start_left_bottom = start_left;
  SimBackgroundVoxelModelPoint start_right_bottom = start_right;
  SimBackgroundVoxelModelPoint end_left_bottom = end_left;
  SimBackgroundVoxelModelPoint end_right_bottom = end_right;
  start_left_bottom.z -= thickness;
  start_right_bottom.z -= thickness;
  end_left_bottom.z -= thickness;
  end_right_bottom.z -= thickness;
  AddFace(model, material, 245,
          start_left, end_left, end_right, start_right);
  AddFace(model, material, 178,
          start_right_bottom, end_right_bottom,
          end_left_bottom, start_left_bottom);
  AddFace(model, material, 204,
          start_left_bottom, end_left_bottom, end_left, start_left);
  AddFace(model, material, 218,
          start_right, end_right, end_right_bottom, start_right_bottom);
  AddFace(model, material, 190,
          end_left_bottom, end_right_bottom, end_right, end_left);
}

static void AddPalmFrond(
    SimBackgroundVoxelModel *model,
    float center_x, float center_y,
    float direction_x, float direction_y,
    SimBackgroundVoxelMaterial material) {
  float middle_x = center_x + direction_x * 3.5f;
  float middle_y = center_y + direction_y * 3.5f;
  float end_x = center_x + direction_x * 6.8f;
  float end_y = center_y + direction_y * 6.8f;
  AddPalmFrondSegment(model,
                      center_x + direction_x * 0.35f,
                      center_y + direction_y * 0.35f,
                      12.45f, 1.15f,
                      middle_x, middle_y, 12.7f, 0.85f,
                      material);
  AddPalmFrondSegment(model,
                      middle_x - direction_x * 0.1f,
                      middle_y - direction_y * 0.1f,
                      12.72f, 0.9f,
                      end_x, end_y, 10.05f, 0.32f,
                      material);
}

static void BuildPalm(const SimBackgroundVoxelObject *object,
                      SimBackgroundVoxelDetail detail,
                      SimBackgroundVoxelModel *model) {
  uint32_t seed = (uint32_t)object->cell_x * 0x45D9F3Bu ^
      (uint32_t)object->cell_y * 0x119DE1F3u ^
      (uint32_t)object->group * 0x3449u;
  float lean_x = (seed & 1u) ? 0.45f : -0.45f;
  float lean_y = (seed & 2u) ? 0.30f : -0.30f;

  /* A stepped, slightly leaning trunk keeps the same broad planar language as
   * the buildings. Tapered two-segment fronds preserve the source palm fan;
   * the former cardinal cuboids read as a floating voxel plus sign. */
  AddStandardBox(model, 6.5f, 6.5f, 0.0f, 9.5f, 9.5f, 5.8f,
                 kSimVoxelMaterial_Trunk);
  AddStandardBox(model, 6.8f + lean_x, 6.8f + lean_y, 5.4f,
                 9.2f + lean_x, 9.2f + lean_y, 11.8f,
                 kSimVoxelMaterial_Trunk);
  float center_x = 8.0f + lean_x;
  float center_y = 8.0f + lean_y;
  AddStandardBox(model, center_x - 1.8f, center_y - 1.8f, 11.2f,
                 center_x + 1.8f, center_y + 1.8f, 13.2f,
                 kSimVoxelMaterial_LeavesLight);
  AddPalmFrond(model, center_x, center_y, 1.0f, 0.0f,
               kSimVoxelMaterial_Leaves);
  AddPalmFrond(model, center_x, center_y, -1.0f, 0.0f,
               kSimVoxelMaterial_LeavesDark);
  AddPalmFrond(model, center_x, center_y, 0.0f, 1.0f,
               kSimVoxelMaterial_LeavesLight);
  AddPalmFrond(model, center_x, center_y, 0.0f, -1.0f,
               kSimVoxelMaterial_Leaves);

  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    AddPalmFrond(model, center_x, center_y, 0.7071f, 0.7071f,
                 kSimVoxelMaterial_LeavesLight);
    AddPalmFrond(model, center_x, center_y, 0.7071f, -0.7071f,
                 kSimVoxelMaterial_Leaves);
    AddPalmFrond(model, center_x, center_y, -0.7071f, 0.7071f,
                 kSimVoxelMaterial_Leaves);
    AddPalmFrond(model, center_x, center_y, -0.7071f, -0.7071f,
                 kSimVoxelMaterial_LeavesDark);
  }
  if (detail >= kSimBackgroundVoxelDetail_High) {
    AddStandardBox(model, center_x - 1.0f, center_y - 1.0f, 12.8f,
                   center_x + 1.0f, center_y + 1.0f, 14.7f,
                   kSimVoxelMaterial_LeavesLight);
    AddStandardBox(model, 6.4f + lean_x, 8.8f + lean_y, 10.5f,
                   7.6f + lean_x, 10.0f + lean_y, 12.0f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 8.4f + lean_x, 8.8f + lean_y, 10.5f,
                   9.6f + lean_x, 10.0f + lean_y, 12.0f,
                   kSimVoxelMaterial_Wood);
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    AddStandardBox(model, 6.25f, 6.25f, 4.2f, 9.75f, 9.75f, 4.7f,
                   kSimVoxelMaterial_Wood);
    AddStandardBox(model, 6.55f + lean_x, 6.55f + lean_y, 8.0f,
                   9.45f + lean_x, 9.45f + lean_y, 8.5f,
                   kSimVoxelMaterial_Wood);
  }
}

static void BuildStoryTree(SimBackgroundVoxelDetail detail,
                           SimBackgroundVoxelModel *model) {
  /* Northwall's ancient tree is the 2x2 $EB plot at (26,14): a single broad
   * snow-laden canopy about 22 source pixels across, resting on a short knot
   * of exposed roots. It is the same cube crown the forest uses, at landmark
   * scale - a smooth frustum dome read as a different material entirely and
   * sat oddly beside every other piece of vegetation in the town. */
  AddStandardBox(model, 11.5f, 12.5f, 0.0f, 20.5f, 21.5f, 5.2f,
                 kSimVoxelMaterial_Trunk);
  AddStandardBox(model, 13.0f, 14.0f, 4.5f, 19.0f, 20.0f, 8.0f,
                 kSimVoxelMaterial_Wood);
  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    /* Roots flaring out from under the canopy, kept inside its outline. */
    AddStandardBox(model, 8.0f, 14.0f, 0.0f, 11.8f, 20.0f, 3.0f,
                   kSimVoxelMaterial_Trunk);
    AddStandardBox(model, 20.2f, 14.0f, 0.0f, 24.0f, 20.0f, 3.2f,
                   kSimVoxelMaterial_Trunk);
  }

  /* Snow is the crown's highlight rather than a separate slab, so the sunlit
   * faces are white and the underside keeps the town's subdued forest greens
   * without any box breaking the silhouette. */
  static const SimBackgroundCrownRamp kSnowCrown = {
    kSimVoxelMaterial_LeavesDark,
    kSimVoxelMaterial_LeavesLight,
    kSimVoxelMaterial_Snow,
  };
  SimBackgroundCrownGeometry crown = {
    .center_x = 16.0f,
    .center_y = 16.0f,
    .span = 23.0f,
    .base_z = 5.0f,
    .height = 22.5f,
  };
  /* A fixed seed: this is one authored landmark, not a member of a forest
   * whose neighbours should each lean differently. */
  BuildVoxelCrown(detail, model, 2u, kCrownShape_Broad,
                  &crown, &kSnowCrown, kSimBackgroundTreeStyle_SnowFir);
}

static void BuildBloodpoolCastle(SimBackgroundVoxelDetail detail,
                                 SimBackgroundVoxelModel *model) {
  /* The 2x2 $EC plot at (6,16). The source art is a pale stone keep with a
   * broad tan dome, four gold-capped spires and a colonnaded front terrace -
   * not the purple factory-sized fort this used to build. Everything below is
   * in that plot's own 32x32 pixels. */
  /* Low detail spends its whole 64-face budget on the silhouette, so the
   * rounded dome and spire cones become boxes rather than disappearing. */
  bool rounded = detail >= kSimBackgroundVoxelDetail_Balanced;
  AddStandardBox(model, 1.0f, 6.0f, 0.0f, 31.0f, 31.0f, 3.0f,
                 kSimVoxelMaterial_Trim);
  /* The dome covers only the middle of the hall, so the hall keeps its own
   * roof plane; the compiler drops whatever the dome actually buries. */
  AddStandardBox(model, 4.0f, 8.0f, 3.0f, 28.0f, 24.0f, 17.0f,
                 kSimVoxelMaterial_Wall);
  if (rounded)
    /* Front terrace. */
    AddStandardBox(model, 1.5f, 21.0f, 3.0f, 30.5f, 30.5f, 10.5f,
                   kSimVoxelMaterial_WallLight);

  /* Central domed keep. */
  if (rounded) {
    AddOctagonalFrustum(model, 16.0f, 15.0f, 8.4f, 7.0f, 15.0f, 22.0f,
                        kSimVoxelMaterial_Roof);
    AddOctagonalFrustum(model, 16.0f, 15.0f, 7.0f, 2.6f, 22.0f, 27.0f,
                        kSimVoxelMaterial_RoofLight);
  } else {
    AddRoofedBox(model, 8.0f, 8.0f, 15.0f, 24.0f, 22.0f, 24.0f,
                 kSimVoxelMaterial_Roof);
  }
  AddStandardBox(model, 14.2f, 13.2f, 26.6f, 17.8f, 16.8f, 29.0f,
                 kSimVoxelMaterial_Gold);

  /* Four corner spires: stone shafts with tan conical caps. */
  for (int corner = 0; corner < 4; corner++) {
    float x = corner & 1 ? 24.0f : 3.0f;
    float y = corner & 2 ? 22.5f : 9.0f;
    float shaft_z = corner & 2 ? 20.0f : 23.0f;
    AddRoofedBox(model, x, y, 3.0f, x + 5.0f, y + 5.0f, shaft_z,
                 kSimVoxelMaterial_WallLight);
    if (rounded)
      AddOctagonalFrustum(model, x + 2.5f, y + 2.5f, 3.1f, 0.4f,
                          shaft_z, shaft_z + 6.0f,
                          kSimVoxelMaterial_RoofLight);
    else
      AddStandardBox(model, x + 0.6f, y + 0.6f, shaft_z,
                     x + 4.4f, y + 4.4f, shaft_z + 6.0f,
                     kSimVoxelMaterial_RoofLight);
  }

  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    /* Terrace parapet and the arcade under the dome. */
    AddStandardBox(model, 1.2f, 29.6f, 10.5f, 30.8f, 31.2f, 12.6f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 4.0f, 22.6f, 12.0f, 28.0f, 24.4f, 14.0f,
                   kSimVoxelMaterial_Trim);
    AddStandardBox(model, 12.5f, 29.0f, 3.0f, 19.5f, 31.4f, 8.5f,
                   kSimVoxelMaterial_Dark);
  }
  if (detail >= kSimBackgroundVoxelDetail_High) {
    /* Window slits in the spires and the hall's dark arcade openings. */
    for (int corner = 0; corner < 2; corner++) {
      float x = corner ? 25.4f : 4.4f;
      AddStandardBox(model, x, 8.6f, 12.0f, x + 2.2f, 10.0f, 17.0f,
                     kSimVoxelMaterial_Dark);
    }
    for (int bay = 0; bay < 4; bay++) {
      float x = 6.5f + bay * 5.0f;
      AddStandardBox(model, x, 23.4f, 15.0f, x + 3.0f, 24.6f, 16.6f,
                     kSimVoxelMaterial_Dark);
      AddStandardBox(model, x + 0.4f, 30.2f, 3.4f,
                     x + 2.6f, 31.4f, 7.4f,
                     kSimVoxelMaterial_Glass);
    }
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    for (int step = 0; step < 3; step++)
      AddStandardBox(model, 13.0f + step, 30.0f + step * 0.6f,
                     step * 0.9f, 19.0f - step, 32.0f,
                     0.9f + step * 0.9f, kSimVoxelMaterial_Paving);
    AddStandardBox(model, 15.2f, 14.2f, 28.8f, 16.8f, 15.8f, 31.0f,
                   kSimVoxelMaterial_Gold);
  }
}

static void BuildPyramid(SimBackgroundVoxelDetail detail,
                         SimBackgroundVoxelModel *model) {
  /* Kasandora's 2x2 $EE plot at (20,4). The source art is a sandstone pyramid
   * whose lower half shows brick courses and whose upper half is smooth
   * casing, so the model steps four wide tiers and then tapers cleanly. */
  /* One constant taper: each step loses the same width per unit of height the
   * smooth casing above it does, so the whole thing reads as one pyramid
   * rather than a small cap sitting on a squat plinth. */
  static const float kTier[][3] = {
    /* inset from each edge, z0, z1 */
    {0.5f, 0.0f, 4.4f},
    {2.7f, 4.4f, 8.4f},
    {4.7f, 8.4f, 12.2f},
    {6.6f, 12.2f, 15.6f},
  };
  enum { kTiers = (int)(sizeof(kTier) / sizeof(kTier[0])) };
  for (int tier = 0; tier < kTiers; tier++) {
    float inset = kTier[tier][0];
    /* Keep each tier's top face: it is both the brick-course ledge the art
     * draws and the only thing closing the step above it. Roofing the tiers
     * instead left an open ring you could see straight through. */
    AddStandardBox(model, inset, inset + 1.0f, kTier[tier][1],
                   32.0f - inset, 32.0f - inset, kTier[tier][2],
                   tier & 1 ? kSimVoxelMaterial_Wall
                            : kSimVoxelMaterial_WallLight);
  }

  /* Smooth casing above the stepped base, tapering to the capstone. */
  int steps = DetailChoice(detail, 2, 3, 5, 6);
  const float cap_z0 = 15.6f, cap_z1 = 28.0f;
  const float cap_inset0 = 8.4f, cap_inset1 = 14.4f;
  for (int step = 0; step < steps; step++) {
    float low = (float)step / steps, high = (float)(step + 1) / steps;
    float inset_low = cap_inset0 + (cap_inset1 - cap_inset0) * low;
    float inset_high = cap_inset0 + (cap_inset1 - cap_inset0) * high;
    float z0 = cap_z0 + (cap_z1 - cap_z0) * low;
    float z1 = cap_z0 + (cap_z1 - cap_z0) * high;
    AddFace(model, kSimVoxelMaterial_WallLight, 232,
            Point(inset_low, 32.0f - inset_low, z0),
            Point(32.0f - inset_low, 32.0f - inset_low, z0),
            Point(32.0f - inset_high, 32.0f - inset_high, z1),
            Point(inset_high, 32.0f - inset_high, z1));
    AddFace(model, kSimVoxelMaterial_Wall, 204,
            Point(32.0f - inset_low, 32.0f - inset_low, z0),
            Point(32.0f - inset_low, inset_low, z0),
            Point(32.0f - inset_high, inset_high, z1),
            Point(32.0f - inset_high, 32.0f - inset_high, z1));
    AddFace(model, kSimVoxelMaterial_Wall, 190,
            Point(inset_low, inset_low, z0),
            Point(inset_low, 32.0f - inset_low, z0),
            Point(inset_high, 32.0f - inset_high, z1),
            Point(inset_high, inset_high, z1));
    AddFace(model, kSimVoxelMaterial_Wall, 178,
            Point(32.0f - inset_low, inset_low, z0),
            Point(inset_low, inset_low, z0),
            Point(inset_high, inset_high, z1),
            Point(32.0f - inset_high, inset_high, z1));
  }
  AddStandardBox(model, cap_inset1, cap_inset1, cap_z1 - 0.6f,
                 32.0f - cap_inset1, 32.0f - cap_inset1, cap_z1,
                 kSimVoxelMaterial_WallLight);

  if (detail >= kSimBackgroundVoxelDetail_High) {
    /* The dark lintel band the art draws where casing meets base. */
    AddStandardBox(model, 7.0f, 8.0f, 15.2f, 25.0f, 25.0f, 16.0f,
                   kSimVoxelMaterial_Dark);
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    AddStandardBox(model, 13.0f, 29.0f, 0.0f, 19.0f, 31.6f, 3.2f,
                   kSimVoxelMaterial_Dark);
    AddStandardBox(model, 12.4f, 29.4f, 3.2f, 19.6f, 31.6f, 4.0f,
                   kSimVoxelMaterial_Trim);
  }
}

static void BuildMarahnaTemple(SimBackgroundVoxelDetail detail,
                               SimBackgroundVoxelModel *model) {
  /* Marahna's sanctuary is the $C0 cathedral variant, so it occupies the same
   * 2x2 plot as every other town's cathedral. It stays a tropical stepped
   * shrine with a central stair rather than a recoloured cathedral. */
  AddStandardBox(model, 1.0f, 3.0f, 0.0f, 31.0f, 31.5f, 2.0f,
                 kSimVoxelMaterial_Paving);
  AddStandardBox(model, 3.5f, 4.5f, 2.0f, 28.5f, 29.5f, 4.0f,
                 kSimVoxelMaterial_Wall);
  AddStandardBox(model, 6.0f, 6.0f, 4.0f, 26.0f, 28.0f, 6.5f,
                 kSimVoxelMaterial_WallLight);
  AddRoofedBox(model, 8.5f, 8.0f, 6.5f, 23.5f, 28.0f, 14.0f,
               kSimVoxelMaterial_Wall);
  AddGableRoofX(model, 7.0f, 25.0f, 6.5f, 29.0f, 14.0f, 21.0f,
                kSimVoxelMaterial_Roof, kSimVoxelMaterial_WallLight);
  AddStandardBox(model, 13.5f, 26.5f, 6.0f, 18.5f, 31.5f, 13.0f,
                 kSimVoxelMaterial_Dark);

  if (detail >= kSimBackgroundVoxelDetail_Balanced) {
    for (int step = 0; step < 5; step++)
      AddStandardBox(model, 11.5f + step * 0.7f, 28.0f + step * 0.75f,
                     2.0f + step * 0.9f,
                     20.5f - step * 0.7f, 32.0f,
                     2.5f + step * 0.9f, kSimVoxelMaterial_WallLight);
    for (int side = 0; side < 2; side++) {
      float x = side ? 21.0f : 9.5f;
      AddStandardBox(model, x, 26.0f, 6.5f, x + 1.5f, 29.8f, 14.0f,
                     kSimVoxelMaterial_Trim);
    }
    AddStandardBox(model, 8.0f, 26.8f, 13.5f, 24.0f, 29.8f, 14.8f,
                   kSimVoxelMaterial_Gold);
  }
  if (detail >= kSimBackgroundVoxelDetail_High) {
    for (int panel = 0; panel < 2; panel++) {
      float x = panel ? 19.0f : 10.0f;
      AddStandardBox(model, x, 27.8f, 8.5f, x + 2.5f, 30.0f, 11.5f,
                     kSimVoxelMaterial_Dark);
      AddStandardBox(model, x + 0.5f, 28.0f, 9.0f,
                     x + 2.0f, 30.3f, 11.0f,
                     kSimVoxelMaterial_Gold);
    }
    AddStandardBox(model, 4.0f, 27.0f, 3.5f, 7.0f, 30.0f, 6.0f,
                   kSimVoxelMaterial_LeavesDark);
    AddStandardBox(model, 25.0f, 27.0f, 3.5f, 28.0f, 30.0f, 6.0f,
                   kSimVoxelMaterial_LeavesDark);
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    AddStandardBox(model, 14.5f, 28.5f, 14.5f, 17.5f, 30.8f, 17.0f,
                   kSimVoxelMaterial_Gold);
    AddStandardBox(model, 15.5f, 28.2f, 17.0f, 16.5f, 30.5f, 19.0f,
                   kSimVoxelMaterial_Gold);
    AddStandardBox(model, 3.0f, 28.5f, 1.8f, 8.0f, 31.0f, 2.6f,
                   kSimVoxelMaterial_Leaves);
    AddStandardBox(model, 24.0f, 28.5f, 1.8f, 29.0f, 31.0f, 2.6f,
                   kSimVoxelMaterial_Leaves);
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
      SimBackgroundVoxelRegion_HouseStyle(
          object->town, object->development_level);
  /* A yurt has no privileged side silhouette. Keeping its circular footprint
   * intact is both more faithful and avoids inventing a lean-to on alternate
   * records that merely selected another source perspective. */
  if (house_style == kSimBackgroundHouseStyle_Yurt) return;

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

  if (house_style == kSimBackgroundHouseStyle_Tent ||
      house_style == kSimBackgroundHouseStyle_WhiteTent) {
    AddStandardBox(model, 0.9f, 5.0f, 0.0f, 6.2f, 14.8f, 1.0f,
                   kSimVoxelMaterial_Trim);
    AddShedRoofX(model, 0.6f, 6.5f, 4.5f, 15.0f, 1.0f, 5.8f,
                 kSimVoxelMaterial_RoofLight,
                 kSimVoxelMaterial_WallLight);
    if (detail != kSimBackgroundVoxelDetail_Low)
      AddStandardBox(model, 2.3f, 14.0f, 1.0f, 4.7f, 15.1f, 3.8f,
                     kSimVoxelMaterial_Dark);
    return;
  }

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
    case kSimBackgroundVoxel_House: {
      SimBackgroundVoxelHouseStyle house_style =
          SimBackgroundVoxelRegion_HouseStyle(
              object->town, object->development_level);
      float eave_z = 9.5f;
      if (house_style == kSimBackgroundHouseStyle_Yurt ||
          house_style == kSimBackgroundHouseStyle_Adobe ||
          house_style == kSimBackgroundHouseStyle_Aitos)
        break;
      if (house_style == kSimBackgroundHouseStyle_Tent) eave_z = 4.8f;
      if (house_style == kSimBackgroundHouseStyle_WhiteTent) eave_z = 3.2f;
      if (house_style == kSimBackgroundHouseStyle_Timber) eave_z = 7.0f;
      if (house_style == kSimBackgroundHouseStyle_MarahnaStilt)
        eave_z = 7.6f;
      if (house_style == kSimBackgroundHouseStyle_MarahnaLogCabin)
        eave_z = 7.8f;
      if (object->flags & kSimBackgroundVoxel_AlternateFacing) {
        float wing_z = eave_z < 6.1f ? eave_z : 6.1f;
        AddStandardBox(model, 0.4f, 14.3f, wing_z, 6.8f, 15.6f,
                       wing_z + 0.7f,
                       kSimVoxelMaterial_Trim);
        AddStandardBox(model, 7.1f, 14.2f, eave_z, 14.2f, 15.6f,
                       eave_z + 0.7f,
                       kSimVoxelMaterial_Trim);
      } else {
        /* Broad fascia pieces produce a stable eave line at authentic output
         * resolution; tiny roof-edge cubes only reintroduce pixel noise. */
        AddStandardBox(model, 0.9f, 14.2f, eave_z, 15.1f, 15.7f,
                       eave_z + 0.7f,
                       kSimVoxelMaterial_Trim);
        AddStandardBox(model, 0.8f, 2.0f, eave_z, 1.6f, 15.2f,
                       eave_z + 0.7f,
                       kSimVoxelMaterial_Trim);
        AddStandardBox(model, 14.4f, 2.0f, eave_z, 15.2f, 15.2f,
                       eave_z + 0.7f,
                       kSimVoxelMaterial_Trim);
      }
      break;
    }
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
      AddStandardBox(model, 6.4f, 13.0f, 16.8f, 25.6f,
                     kWindmillWallDetailFront, 17.5f,
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
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_BroadTree:
      AddStandardBox(model, 5.7f, 6.8f, 0.0f, 10.3f, 9.2f, 1.0f,
                     kSimVoxelMaterial_Trunk);
      AddStandardBox(model, 6.8f, 5.7f, 0.0f, 9.2f, 10.3f, 1.0f,
                     kSimVoxelMaterial_Trunk);
      break;
    case kSimBackgroundVoxel_Shrub:
      /* A root flare at the foot of the trunk. Narrower than the other
       * families' because the bush's own trunk is a 5px stub. */
      AddStandardBox(model, 5.9f, 6.6f, 0.0f, 10.1f, 9.4f, 0.8f,
                     kSimVoxelMaterial_Trunk);
      AddStandardBox(model, 6.6f, 5.9f, 0.0f, 9.4f, 10.1f, 0.8f,
                     kSimVoxelMaterial_Trunk);
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
      SimBackgroundVoxelRegion_HouseStyle(
          object->town, object->development_level);
  uint32_t variant_count =
      detail == kSimBackgroundVoxelDetail_Ultra ? 6u : 4u;
  uint32_t variant = seed % variant_count;

  /* Varied is deliberately a surface-detail tier. It may change the facade
   * rhythm, but it must not alter a source building's footprint, eave, ridge,
   * or authored height. That keeps High/Ultra useful without inventing bays,
   * dormers, porches, chimneys, and rooftop masses absent from the ROM art. */
  float front_y0 = 14.55f, front_y1 = 15.2f;
  SimBackgroundVoxelMaterial accent = kSimVoxelMaterial_Trim;
  SimBackgroundVoxelMaterial secondary = kSimVoxelMaterial_WallLight;
  if (house_style == kSimBackgroundHouseStyle_Yurt) {
    front_y0 = 14.12f;
    front_y1 = 14.38f;
    float x0 = 5.4f + (float)(variant % 3u) * 1.6f;
    AddStandardBox(model, x0, front_y0, 2.4f,
                   x0 + 1.0f, front_y1, 4.4f, accent);
    if (detail == kSimBackgroundVoxelDetail_Ultra) {
      float other_x = 9.6f - (x0 - 5.4f);
      AddStandardBox(model, other_x, front_y0, 4.8f,
                     other_x + 0.8f, front_y1, 6.2f, secondary);
    }
    return;
  }
  if (house_style == kSimBackgroundHouseStyle_Tent ||
      house_style == kSimBackgroundHouseStyle_WhiteTent) {
    front_y0 = house_style == kSimBackgroundHouseStyle_WhiteTent
        ? 15.22f : 15.05f;
    front_y1 = house_style == kSimBackgroundHouseStyle_WhiteTent
        ? 15.55f : 15.45f;
    /* Canvas variants are seams and lashings, never masonry window bays. */
    float seam_x = 2.8f + (float)variant * 1.55f;
    AddStandardBox(model, seam_x, front_y0, 3.0f,
                   seam_x + 0.45f, front_y1, 6.6f, accent);
    if (detail == kSimBackgroundVoxelDetail_Ultra)
      AddStandardBox(model, 2.4f, front_y0, 5.1f,
                     13.6f, front_y1, 5.55f, secondary);
    return;
  }
  if (house_style == kSimBackgroundHouseStyle_Adobe ||
      house_style == kSimBackgroundHouseStyle_Aitos) {
    accent = kSimVoxelMaterial_WallLight;
    secondary = kSimVoxelMaterial_Trim;
  }

  switch (variant) {
    case 0:
      AddStandardBox(model, 2.8f, front_y0, 5.7f,
                     5.6f, front_y1, 6.25f, accent);
      break;
    case 1:
      AddStandardBox(model, 3.0f, front_y0, 2.7f,
                     3.55f, front_y1, 6.5f, accent);
      AddStandardBox(model, 5.05f, front_y0, 2.7f,
                     5.6f, front_y1, 6.5f, accent);
      break;
    case 2:
      AddStandardBox(model, 10.4f, front_y0, 3.0f,
                     13.2f, front_y1, 3.55f, accent);
      break;
    case 3:
      AddStandardBox(model, 11.0f, front_y0, 2.2f,
                     11.6f, front_y1, 6.6f, accent);
      break;
    case 4:
      AddStandardBox(model, 2.8f, front_y0, 4.8f,
                     5.4f, front_y1, 5.3f, accent);
      AddStandardBox(model, 10.6f, front_y0, 4.8f,
                     13.2f, front_y1, 5.3f, accent);
      break;
    case 5:
      AddStandardBox(model, 5.4f, front_y0, 6.2f,
                     10.6f, front_y1, 6.75f, accent);
      break;
  }
  if (detail == kSimBackgroundVoxelDetail_Ultra) {
    float x0 = (seed & 4u) ? 10.8f : 3.2f;
    AddStandardBox(model, x0, front_y0, 6.8f,
                   x0 + 1.8f, front_y1, 7.25f, secondary);
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
    case kSimBackgroundVoxel_Palm:
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
      BuildPyramid(detail, out);
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
  FinalizeModelSurface(out);
}
