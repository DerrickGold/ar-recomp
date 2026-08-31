#include "display_geometry.h"

static ActRaiserDisplayGeometry s_geometry;

const ActRaiserDisplayGeometry *const g_actraiser_display_geometry =
    &s_geometry;

void DisplayGeometry_SetHorizontal(int render_extra, int display_extra) {
  s_geometry.render_extra = render_extra;
  s_geometry.display_extra = display_extra;
  s_geometry.widescreen_active = render_extra > 0;
}

void DisplayGeometry_SetVertical(int extra_top, int extra_bottom) {
  s_geometry.extra_top = extra_top;
  s_geometry.extra_bottom = extra_bottom;
}
