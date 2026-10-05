#include "present/display_geometry.h"

#include <stdint.h>

#include "constants.h"
#include "actraiser_game.h"

bool DisplayGeometry_ResolveAutoCanvas(
    int drawable_width, int drawable_height, bool crt_pixel_aspect,
    ActRaiserAutoCanvas *canvas) {
  if (!canvas || drawable_width <= 0 || drawable_height <= 0) return false;
  const int64_t par_x = crt_pixel_aspect ? 7 : 1;
  const int64_t par_y = crt_pixel_aspect ? 6 : 1;
  const int64_t width = drawable_width;
  const int64_t height = drawable_height;
  *canvas = (ActRaiserAutoCanvas){0};
  /* Share horizontal rounding with explicit ratios so Auto reveals the same
   * columns on a matching drawable. Aspect fit absorbs whole-pixel rounding;
   * never stretch, crop, or exceed the capture/streaming caps. */
  if (width * 224 * par_y > height * 256 * par_x) {
    canvas->extra_columns = DisplayGeometry_CalculateHorizontal(
        kActRaiserAuthenticHeight, drawable_width, drawable_height,
        crt_pixel_aspect, false).display_extra;
  } else {
    const int64_t denominator = 2 * width * par_y;
    int64_t extra = (height * 256 * par_x - width * 224 * par_y +
                     denominator / 2) / denominator;
    canvas->extra_rows = extra > 64 ? 64 : (int)extra;
  }
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
      map_number == kActRaiserNonActionMap_SkyPalace)
    return (ActRaiserAutoCanvas){.extra_columns = requested.extra_columns};
  return (ActRaiserAutoCanvas){0};
}

static ActRaiserDisplayGeometry s_geometry;

const ActRaiserDisplayGeometry *const g_actraiser_display_geometry =
    &s_geometry;

ActRaiserDisplayGeometry DisplayGeometry_CalculateHorizontal(
    int height, int aspect_x, int aspect_y,
    bool crt_pixel_aspect, bool diorama_mode) {
  int extra = 0;
  if (height > 0 && aspect_x > 0 && aspect_y > 0) {
    const int64_t numerator =
        (int64_t)height * aspect_x * (crt_pixel_aspect ? 6 : 7);
    const int64_t denominator = 7LL * aspect_y;
    const int64_t width = (numerator + denominator - 1) / denominator;
    if (width > kActRaiserAuthenticWidth) {
      const int64_t requested = (width - kActRaiserAuthenticWidth + 1) / 2;
      extra = requested > kActRaiserWidescreenExtraMax
          ? kActRaiserWidescreenExtraMax : (int)requested;
    }
  }
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

void DisplayGeometry_SetVertical(int extra_top, int extra_bottom) {
  s_geometry.extra_top = extra_top;
  s_geometry.extra_bottom = extra_bottom;
}
