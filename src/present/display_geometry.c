#include "present/display_geometry.h"

#include <stdint.h>

#include "constants.h"

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
  s_geometry.widescreen_active = render_extra > 0;
}

void DisplayGeometry_SetVertical(int extra_top, int extra_bottom) {
  s_geometry.extra_top = extra_top;
  s_geometry.extra_bottom = extra_bottom;
}
