#ifndef AR_DIORAMA_CAMERA_POLICY_H
#define AR_DIORAMA_CAMERA_POLICY_H

#include "diorama.h"
#include "diorama_scene_extent.h"
#include "action/action_camera_bounds.h"

/* Capture policy shared by gameplay, presentation and the whole-room editor.
 * Callers supply the plan AFTER saved BG overrides and verified source
 * availability. Storage headroom alone never makes terrain available. */
static inline bool DioramaCameraPolicy_FiniteLayer(const ActionBgLayerPlan *layer) {
  return layer && layer->valid && layer->source == kActionBgSource_WorldMap &&
      !layer->wrap_world_x && layer->default_edge == kActionBgEdge_LiveWorld &&
      !layer->band_count && layer->horizontal_extent.mode == kActionBgExtent_Available;
}

static inline bool DioramaCameraPolicy_WorldApron(const ActionBgLayerPlan *layer) {
  return layer && layer->valid && layer->source == kActionBgSource_WorldMap &&
      layer->default_edge == kActionBgEdge_LiveWorld && !layer->band_count &&
      layer->horizontal_extent.mode == kActionBgExtent_Available &&
      layer->vertical_extent.mode == kActionBgExtent_Available;
}

static inline bool DioramaCameraPolicy_HasEdits(const DioramaRoomOverride *room, unsigned bg) {
  return room && bg < 2 && (room->stamp_layers[bg].count ||
      room->pixel_layers[bg].count || room->framing[0].x);
}

static inline bool DioramaCameraPolicy_NeedsBgApron(const DioramaRoomOverride *room,
    unsigned bg, bool pixel_edits, bool world_available, int aspect_x, int aspect_y) {
  return bg < 2 && ((pixel_edits && room &&
      (room->framing[0].x || room->stamp_layers[bg].count)) ||
      (world_available && aspect_x > 0 && aspect_y > 0 &&
       (int64_t)aspect_x * 10 > (int64_t)aspect_y * 16));
}

static inline void DioramaCameraPolicy_HorizontalMargins(const ActionBgPlan *plan,
    const DioramaRoomOverride *room, int extra, int *before, int *after) {
  *before = *after = 0;
  if (ActionBgPlan_CanvasOwner(plan) != 0 || extra <= 0) return;
  const ActionBgHorizontalExtent *extent = &plan->layer[0].horizontal_extent;
  *before = extent->mode == kActionBgExtent_Fixed && extent->left < extra ? extent->left : extra;
  *after = extent->mode == kActionBgExtent_Fixed && extent->right < extra ? extent->right : extra;
  int start, width;
  DioramaScenery_AxisExtent(room, 0, plan->layer[0].world_width, false, &start, &width);
  ActionCameraAxisBounds_ApplySceneryPadding(plan->layer[0].world_width, start, width, before, after);
}

static inline void DioramaCameraPolicy_VerticalMargins(
    int y, int height, int budget, int *top, int *bottom) {
  if (budget < 0) budget = 0;
  if (budget > 64) budget = 64;
  /* Scanline 1 samples camera + 1: world row 0 belongs to the upper margin.
   * At an edge, spend the unused budget on actual rows on the other side. */
  int above = y >= 0 ? y + 1 : 0, below = height - 225 - y;
  if (below < 0) below = 0;
  int t = above < budget ? above : budget;
  int b = below < budget ? below : budget;
  const int remaining = budget * 2 - t - b;
  if (b < budget) t += above - t < remaining ? above - t : remaining;
  else if (t < budget) b += below - b < remaining ? below - b : remaining;
  if (top) *top = t;
  if (bottom) *bottom = b;
}

/* Geometry only; textures/effects are populated by each capture backend. The
 * camera solver uses this same rectangle instead of inventing guard columns. */
static inline DioramaCapture DioramaCameraPolicy_Capture(const ActionBgPlan *plan,
    const DioramaRoomOverride *room, int x, int y, int extra, int budget, uint8_t apron_mask) {
  const ActionBgLayerPlan *layer = &plan->layer[0];
  int x0, width, y0, height, top, bottom;
  DioramaScenery_AxisExtent(room, 0, layer->world_width, false, &x0, &width);
  DioramaScenery_AxisExtent(room, 0, layer->world_height, true, &y0, &height);
  if (ActionBgPlan_PrimaryLayer(plan) < 0) budget = 0;
  DioramaCameraPolicy_VerticalMargins(y - y0, height, budget, &top, &bottom);
  const bool playfield = ActionBgPlan_PlayfieldLayer(plan) == 0;
  return (DioramaCapture){.width = 256 + 2 * extra, .height = 224 + top + bottom,
    .authentic_y0 = top, .obj_apron = 64, .bg_apron_mask = apron_mask,
    .horizontal_bounds = playfield && DioramaCameraPolicy_FiniteLayer(layer)
        ? DioramaHorizontalBounds_Resolve(0, x, x0, width, 256 + 2 * extra, (apron_mask & 1) ? 64 : 0)
        : (DioramaHorizontalBounds){0},
    .vertical_bounds = playfield
        ? DioramaVerticalBounds_Resolve(0, y - y0, height, top, 224 + top + bottom)
        : (DioramaVerticalBounds){0},
    .framing_x = room ? room->framing[0].x : 0, .framing_y = room ? room->framing[0].y : 0};
}
#endif
