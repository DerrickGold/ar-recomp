#include <math.h>
#include <stdio.h>
#include <string.h>

#include "diorama.h"
#include "render/scene3d_math.h"

static int s_failures;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (0)

static bool Near(float actual, float expected) { return fabsf(actual - expected) < 0.001f; }

static DioramaProjection Projection(void) {
  DioramaProjection projection = {
      .valid = true,
      .matrix =
          {
              1,
              0,
              0,
              0,
              0,
              1,
              0,
              0,
              0,
              0,
              1,
              0,
              0,
              0,
              0,
              1,
          },
      .aspect_x = 2.0f,
      .height_scale = 1.0f,
      .texture_width = 100,
      .texture_height = 50,
      .output_width = 100,
      .output_height = 100,
  };
  projection.bg1_plane = (DioramaPlaneProjection){
      .valid = true,
      .u1 = 1.0f,
      .v1 = 1.0f,
  };
  projection.bg2_plane = projection.bg1_plane;
  projection.object_planes[0] = projection.bg1_plane;
  return projection;
}

static void FocalBounds(const DioramaProjection *projection, float t0, float t1, float *top,
                        float *bottom) {
  *top = INFINITY;
  *bottom = -INFINITY;
  for (int row = 0; row <= kDioramaPlaneSubdivY; row++) {
    const float t = t0 + (t1 - t0) * (float)row / kDioramaPlaneSubdivY;
    for (int col = 0; col <= 8; col++) {
      ArRenderPointF p;
      CHECK(Diorama_ProjectCapturedBg1Point(projection, (float)col * 100.0f / 8.0f, t * 50.0f, &p,
                                            NULL, NULL));
      *top = fminf(*top, p.y);
      *bottom = fmaxf(*bottom, p.y);
    }
  }
}

static void TestTiltedCameraFraming(void) {
  const float pitches[] = {-0.7f, -0.2f, 0.0f, 0.2f, 0.7f};
  const float yaws[] = {-0.6f, 0.0f, 0.6f};
  const float distances[] = {3.25f, 5.0f, 20.0f};
  const float aspects[] = {256.0f / 224.0f, 496.0f / 224.0f};
  for (unsigned p = 0; p < sizeof(pitches) / sizeof(*pitches); p++)
    for (unsigned y = 0; y < sizeof(yaws) / sizeof(*yaws); y++)
      for (unsigned d = 0; d < sizeof(distances) / sizeof(*distances); d++)
        for (unsigned a = 0; a < sizeof(aspects) / sizeof(*aspects); a++)
          for (int shaped = 0; shaped < 2; shaped++) {
            DioramaProjection projection = Projection();
            projection.aspect_x = aspects[a];
            projection.height_scale = 256.0f / 224.0f;
            projection.output_width = a ? 1600 : 1200;
            projection.output_height = 900;
            projection.output_y = 37;
            projection.bg1_plane.z_world = shaped ? 0.12f : 0.0f;
            projection.bg1_plane.rake = shaped ? 0.15f : 0.0f;
            projection.bg1_plane.bow = shaped ? -0.25f : 0.0f;
            const Scene3DCamera camera = {
                pitches[p],
                yaws[y],
                distances[d],
                0.4f,
            };
            Scene3D_BuildViewProjection(&camera, projection.output_width, projection.output_height,
                                        projection.matrix);
            DioramaProjection before = projection;
            CHECK(Diorama_CenterCameraVertically(
                projection.matrix, projection.aspect_x, projection.height_scale,
                projection.bg1_plane.z_world, projection.bg1_plane.rake, projection.bg1_plane.bow,
                0.0f, 1.0f));
            float top, bottom;
            FocalBounds(&projection, 0, 1, &top, &bottom);
            CHECK(
                Near((top + bottom) * 0.5f, projection.output_y + projection.output_height * 0.5f));

            /* The layer and attached OBJ effects at different depths must move by
             * the same number of pixels without a scale or perspective change. */
            float delta = 0.0f;
            for (int i = 0; i < 3; i++) {
              before.object_planes[0].z_world = (float)i * 0.2f;
              projection.object_planes[0] = before.object_planes[0];
              ArRenderPointF old_point, new_point;
              float old_sx, old_sy, new_sx, new_sy;
              CHECK(Diorama_ProjectCapturedPoint(&before, 35, 23, 0, &old_point, &old_sx, &old_sy));
              CHECK(Diorama_ProjectCapturedPoint(&projection, 35, 23, 0, &new_point, &new_sx,
                                                 &new_sy));
              if (i == 0) delta = new_point.y - old_point.y;
              CHECK(Near(new_point.y - old_point.y, delta));
              CHECK(Near(new_point.x, old_point.x));
              CHECK(Near(new_sx, old_sx));
              CHECK(Near(new_sy, old_sy));
            }
            for (int i = 0; i < 16; i++)
              if (i % 4 != 1) CHECK(projection.matrix[i] == before.matrix[i]);
          }
}

static void TestCameraFramingCentersNativeBand(void) {
  for (int bottom_heavy = 0; bottom_heavy < 2; bottom_heavy++) {
    DioramaProjection projection = Projection();
    projection.height_scale = 288.0f / 224.0f;
    const float t0 = bottom_heavy ? 0.0f : 64.0f / 288.0f;
    const float t1 = bottom_heavy ? 224.0f / 288.0f : 1.0f;
    for (int tight = 0; tight < 2; tight++) {
      const Scene3DCamera camera = {0.0f, 0.0f, tight ? 2.0f : 2.6f, 0.4f};
      Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
      CHECK(Diorama_CenterCameraVertically(projection.matrix, projection.aspect_x,
                                           projection.height_scale, 0, 0, 0, t0, t1));
      float top, bottom;
      FocalBounds(&projection, t0, t1, &top, &bottom);
      CHECK(Near((top + bottom) * 0.5f, 50.0f));
      if (!tight) CHECK(top >= -0.001f && bottom <= 100.001f);
    }
  }
}

static DioramaProjection CaptureProjection(
    const Scene3DCamera *camera, int height, int top) {
  DioramaProjection projection = Projection();
  projection.aspect_x = 496.0f / 224.0f;
  projection.height_scale = height / 224.0f;
  projection.texture_height = height;
  projection.output_width = 1600;
  projection.output_height = 900;
  projection.bg2_plane.z_world = -.3f;
  projection.object_planes[0].z_world = .01f;
  Scene3D_BuildViewProjection(camera, 1600, 900, projection.matrix);
  CHECK(Diorama_AlignCaptureToNativeCamera(projection.matrix, height, top));
  return projection;
}

static void CenterCapture(DioramaProjection *projection, int top) {
  CHECK(Diorama_CenterCameraVertically(
      projection->matrix, projection->aspect_x, projection->height_scale,
      0, 0, 0, top / (float)projection->texture_height,
      (top + 224) / (float)projection->texture_height));
}

static void TestCaptureOriginKeepsEveryDepthRegistered(void) {
  const int heights[] = {224, 288, 288, 352, 352, 352, 352};
  const int tops[] = {0, 0, 64, 0, 32, 64, 128};
  const float tilts[] = {-.6f, 0, .6f};
  const float distances[] = {3.25f, 6};
  for (unsigned p = 0; p < sizeof(tilts) / sizeof(*tilts); p++)
    for (unsigned y = 0; y < sizeof(tilts) / sizeof(*tilts); y++)
      for (unsigned d = 0; d < sizeof(distances) / sizeof(*distances); d++)
        for (unsigned i = 0; i < sizeof(heights) / sizeof(*heights); i++)
          for (int framed = 0; framed < 2; framed++) {
            const Scene3DCamera camera = {tilts[p], tilts[y], distances[d], .4f};
            DioramaProjection reference = CaptureProjection(&camera, 224, 0);
            DioramaProjection expanded = CaptureProjection(&camera, heights[i], tops[i]);
            if (framed) {
              CenterCapture(&reference, 0);
              CenterCapture(&expanded, tops[i]);
            }
            for (int band = 0; band < 3; band++) {
              const float z = band == 0 ? 0 : band == 1 ? -.3f : .01f;
              reference.object_planes[0].z_world = z;
              expanded.object_planes[0].z_world = z;
              for (int row = 0; row <= 224; row += 16) {
                ArRenderPointF expected, actual;
                float expected_x, expected_y, actual_x, actual_y;
                CHECK(Diorama_ProjectCapturedPoint(
                    &reference, 35, row, 0, &expected, &expected_x, &expected_y));
                CHECK(Diorama_ProjectCapturedPoint(
                    &expanded, 35, row + tops[i], 0, &actual, &actual_x, &actual_y));
                CHECK(Near(actual.x, expected.x) && Near(actual.y, expected.y));
                CHECK(Near(actual_x, expected_x) && Near(actual_y, expected_y));
              }
            }
          }
}

static void TestScrollContinuesAcrossRedistributedMargins(void) {
  /* At 3.25 distance the old capture-centred camera paused through Y=28..64
   * and Y=223..258, then resumed. These spans still have sufficient world
   * above/below the displayed view, so reaching a buffer edge must not pin it. */
  const Scene3DCamera camera = {0, 0, 3.25f, .4f};
  const int starts[] = {48, 232};
  const int ends[] = {72, 240};
  for (int span = 0; span < 2; span++) {
    ArRenderPointF previous = {0};
    for (int y = starts[span]; y <= ends[span]; y++) {
      const int top = y < 64 ? y : y > 223 ? y - 159 : 64;
      DioramaProjection projection = CaptureProjection(&camera, 352, top);
      CenterCapture(&projection, top);
      DioramaVerticalBounds bounds = DioramaVerticalBounds_Resolve(
          SR_PPU_OVERLAY_BG1, y, 512, top, 352);
      CHECK(bounds.valid);
      CHECK(Diorama_ClampCameraVertically(
          projection.matrix, projection.aspect_x, projection.height_scale,
          0, 0, 0, bounds.top_reached, bounds.bottom_reached, NULL));
      DioramaProjection reference = CaptureProjection(&camera, 224, 0);
      CenterCapture(&reference, 0);
      ArRenderPointF actual, expected;
      CHECK(Diorama_ProjectCapturedBg1Point(&projection, 50, 255 - y + top,
                                            &actual, NULL, NULL));
      CHECK(Diorama_ProjectCapturedBg1Point(&reference, 50, 255 - y,
                                            &expected, NULL, NULL));
      CHECK(Near(actual.y, expected.y));
      if (y > starts[span]) CHECK(actual.y < previous.y - 3.0f);
      previous = actual;
      /* BG2 has half-rate native parallax; buffer changes must not make it
       * reverse direction while BG1 appears stalled. */
      CHECK(Diorama_ProjectCapturedBg2Point(&projection, 50, 199 - y / 2 + top,
                                            &actual, NULL, NULL));
      CHECK(Diorama_ProjectCapturedBg2Point(&reference, 50, 199 - y / 2,
                                            &expected, NULL, NULL));
      CHECK(Near(actual.y, expected.y));
    }
  }
}

static void TestCameraEasesIntoWorldEdge(void) {
  /* The user's 03/01 snapshots move from camera Y=504 to 520. Both retain
   * the same 352 world rows through top margins 89 and 105. Sweep that floor
   * handoff in both directions and mirror it at the ceiling. */
  const Scene3DCamera camera = {0, 0, 3.25f, .4f};
  for (int at_bottom = 0; at_bottom < 2; at_bottom++)
    for (int direction = -1; direction <= 1; direction += 2) {
      float previous = 0, previous_delta = 0;
      for (int step = 0; step <= 63; step++) {
        const int top = direction > 0 ? step : 63 - step;
        const int margin = at_bottom ? top + 65 : top;
        DioramaProjection projection = CaptureProjection(&camera, 352, margin);
        CenterCapture(&projection, margin);
        CHECK(Diorama_ClampCameraVertically(
            projection.matrix, projection.aspect_x, projection.height_scale,
            0, 0, 0, !at_bottom, at_bottom, NULL));
        ArRenderPointF point, edge;
        CHECK(Diorama_ProjectCapturedBg1Point(
            &projection, 50, 176, &point, NULL, NULL));
        CHECK(Diorama_ProjectCapturedBg1Point(
            &projection, 50, at_bottom ? 352 : 0, &edge, NULL, NULL));
        CHECK(at_bottom ? edge.y >= 899.999f : edge.y <= .001f);
        if (step > 0) {
          const float delta = point.y - previous;
          CHECK(direction * delta <= .001f && fabsf(delta) < 3.05f);
          if (step > 1) CHECK(fabsf(delta - previous_delta) < .25f);
          previous_delta = delta;
        }
        previous = point.y;
      }
    }
}

static void TestCaptureAlignmentRejectsInvalidInputs(void) {
  DioramaProjection projection = Projection();
  const DioramaProjection before = projection;
  CHECK(Diorama_AlignCaptureToNativeCamera(projection.matrix, 352, 64));
  CHECK(!memcmp(projection.matrix, before.matrix, sizeof(before.matrix)));
  CHECK(!Diorama_AlignCaptureToNativeCamera(NULL, 352, 64));
  CHECK(!Diorama_AlignCaptureToNativeCamera(projection.matrix, 223, 0));
  CHECK(!Diorama_AlignCaptureToNativeCamera(projection.matrix, 352, -1));
  CHECK(!Diorama_AlignCaptureToNativeCamera(projection.matrix, 352, 129));
  CHECK(!memcmp(projection.matrix, before.matrix, sizeof(before.matrix)));
  projection.matrix[0] = NAN;
  CHECK(!Diorama_AlignCaptureToNativeCamera(projection.matrix, 352, 128));
  CHECK(!memcmp(projection.matrix + 1, before.matrix + 1, 15 * sizeof(float)));
}

static void TestCameraFramingRejectsUnprojectableMesh(void) {
  DioramaProjection projection = Projection();
  const Scene3DCamera camera = {0.7f, 0.7f, 0.2f, 0.4f};
  Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
  const DioramaProjection before = projection;
  CHECK(!Diorama_CenterCameraVertically(projection.matrix, 2, 1, 0, 0, 0, 0, 1));
  CHECK(memcmp(projection.matrix, before.matrix, sizeof(before.matrix)) == 0);
  CHECK(!Diorama_CenterCameraVertically(NULL, 2, 1, 0, 0, 0, 0, 1));
}

static void TestCapturedWorldVerticalBounds(void) {
  /* The user's two Kassandora 03/03 snapshots have identical 512px maps.
   * gf1334 captures 64 rows on both sides; gf1841 reaches the floor. */
  DioramaVerticalBounds bounds = DioramaVerticalBounds_Resolve(
      SR_PPU_OVERLAY_BG1, 220, 512, 64, 352);
  CHECK(bounds.valid && bounds.plane == SR_PPU_OVERLAY_BG1);
  CHECK(!bounds.top_reached && !bounds.bottom_reached);
  bounds = DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 287, 512, 64, 288);
  CHECK(bounds.valid && !bounds.top_reached && bounds.bottom_reached);
  /* Rebalancing capture rows preserves the floor without shortening the top. */
  bounds = DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 287, 512, 128, 352);
  CHECK(bounds.valid && !bounds.top_reached && bounds.bottom_reached);
  bounds = DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 280, 512, 121, 352);
  CHECK(bounds.valid && !bounds.top_reached && bounds.bottom_reached);
  /* A parallax background's camera has not reached that same floor. */
  bounds = DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG2, 143, 512, 64, 288);
  CHECK(bounds.valid && !bounds.top_reached && !bounds.bottom_reached);
  bounds = DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 0, 512, 0, 288);
  CHECK(bounds.valid && bounds.top_reached && !bounds.bottom_reached);
  bounds = DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 0, 225, 0, 224);
  CHECK(bounds.valid && bounds.top_reached && bounds.bottom_reached);
  CHECK(!DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 287, 512, 64, 352).valid);
  CHECK(!DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 288, 512, 64, 288).valid);
  CHECK(!DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 0, 512, 64, 288).valid);
  CHECK(!DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_BG1, 0, 0, 0, 224).valid);
  CHECK(!DioramaVerticalBounds_Resolve(SR_PPU_OVERLAY_OBJ, 0, 512, 0, 224).valid);
}

static void TestFloorCaptureCoversUpperViewport(void) {
  /* The old floor capture (64 + 224 rows) ended 52px below the viewport's
   * top at the reported 3.25 distance. Reusing the lower budget above keeps
   * actual world artwork available through the seven-pixel ledge transition. */
  const int top_margins[] = {105, 121, 128, 121};
  for (size_t i = 0; i < sizeof(top_margins) / sizeof(top_margins[0]); i++) {
    DioramaProjection projection = Projection();
    projection.aspect_x = 496.0f / 224.0f;
    projection.height_scale = 352.0f / 224.0f;
    const Scene3DCamera camera = {0, 0, 3.25f, .4f};
    Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
    CHECK(Diorama_AlignCaptureToNativeCamera(projection.matrix, 352, top_margins[i]));
    CHECK(Diorama_CenterCameraVertically(
        projection.matrix, projection.aspect_x, projection.height_scale,
        0, 0, 0, top_margins[i] / 352.0f, (top_margins[i] + 224) / 352.0f));
    CHECK(Diorama_ClampCameraVertically(
        projection.matrix, projection.aspect_x, projection.height_scale,
        0, 0, 0, false, true, NULL));
    float top, bottom;
    FocalBounds(&projection, 0, 1, &top, &bottom);
    CHECK(top <= .001f);
    CHECK(bottom >= 99.999f);
  }
}

static void TestCameraClampsReachedWorldEdge(void) {
  const float pitches[] = {-.15f, 0.0f, .15f};
  const float yaws[] = {-.2f, 0.0f, .2f};
  for (int at_bottom = 0; at_bottom < 2; at_bottom++)
    for (unsigned p = 0; p < sizeof(pitches) / sizeof(*pitches); p++)
      for (unsigned y = 0; y < sizeof(yaws) / sizeof(*yaws); y++)
        for (int shaped = 0; shaped < 2; shaped++) {
          DioramaProjection projection = Projection();
          projection.aspect_x = 496.0f / 224.0f;
          projection.height_scale = 288.0f / 224.0f;
          projection.bg1_plane.rake = shaped ? .1f : 0;
          projection.bg1_plane.bow = shaped ? -.15f : 0;
          const Scene3DCamera camera = {pitches[p], yaws[y], 3.25f, .4f};
          Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
          CHECK(Diorama_CenterCameraVertically(
              projection.matrix, projection.aspect_x, projection.height_scale,
              0, projection.bg1_plane.rake, projection.bg1_plane.bow,
              at_bottom ? 64.0f / 288.0f : 0,
              at_bottom ? 1 : 224.0f / 288.0f));
          const DioramaProjection before = projection;
          float world_offset = 0;
          CHECK(Diorama_ClampCameraVertically(
              projection.matrix, projection.aspect_x, projection.height_scale,
              0, projection.bg1_plane.rake, projection.bg1_plane.bow,
              !at_bottom, at_bottom, &world_offset));
          /* Clamp the whole tilted edge, not only its midpoint. */
          for (int side = 0; side <= 8; side++) {
            ArRenderPointF point;
            CHECK(Diorama_ProjectCapturedBg1Point(
                &projection, side * 100.0f / 8, at_bottom ? 50 : 0,
                &point, NULL, NULL));
            CHECK(at_bottom ? point.y >= 99.999f : point.y <= .001f);
          }
          for (int depth = 0; depth < 3; depth++) {
            DioramaProjection old_actor = before;
            old_actor.object_planes[0].z_world = depth * .2f;
            projection.object_planes[0] = old_actor.object_planes[0];
            ArRenderPointF old_point, new_point;
            float old_sx, old_sy, new_sx, new_sy;
            /* A clamped camera must agree with a native camera at its virtual
             * world position, including the perspective at every OBJ depth. */
            CHECK(Diorama_ProjectCapturedPoint(
                &old_actor, 35, 23 - world_offset * 50 / projection.height_scale,
                0, &old_point, &old_sx, &old_sy));
            CHECK(Diorama_ProjectCapturedPoint(
                &projection, 35, 23, 0, &new_point, &new_sx, &new_sy));
            CHECK(Near(new_point.y, old_point.y));
            CHECK(Near(new_point.x, old_point.x));
            CHECK(Near(new_sx, old_sx) && Near(new_sy, old_sy));
          }
          for (int i = 0; i < 12; i++)
            CHECK(projection.matrix[i] == before.matrix[i]);
        }
}

static void TestParallaxSharesTheCameraStop(void) {
  const float pitches[] = {-.15f, 0, .15f};
  for (unsigned pose = 0; pose < sizeof(pitches) / sizeof(*pitches); pose++) {
    const Scene3DCamera camera = {pitches[pose], .1f, 3.25f, .4f};
    ArRenderPointF previous_bg = {0}, previous_actor = {0};
    for (int y = 480; y <= 543; y++) {
      const int top = y - 415;
      DioramaProjection projection = CaptureProjection(&camera, 352, top);
      CenterCapture(&projection, top);
      float shift;
      CHECK(Diorama_ClampCameraVertically(
          projection.matrix, projection.aspect_x, projection.height_scale,
          0, 0, 0, false, true, &shift));
      projection.bg2_plane.world_y_offset =
          Diorama_BackgroundClampOffset(y, y / 3, 0x13, 512, shift);
      ArRenderPointF background, actor;
      CHECK(Diorama_ProjectCapturedBg2Point(
          &projection, 50, 299 - y / 3 + top, &background, NULL, NULL));
      CHECK(Diorama_ProjectCapturedPoint(
          &projection, 50, 650 - y + top, 0, &actor, NULL, NULL));
      if (y > 480) CHECK(background.y <= previous_bg.y + .001f);
      /* Past the eased stop, static scenery and objects at all depths stop
       * together. The old screen-space clamp reversed BG2's 1/3-rate scroll. */
      if (y > 530) {
        CHECK(Near(background.x, previous_bg.x));
        CHECK(Near(background.y, previous_bg.y));
        CHECK(Near(actor.x, previous_actor.x));
        CHECK(Near(actor.y, previous_actor.y));
      }
      previous_bg = background;
      previous_actor = actor;
    }
  }
  /* A stationary backdrop and a background that reached its own native
   * floor must remain stationary while the foreground camera moves. */
  CHECK(Near(Diorama_BackgroundClampOffset(100, 0, 0, 512, -.1f), .1f));
  CHECK(Near(Diorama_BackgroundClampOffset(100, 10, 0, 512, -.1f), .1f));
  CHECK(Near(Diorama_BackgroundClampOffset(600, 544, 0x11, 768, -10.0f / 224),
             10.0f / 224));
  CHECK(Near(Diorama_BackgroundClampOffset(1017, 1017, 0x11, 512, 16.0f / 224), 0));
}

static void TestCameraClampPreservesInteriorAndSmallRoom(void) {
  DioramaProjection projection = Projection();
  const Scene3DCamera camera = {0, 0, 6, .4f};
  Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
  const DioramaProjection before = projection;
  CHECK(Diorama_ClampCameraVertically(projection.matrix, 2, 1, 0, 0, 0, false, false, NULL));
  CHECK(!memcmp(projection.matrix, before.matrix, sizeof(before.matrix)));
  CHECK(Diorama_ClampCameraVertically(projection.matrix, 2, 1, 0, 0, 0, true, true, NULL));
  CHECK(!memcmp(projection.matrix, before.matrix, sizeof(before.matrix)));
  CHECK(!Diorama_ClampCameraVertically(NULL, 2, 1, 0, 0, 0, false, true, NULL));
  CHECK(!Diorama_ClampCameraVertically(projection.matrix, 2, 1, NAN, 0, 0, false, true, NULL));
  CHECK(!memcmp(projection.matrix, before.matrix, sizeof(before.matrix)));
  const Scene3DCamera unprojectable = {.7f, .7f, .2f, .4f};
  Scene3D_BuildViewProjection(&unprojectable, 100, 100, projection.matrix);
  float matrix[16];
  memcpy(matrix, projection.matrix, sizeof(matrix));
  CHECK(!Diorama_ClampCameraVertically(projection.matrix, 2, 1, 0, 0, 0, true, true, NULL));
  CHECK(!memcmp(projection.matrix, matrix, sizeof(matrix)));
}

static void TestRegisteredProjectionAndScale(void) {
  DioramaProjection projection = Projection();
  ArRenderPointF point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(&projection, 50.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 50.0f));
  CHECK(Near(point.y, 50.0f));
  CHECK(Near(scale_x, 1.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestPriorityPlaneShapeIsApplied(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f; /* make depth visible in screen X */
  projection.object_planes[0].rake = 0.5f;
  projection.object_planes[1] = projection.object_planes[0];
  projection.object_planes[1].rake = 0.0f;
  ArRenderPointF raked, flat;
  CHECK(Diorama_ProjectCapturedPoint(&projection, 50.0f, 50.0f, 0, &raked, NULL, NULL));
  CHECK(Diorama_ProjectCapturedPoint(&projection, 50.0f, 50.0f, 1, &flat, NULL, NULL));
  CHECK(Near(raked.x - flat.x, 25.0f));
  CHECK(Near(raked.y, flat.y));

  projection.object_planes[0].rake = 0.0f;
  projection.object_planes[0].bow = 0.25f;
  CHECK(Diorama_ProjectCapturedPoint(&projection, 50.0f, 50.0f, 0, &raked, NULL, NULL));
  CHECK(Near(raked.x - flat.x, 12.5f));
}

static void TestOutputViewportOriginIsApplied(void) {
  DioramaProjection projection = Projection();
  projection.output_x = 120;
  projection.output_y = 40;
  ArRenderPointF point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(&projection, 50.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 170.0f));
  CHECK(Near(point.y, 90.0f));
  CHECK(Near(scale_x, 1.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestCapturedTextureOriginIsApplied(void) {
  DioramaProjection projection = Projection();
  /* A 20-column resolve apron precedes the displayed [20,80] texture span.
   * Display-capture x=30 is therefore texture column 50, the plane midpoint.
   * This is the contract action effects need when the diorama layer surfaces
   * are wider than the region they display. */
  projection.texture_x_origin = 20;
  projection.object_planes[0].u0 = 0.20f;
  projection.object_planes[0].u1 = 0.80f;
  ArRenderPointF point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(&projection, 30.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 50.0f));
  CHECK(Near(point.y, 50.0f));
  CHECK(Near(scale_x, 100.0f / 60.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestBg1PlaneShapeIsIndependent(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f; /* make source-plane depth visible in X */
  projection.bg1_plane.z_world = 0.40f;
  projection.bg1_plane.rake = 0.20f;
  projection.object_planes[0].z_world = 0.0f;
  ArRenderPointF wall, object;
  CHECK(Diorama_ProjectCapturedBg1Point(&projection, 50.0f, 25.0f, &wall, NULL, NULL));
  CHECK(Diorama_ProjectCapturedPoint(&projection, 50.0f, 25.0f, 0, &object, NULL, NULL));
  /* z 0.40 plus rake 0.20 at texture midpoint t=0.5 => depth 0.50;
   * identity projection maps that to 25 output pixels. */
  CHECK(Near(wall.x - object.x, 25.0f));
  CHECK(Near(wall.y, object.y));

  projection.bg1_plane.valid = false;
  CHECK(!Diorama_ProjectCapturedBg1Point(&projection, 50.0f, 25.0f, &wall, NULL, NULL));
}

static void TestBg2PlaneShapeAndWindowAreIndependent(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f;
  projection.bg2_plane.z_world = -0.30f;
  projection.bg2_plane.rake = 0.10f;
  projection.bg2_plane.u0 = 0.20f;
  projection.bg2_plane.u1 = 0.80f;
  ArRenderPointF backdrop, playfield;
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 50.0f, 25.0f, &backdrop, NULL, NULL));
  CHECK(Diorama_ProjectCapturedBg1Point(&projection, 50.0f, 25.0f, &playfield, NULL, NULL));
  CHECK(Near(backdrop.x - playfield.x, -12.5f));
  CHECK(Near(backdrop.y, playfield.y));
  projection.bg2_plane.valid = false;
  CHECK(!Diorama_ProjectCapturedBg2Point(&projection, 50.0f, 25.0f, &backdrop, NULL, NULL));
}

static void TestBg2FoldedOverflowProjection(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f; /* make folded Z visible in screen X */
  projection.bg2_plane.z_world = -0.30f;
  projection.bg2_plane.overflow_valid = true;
  projection.bg2_plane.overflow_fold_t = 0.50f;
  projection.bg2_plane.overflow_height = 1.0f;
  projection.bg2_plane.overflow_overlap_t = 0.25f;
  projection.bg2_plane.overflow_handoff_z = -0.30f;
  projection.bg2_plane.overflow_front_z = 0.45f;
  projection.bg2_plane.overflow_front_drop = 0.18f;

  /* capture y=56.25 maps to plane t=1.125, hence overflow t=0.625.
   * This is the same halfway-bend point pinned by the pure geometry test:
   * world y=-0.4825 and z=0.075 for this y_top=0 variant. */
  ArRenderPointF folded;
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 50.0f, 56.25f, &folded, NULL, NULL));
  CHECK(Near(folded.x, 53.75f));
  CHECK(Near(folded.y, 74.125f));

  /* Disabling the continuation returns to ordinary flat extrapolation. This
   * guards the production seam used by the waterfall veil and mist, not only
   * the mesh arithmetic. */
  projection.bg2_plane.overflow_valid = false;
  ArRenderPointF flat;
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 50.0f, 56.25f, &flat, NULL, NULL));
  CHECK(!Near(folded.x, flat.x));
  CHECK(!Near(folded.y, flat.y));
}

static void TestInvalidInputsFailClosed(void) {
  DioramaProjection projection = Projection();
  ArRenderPointF point = {17.0f, 29.0f};
  CHECK(!Diorama_ProjectCapturedPoint(&projection, 0.0f, 0.0f, 1, &point, NULL, NULL));
  CHECK(point.x == 17.0f && point.y == 29.0f);
  CHECK(!Diorama_ProjectCapturedPoint(&projection, 0.0f, 0.0f, 4, &point, NULL, NULL));
  projection.bg1_plane.valid = false;
  CHECK(!Diorama_ProjectCapturedBg1Point(&projection, 0.0f, 0.0f, &point, NULL, NULL));
  projection.valid = false;
  CHECK(!Diorama_ProjectCapturedPoint(&projection, 0.0f, 0.0f, 0, &point, NULL, NULL));
  CHECK(!Diorama_ProjectCapturedPoint(NULL, 0.0f, 0.0f, 0, &point, NULL, NULL));
  CHECK(!Diorama_ProjectCapturedPoint(&projection, 0.0f, 0.0f, 0, NULL, NULL, NULL));
}

static void TestPlaneEligibilityMatchesDrawableInputs(void) {
  CHECK(Diorama_PlaneEligible(SR_PPU_OVERLAY_BG2, true, true, true, false, false, 0));
  CHECK(!Diorama_PlaneEligible(SR_PPU_OVERLAY_BG2, false, true, true, false, false, 0));
  CHECK(!Diorama_PlaneEligible(SR_PPU_OVERLAY_BG2, true, false, true, false, false, 0));
  CHECK(!Diorama_PlaneEligible(SR_PPU_OVERLAY_BG2, true, true, false, false, false, 0));
  CHECK(!Diorama_PlaneEligible(SR_PPU_OVERLAY_BG2, true, true, true, false, true, 0));
  CHECK(!Diorama_PlaneEligible(kDioramaPlane_Bg2Far, true, true, true, false, true, 0));
  CHECK(!Diorama_PlaneEligible(SR_PPU_OVERLAY_BG3, true, true, true, true, false, 0));

  /* A current attached effect supplies current projection content for its
   * exact BG or OBJ plane. It needs no source texture when that isolated
   * hardware band is empty, but cannot bypass visibility or skybox policy. */
  CHECK(Diorama_PlaneProjectable(SR_PPU_OVERLAY_OBJ, true, true, false, true, false, false, 0));
  CHECK(!Diorama_PlaneProjectable(SR_PPU_OVERLAY_OBJ, false, true, false, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(SR_PPU_OVERLAY_OBJ, true, false, false, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(SR_PPU_OVERLAY_BG2, true, true, false, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(SR_PPU_OVERLAY_BG1, true, false, false, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(kDioramaPlane_Bg1Hi, true, false, false, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(SR_PPU_OVERLAY_BG2, true, true, true, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(kDioramaPlane_Bg2Hi, true, true, false, true, false, false, 0));
  CHECK(Diorama_PlaneProjectable(kDioramaPlane_Bg2Hi, true, true, false, true, false, true, 0));
  CHECK(Diorama_PlaneEligible(kDioramaPlane_Bg2Hi, true, true, true, false, true, 0));
  CHECK(!Diorama_PlaneProjectable(kDioramaPlane_Bg2Hi, false, true, true, true, false, true, 0));
  CHECK(!Diorama_PlaneProjectable(SR_PPU_OVERLAY_BG2, true, true, false, true, false, true, 0));
  CHECK(!Diorama_PlaneProjectable(SR_PPU_OVERLAY_OBJ, true, true, false, false, false, false, 0));
}

static void TestSkyboxKeepsAdditiveSceneColorBase(void) {
  /* Marahna captures BG1 and OBJ as subscreen winners. BG2 is the main
   * color input, including animated water, rather than replacement sky art. */
  const uint32_t subscreen = (1u << SR_PPU_OVERLAY_BG1) |
      (1u << kDioramaPlane_Bg1Hi) | (1u << SR_PPU_OVERLAY_OBJ);
  const int bases[] = {SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Far};
  for (unsigned i = 0; i < sizeof(bases) / sizeof(*bases); i++) {
    const int plane = bases[i];
    CHECK(Diorama_PlaneEligible(plane, true, true, true, false, true, subscreen));
    CHECK(Diorama_PlaneProjectable(plane, true, true, true, false, false, true, subscreen));
    CHECK(!Diorama_PlaneEligible(plane, false, true, true, false, true, subscreen));
    CHECK(!Diorama_PlaneEligible(plane, true, false, true, false, true, subscreen));
    CHECK(!Diorama_PlaneEligible(plane, true, true, false, false, true, subscreen));
    /* Normal and BG2-on-subscreen scenes keep their existing skybox policy. */
    CHECK(!Diorama_PlaneEligible(plane, true, true, true, false, true, 0));
    CHECK(!Diorama_PlaneEligible(plane, true, true, true, false, true,
                                 1u << SR_PPU_OVERLAY_BG2));
    /* Upload masks may contain only a split band when the base is empty. */
    CHECK(!Diorama_PlaneEligible(plane, true, true, true, false, true,
                                 1u << kDioramaPlane_Bg2Hi));
    CHECK(!Diorama_PlaneEligible(plane, true, true, true, false, true,
                                 1u << kDioramaPlane_Bg2Far));
  }
  /* Effects retain the color plane's own projection, not the stretched sky. */
  CHECK(Diorama_PlaneProjectable(SR_PPU_OVERLAY_BG2, true, false, false,
                                  true, false, true, subscreen));
  CHECK(!Diorama_PlaneEligible(kDioramaPlane_Backdrop, true, true, true,
                                false, true, subscreen));
  CHECK(!Diorama_PlaneEligible(SR_PPU_OVERLAY_BG3, true, true, true,
                                true, true, subscreen));
}

static void TestObjEffectMaskDistinguishesEmptyFromFailedUpload(void) {
  const uint32_t obj0 = 1u << SR_PPU_OVERLAY_OBJ;
  const uint32_t obj2 = 1u << kDioramaPlane_Obj2;
  const uint8_t required = (1u << 0) | (1u << 2);

  /* Empty OBJ0 needs its actor transform without a texture upload; OBJ2 had
   * pixels and uploaded, so both exact priorities remain available. */
  CHECK(Diorama_FilterObjEffectProjectionMask(required, obj0 | obj2, obj2, obj2) == required);
  /* Content without a successful upload is a real resource failure, not an
   * empty-band actor case, and therefore fails closed. */
  CHECK(Diorama_FilterObjEffectProjectionMask(required, obj0 | obj2, obj0 | obj2, obj2) ==
        (1u << 2));
  CHECK(Diorama_FilterObjEffectProjectionMask(required, obj2, 0, 0) == (1u << 2));
  CHECK(Diorama_FilterObjEffectProjectionMask(0xFFu, 0, 0, 0) == 0);
}

static void TestBgEffectMaskDistinguishesEmptyFromFailedUpload(void) {
  const uint32_t bg1 = 1u << SR_PPU_OVERLAY_BG1;
  const uint32_t bg2 = 1u << SR_PPU_OVERLAY_BG2;
  const uint32_t bg1hi = 1u << kDioramaPlane_Bg1Hi;
  const uint32_t bg2hi = 1u << kDioramaPlane_Bg2Hi;
  const uint32_t required = bg1 | bg2 | bg1hi | bg2hi;

  CHECK(Diorama_FilterBgEffectProjectionMask(required, bg1 | bg2 | bg1hi | bg2hi, bg2, bg2) ==
        (bg1 | bg2 | bg1hi | bg2hi));
  CHECK(Diorama_FilterBgEffectProjectionMask(required, bg1 | bg2, bg1 | bg2, bg2) == bg2);
  CHECK(Diorama_FilterBgEffectProjectionMask(required, bg2, 0, 0) == bg2);
  CHECK(Diorama_FilterBgEffectProjectionMask(required, 0, 0, 0) == 0);
  CHECK(Diorama_FilterBgEffectProjectionMask(bg2hi, bg2hi, bg2hi, 0) == 0);
  CHECK(Diorama_FilterBgEffectProjectionMask(bg2hi, bg2hi, bg2hi, bg2hi) == bg2hi);
}

static void TestGeneratedPlaneOffset(void) {
  DioramaProjection p = Projection();
  ArRenderPointF expected, actual, other;
  CHECK(Diorama_ProjectCapturedBg1Point(&p,40.25f,20.5f,&expected,NULL,NULL));
  CHECK(Diorama_ProjectCapturedBg2Point(&p,40,20,&other,NULL,NULL));
  p.bg1_plane.capture_offset = (ArRenderPointF){.25f,.5f};
  CHECK(Diorama_ProjectCapturedBg1Point(&p,40,20,&actual,NULL,NULL));
  CHECK(Near(expected.x,actual.x) && Near(expected.y,actual.y));
  CHECK(Diorama_ProjectCapturedBg2Point(&p,40,20,&actual,NULL,NULL));
  CHECK(Near(other.x,actual.x) && Near(other.y,actual.y));
}

static void TestSkyboxProjection(void) {
  DioramaProjection p = Projection();
  p.bg2_plane.valid = false;
  p.output_x = 17; p.output_y = 29;
  p.output_width = 800; p.output_height = 400;
  p.bg2_skybox = (DioramaSkyboxProjection){
    .count = 2, .active_band = -1,
    .bands = {{102,64,354,192,0,.5f}, {2,192,454,320,.5f,1}},
  };
  ArRenderPointF point;
  float sx,sy,x0,y0,x1,y1;
  CHECK(Diorama_ProjectCapturedBg2Point(&p,228,128,&point,&sx,&sy));
  CHECK(Near(point.x,417) && Near(point.y,129));
  CHECK(Near(sx,800.0f/252) && Near(sy,200.0f/128));
  CHECK(Diorama_ProjectCapturedBg2Point(&p,2,192.01f,&point,NULL,NULL));
  CHECK(Near(point.x,17) && Near(point.y,229.015625f));
  /* A tilted gameplay camera does not move this viewport-filling sky. */
  p.matrix[0] = .3f; p.matrix[12] = .2f;
  CHECK(Diorama_ProjectCapturedBg2Point(&p,228,128,&point,NULL,NULL));
  CHECK(Near(point.x,417) && Near(point.y,129));
  CHECK(Diorama_SkyboxCaptureBounds(&p,&x0,&y0,&x1,&y1));
  CHECK(x0 == 2 && y0 == 64 && x1 == 454 && y1 == 320);
  p.bg2_skybox.active_band = 0;
  CHECK(Diorama_SkyboxCaptureBounds(&p,&x0,&y0,&x1,&y1));
  CHECK(x0 == 102 && y0 == 64 && x1 == 354 && y1 == 192);
  /* Boundary vertices stay in the selected band's mapping. */
  CHECK(Diorama_ProjectCapturedBg2Point(&p,102,192,&point,NULL,NULL));
  CHECK(Near(point.x,17) && Near(point.y,229));
  p.bg2_skybox.active_band = 1;
  CHECK(Diorama_ProjectCapturedBg2Point(&p,2,192,&point,NULL,NULL));
  CHECK(Near(point.x,17) && Near(point.y,229));
  CHECK(Diorama_ProjectSkyboxAnchorPoint(&p,128,102,256,&point));
  CHECK(Near(point.x,17) && Near(point.y,329));
  CHECK(Diorama_SkyboxAnchorBounds(&p,128,&x0,&y0,&x1,&y1));
  CHECK(x0 == 102 && y0 == 192 && x1 == 354 && y1 == 320);
  p.bg2_skybox.active_band = -1;
  CHECK(Diorama_SkyboxAnchorBounds(&p,128,&x0,&y0,&x1,&y1));
  CHECK(x0 == 102 && y0 == 64 && x1 == 354 && y1 == 320);
  CHECK(!Diorama_ProjectSkyboxAnchorPoint(&p,NAN,102,256,&point));
  CHECK(!Diorama_SkyboxAnchorBounds(&p,NAN,&x0,&y0,&x1,&y1));
  p.bg2_skybox.active_band = 2;
  CHECK(!Diorama_ProjectCapturedBg2Point(&p,228,128,&point,NULL,NULL));
  CHECK(!Diorama_SkyboxCaptureBounds(&p,&x0,&y0,&x1,&y1));
  CHECK(!Diorama_ProjectSkyboxAnchorPoint(&p,128,102,256,&point));
  CHECK(!Diorama_SkyboxAnchorBounds(&p,128,&x0,&y0,&x1,&y1));
  p.bg2_skybox.active_band = -1;
  p.bg2_skybox.bands[0].x1 = NAN;
  CHECK(!Diorama_ProjectCapturedBg2Point(&p,228,128,&point,NULL,NULL));
  CHECK(!Diorama_ProjectSkyboxAnchorPoint(&p,128,102,256,&point));
  CHECK(!Diorama_SkyboxAnchorBounds(&p,128,&x0,&y0,&x1,&y1));
  p.bg2_skybox.count = 0;
  CHECK(!Diorama_ProjectCapturedBg2Point(&p,228,128,&point,NULL,NULL));
}

int main(void) {
  TestSkyboxProjection();
  TestGeneratedPlaneOffset();
  TestTiltedCameraFraming();
  TestCameraFramingCentersNativeBand();
  TestCaptureOriginKeepsEveryDepthRegistered();
  TestScrollContinuesAcrossRedistributedMargins();
  TestCameraEasesIntoWorldEdge();
  TestCaptureAlignmentRejectsInvalidInputs();
  TestCameraFramingRejectsUnprojectableMesh();
  TestCapturedWorldVerticalBounds();
  TestFloorCaptureCoversUpperViewport();
  TestCameraClampsReachedWorldEdge();
  TestParallaxSharesTheCameraStop();
  TestCameraClampPreservesInteriorAndSmallRoom();
  TestRegisteredProjectionAndScale();
  TestPriorityPlaneShapeIsApplied();
  TestOutputViewportOriginIsApplied();
  TestCapturedTextureOriginIsApplied();
  TestBg1PlaneShapeIsIndependent();
  TestBg2PlaneShapeAndWindowAreIndependent();
  TestBg2FoldedOverflowProjection();
  TestInvalidInputsFailClosed();
  TestPlaneEligibilityMatchesDrawableInputs();
  TestSkyboxKeepsAdditiveSceneColorBase();
  TestObjEffectMaskDistinguishesEmptyFromFailedUpload();
  TestBgEffectMaskDistinguishesEmptyFromFailedUpload();
  if (s_failures) {
    fprintf(stderr, "%d diorama projection test(s) failed\n", s_failures);
    return 1;
  }
  puts("diorama projection: all tests passed");
  return 0;
}
