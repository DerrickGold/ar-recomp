/* Supported display contracts, including the camera/capture distinction.
 * Exercise the production sizing function without a window or ROM. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "action/action_camera_bounds.h"
#include "present/display_geometry.h"
#include "render/presentation_layout.h"

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
  } \
} while (0)

static void CheckViewport(int width, bool crt) {
  const int outputs[][2] = {
    {640, 480}, {1920, 1080}, {1920, 1200}, {3840, 2160},
    {800, 1200}, {997, 613},
  };
  for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
    const int w = outputs[i][0], h = outputs[i][1];
    const ArRenderRectI r = ArPresentationLayout_ResolveViewport(
        w, h, false, crt, width, 224);
    CHECK(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0);
    CHECK(r.x + r.w <= w && r.y + r.h <= h);
    CHECK(r.w == w || r.h == h);
    CHECK(abs(w - r.w - 2 * r.x) <= 1);
    CHECK(abs(h - r.h - 2 * r.y) <= 1);
    const int logical_w = width * (crt ? 7 : 1);
    const int logical_h = 224 * (crt ? 6 : 1);
    const int64_t error = (int64_t)r.w * logical_h -
        (int64_t)r.h * logical_w;
    /* Integer letterboxing may round down by less than one output pixel. */
    CHECK(llabs(error) < (logical_w > logical_h ? logical_w : logical_h));
  }
}

static void CheckScrollLimits(int margin) {
  ActionCameraAxisBounds bounds;
  CHECK(ActionCameraAxisBounds_Resolve(768, 256, margin, margin, &bounds));
  CHECK(bounds.minimum == margin && bounds.maximum == 512 - margin);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      margin + 1, -2, 768, 256, margin, margin, &bounds) == margin);
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      511 - margin, 2, 768, 256, margin, margin, &bounds) == 512 - margin);
  /* A room transition to a one-screen arena must not retain wide stops. */
  CHECK(ActionCameraAxisBounds_UpdateCamera(
      120, 1, 256, 256, margin, margin, &bounds) == 0);
  CHECK(bounds.minimum == 0 && bounds.maximum == 0);
  CHECK(!bounds.includes_requested_margins);
  /* Extra diorama rows do not move the native floor or ceiling stops. */
  CHECK(ActionCameraAxisBounds_UpdateNativeCamera(
      286, 2, 512, 225, &bounds) == 287);
  CHECK(ActionCameraAxisBounds_UpdateNativeCamera(
      1, -2, 512, 225, &bounds) == 0);
}

static void CheckSupportedRatios(void) {
  const struct {
    int x, y;
    int square_extra, crt_extra;
  } ratios[] = {
    {0, 0, 0, 0}, /* 4:3 setting retains the native 256-pixel viewport. */
    {16, 9, 72, 43},
    {16, 10, 52, 26},
  };
  for (unsigned i = 0; i < sizeof(ratios) / sizeof(ratios[0]); i++) {
    for (int crt = 0; crt <= 1; crt++) {
      for (int diorama = 0; diorama <= 1; diorama++) {
        const int extra = crt ? ratios[i].crt_extra : ratios[i].square_extra;
        const int render = diorama && extra ? 120 : extra;
        const ActRaiserDisplayGeometry g = DisplayGeometry_CalculateHorizontal(
            224, ratios[i].x, ratios[i].y, crt, diorama);
        CHECK(g.display_extra == extra && g.render_extra == render);
        CHECK(g.widescreen_active == (extra > 0));
        CHECK(g.extra_top == 0 && g.extra_bottom == 0);
        CHECK(g.display_extra <= g.render_extra);
        CHECK(g.render_extra <= kActRaiserWidescreenExtraMax);
        if (ratios[i].x > 0) {
          ActRaiserAutoCanvas canvas;
          CHECK(DisplayGeometry_ResolveAutoCanvas(
              ratios[i].x * 120, ratios[i].y * 120, crt, &canvas));
          CHECK(canvas.extra_columns == g.display_extra);
          CHECK(canvas.extra_rows == 0);
        }
        CheckViewport(256 + 2 * extra, crt);
        CheckScrollLimits(render);
      }
    }
  }
  /* Square-pixel native output is intentionally 8:7; CRT PAR makes it 4:3. */
  const ArRenderRectI native = ArPresentationLayout_ResolveViewport(
      1920, 1080, false, true, 256, 224);
  CHECK(native.x == 240 && native.y == 0 && native.w == 1440 && native.h == 1080);
}

static void CheckLimits(void) {
  /* 21:9 is not certified: square-pixel width would need 134 per side,
   * beyond the existing 120-pixel streaming budget. Do not silently raise it. */
  ActRaiserDisplayGeometry g = DisplayGeometry_CalculateHorizontal(224, 21, 9, false, false);
  CHECK(g.render_extra == 120 && g.display_extra == 120);
  g = DisplayGeometry_CalculateHorizontal(224, 0, 9, true, true);
  CHECK(!g.widescreen_active && g.render_extra == 0 && g.display_extra == 0);
  g = DisplayGeometry_CalculateHorizontal(0, 16, 9, false, true);
  CHECK(!g.widescreen_active);
}

int main(void) {
  CheckSupportedRatios();
  CheckLimits();
  puts("aspect_ratio_test: PASS");
  return 0;
}
