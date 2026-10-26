#include "present/display_geometry.h"

#include "constants.h"
#include "actraiser_game.h"
#include "render/presentation_layout.h"

bool DisplayGeometry_ResolveAutoCanvas(
    int drawable_width, int drawable_height, bool crt_pixel_aspect,
    ActRaiserAutoCanvas *canvas) {
  if (!canvas) return false;
  ArPresentationCanvas resolved;
  const ArPresentationCanvasLimits limits = {
    kActRaiserAuthenticWidth, kActRaiserAuthenticHeight,
    kActRaiserWidescreenExtraMax, 64,
  };
  if (!ArPresentationLayout_ResolveCanvas(
          drawable_width, drawable_height, crt_pixel_aspect, limits, &resolved))
    return false;
  *canvas = (ActRaiserAutoCanvas){resolved.extra_columns, resolved.extra_rows};
  return true;
}

ActRaiserAutoCanvas DisplayGeometry_ConstrainAutoCanvas(
    ActRaiserAutoCanvas requested, int map_group, int map_number, int bg_mode,
    bool projected_sim) {
  if (ActRaiser_IsActionMapGroup(map_group) && bg_mode >= 0 && bg_mode != 7)
    return requested;
  if (ActRaiser_IsSimulationTown(map_group, map_number) ||
      (map_group == kActRaiserMapGroup_NonAction &&
       map_number == kActRaiserNonActionMap_WorldMap)) {
    if (!projected_sim) requested.extra_rows = 0;
    return requested;
  }
  if (map_group == kActRaiserMapGroup_NonAction &&
      (map_number == kActRaiserNonActionMap_SkyPalace ||
       map_number == kActRaiserNonActionMap_Temple))
    return (ActRaiserAutoCanvas){.extra_columns = requested.extra_columns};
  return (ActRaiserAutoCanvas){0};
}

static ActRaiserDisplayGeometry s_geometry;

const ActRaiserDisplayGeometry *const g_actraiser_display_geometry =
    &s_geometry;

ActRaiserDisplayGeometry DisplayGeometry_CalculateHorizontal(
    int height, int aspect_x, int aspect_y,
    bool crt_pixel_aspect, bool diorama_mode) {
  const int extra = ArPresentationLayout_ResolveHorizontalMargin(
      kActRaiserAuthenticWidth, height, aspect_x, aspect_y,
      crt_pixel_aspect, kActRaiserWidescreenExtraMax);
  return (ActRaiserDisplayGeometry){
    .widescreen_active = extra > 0,
    .render_extra = diorama_mode && extra > 0
        ? kActRaiserWidescreenExtraMax : extra,
    .display_extra = extra,
  };
}

void DisplayGeometry_SetHorizontal(int render_extra, int display_extra) {
  s_geometry.render_extra = render_extra;
  s_geometry.display_extra = display_extra;
  s_geometry.widescreen_active =
      render_extra > 0 || s_geometry.auto_vertical_budget > 0;
}

void DisplayGeometry_SetAutoVerticalBudget(int budget) {
  s_geometry.auto_vertical_budget = budget;
}

void DisplayGeometry_SetOutputSize(int width, int height) {
  if (width <= 0 || height <= 0) return;
  s_geometry.output_width = width;
  s_geometry.output_height = height;
}

void DisplayGeometry_SetVertical(int extra_top, int extra_bottom) {
  s_geometry.extra_top = extra_top;
  s_geometry.extra_bottom = extra_bottom;
}
