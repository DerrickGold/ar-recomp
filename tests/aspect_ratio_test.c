/* Supported display contracts, including the camera/capture distinction.
 * Exercise the production sizing function without a window or ROM. */
#include <stdint.h>
#include <limits.h>
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
    {2560, 1080}, {3440, 1440}, {5120, 1440},
    {1080, 1080}, {800, 1200}, {997, 613},
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

static void CheckProducerCoverage(void) {
  const struct {
    int width, height;
    int square_columns, crt_columns;
    int square_rows, crt_rows;
  } cases[] = {
    {1920, 1080, 72, 43, 0, 0},
    {1920, 1200, 52, 26, 0, 0},
    {2520, 1080, 134, 96, 0, 0}, /* exact 21:9 */
    {3440, 1440, 140, 102, 0, 0},
    {5120, 1440, 271, 214, 0, 0},
    {1080, 1080, 0, 0, 16, 37},
    {1080, 1920, 0, 0, 116, 153},
  };
  /* A future world producer can supply more coverage without changing
   * viewport math. These budgets do not certify current AR1/AR2 streaming. */
  const int budgets[] = {0, 120, 128, 320};
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    for (int crt = 0; crt <= 1; ++crt) {
      const int columns = crt ? cases[i].crt_columns : cases[i].square_columns;
      const int rows = crt ? cases[i].crt_rows : cases[i].square_rows;
      for (unsigned b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
        ArPresentationCanvas resolved;
        ArPresentationCanvasLimits limits = {256, 224, budgets[b], 64};
        CHECK(ArPresentationLayout_ResolveCanvas(
            cases[i].width, cases[i].height, crt, limits, &resolved));
        CHECK(resolved.extra_columns == (columns > budgets[b] ? budgets[b] : columns));
        CHECK(resolved.extra_rows == (rows > 64 ? 64 : rows));
        CHECK(resolved.coverage_limited == (columns > budgets[b] || rows > 64));
        const int width = 256 + 2 * resolved.extra_columns;
        const int height = 224 + 2 * resolved.extra_rows;
        ArRenderRectI viewport = ArPresentationLayout_ResolveViewport(
            cases[i].width, cases[i].height, false, crt, width, height);
        CHECK(viewport.w > 0 && viewport.h > 0);
        CHECK(viewport.x >= 0 && viewport.y >= 0);
        CHECK(viewport.x + viewport.w <= cases[i].width);
        CHECK(viewport.y + viewport.h <= cases[i].height);
        if (resolved.coverage_limited) {
          CHECK(viewport.w < cases[i].width || viewport.h < cases[i].height);
        }
        CheckViewport(width, crt);
      }
    }
  }
  /* Game/scene dimensions are supplied by the adapter, not assumed SNES 224. */
  ArPresentationCanvas resolved = {43, 0, true};
  const ArPresentationCanvasLimits tall_native = {256, 240, 320, 64};
  CHECK(ArPresentationLayout_ResolveCanvas(1920, 1080, false, tall_native, &resolved));
  CHECK(resolved.extra_columns == 86 && !resolved.coverage_limited);
  CHECK(!ArPresentationLayout_ResolveCanvas(0, 1080, false, tall_native, &resolved));
  CHECK(resolved.extra_columns == 86 && !resolved.coverage_limited);
  CHECK(!ArPresentationLayout_ResolveCanvas(1, 1, false, tall_native, NULL));
  CHECK(!ArPresentationLayout_ResolveCanvas(1, 1, false,
      (ArPresentationCanvasLimits){256, 224, -1, 0}, &resolved));
  CHECK(!ArPresentationLayout_ResolveCanvas(1, 1, false,
      (ArPresentationCanvasLimits){INT_MAX, 224, 0, 0}, &resolved));
  CHECK(ArPresentationLayout_ResolveCanvas(INT_MAX, 1, true, tall_native, &resolved));
  CHECK(resolved.extra_columns == 320 && resolved.coverage_limited);
  CHECK(ArPresentationLayout_ResolveCanvas(1, INT_MAX, true, tall_native, &resolved));
  CHECK(resolved.extra_rows == 64 && resolved.coverage_limited);
}

int main(void) {
  CheckSupportedRatios();
  CheckLimits();
  CheckProducerCoverage();
  puts("aspect_ratio_test: PASS");
  return 0;
}
