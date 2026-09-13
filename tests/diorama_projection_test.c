#include <math.h>
#include <stdio.h>
#include <string.h>

#include "diorama.h"
#include "scene3d_math.h"

static int g_failures;

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
            #condition);                                                     \
    g_failures++;                                                            \
  }                                                                          \
} while (0)

static bool Near(float actual, float expected) {
  return fabsf(actual - expected) < 0.001f;
}

static DioramaProjection Projection(void) {
  DioramaProjection projection = {
    .valid = true,
    .matrix = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
    },
    .aspect_x = 2.0f,
    .height_scale = 1.0f,
    .texture_width = 100,
    .texture_height = 50,
    .output_width = 100,
    .output_height = 100,
  };
  projection.bg1_plane = (DioramaPlaneProjection){
    .valid = true, .u1 = 1.0f, .v1 = 1.0f,
  };
  projection.bg2_plane = projection.bg1_plane;
  projection.object_planes[0] = projection.bg1_plane;
  return projection;
}

static void FocalBounds(const DioramaProjection *projection,
                         float t0, float t1, float *top, float *bottom) {
  *top = INFINITY;
  *bottom = -INFINITY;
  for (int row = 0; row <= kDioramaPlaneSubdivY; row++) {
    const float t = t0 + (t1 - t0) * (float)row / kDioramaPlaneSubdivY;
    for (int col = 0; col <= 8; col++) {
      ArRenderPointF p;
      CHECK(Diorama_ProjectCapturedBg1Point(
          projection, (float)col * 100.0f / 8.0f, t * 50.0f,
          &p, NULL, NULL));
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
      pitches[p], yaws[y], distances[d], 0.4f,
    };
    Scene3D_BuildViewProjection(&camera, projection.output_width,
                               projection.output_height, projection.matrix);
    DioramaProjection before = projection;
    CHECK(Diorama_CenterCameraVertically(
        projection.matrix, projection.aspect_x, projection.height_scale,
        projection.bg1_plane.z_world, projection.bg1_plane.rake,
        projection.bg1_plane.bow, 0.0f, 1.0f));
    float top, bottom;
    FocalBounds(&projection, 0, 1, &top, &bottom);
    CHECK(Near((top + bottom) * 0.5f,
               projection.output_y + projection.output_height * 0.5f));

    /* The layer and attached OBJ effects at different depths must move by
     * the same number of pixels without a scale or perspective change. */
    float delta = 0.0f;
    for (int i = 0; i < 3; i++) {
      before.object_planes[0].z_world = (float)i * 0.2f;
      projection.object_planes[0] = before.object_planes[0];
      ArRenderPointF old_point, new_point;
      float old_sx, old_sy, new_sx, new_sy;
      CHECK(Diorama_ProjectCapturedPoint(
          &before, 35, 23, 0, &old_point, &old_sx, &old_sy));
      CHECK(Diorama_ProjectCapturedPoint(
          &projection, 35, 23, 0, &new_point, &new_sx, &new_sy));
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

static void TestCameraFramingProtectsNativeBand(void) {
  for (int bottom_heavy = 0; bottom_heavy < 2; bottom_heavy++) {
    DioramaProjection projection = Projection();
    projection.height_scale = 288.0f / 224.0f;
    const float t0 = bottom_heavy ? 0.0f : 64.0f / 288.0f;
    const float t1 = bottom_heavy ? 224.0f / 288.0f : 1.0f;
    for (int tight = 0; tight < 2; tight++) {
      const Scene3DCamera camera = {0.0f, 0.0f, tight ? 2.0f : 2.6f, 0.4f};
      Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
      CHECK(Diorama_CenterCameraVertically(
          projection.matrix, projection.aspect_x, projection.height_scale,
          0, 0, 0, t0, t1));
      float top, bottom;
      FocalBounds(&projection, t0, t1, &top, &bottom);
      if (tight) {
        CHECK(Near((top + bottom) * 0.5f, 50.0f));
      } else {
        CHECK(top >= -0.001f && bottom <= 100.001f);
        CHECK(bottom_heavy ? Near(top, 0.0f) : Near(bottom, 100.0f));
      }
    }
  }
}

static void TestCameraFramingRejectsUnprojectableMesh(void) {
  DioramaProjection projection = Projection();
  const Scene3DCamera camera = {0.7f, 0.7f, 0.2f, 0.4f};
  Scene3D_BuildViewProjection(&camera, 100, 100, projection.matrix);
  const DioramaProjection before = projection;
  CHECK(!Diorama_CenterCameraVertically(
      projection.matrix, 2, 1, 0, 0, 0, 0, 1));
  CHECK(memcmp(projection.matrix, before.matrix, sizeof(before.matrix)) == 0);
  CHECK(!Diorama_CenterCameraVertically(NULL, 2, 1, 0, 0, 0, 0, 1));
}

static void TestRegisteredProjectionAndScale(void) {
  DioramaProjection projection = Projection();
  ArRenderPointF point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 50.0f));
  CHECK(Near(point.y, 50.0f));
  CHECK(Near(scale_x, 1.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestPriorityPlaneShapeIsApplied(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f;  /* make depth visible in screen X */
  projection.object_planes[0].rake = 0.5f;
  projection.object_planes[1] = projection.object_planes[0];
  projection.object_planes[1].rake = 0.0f;
  ArRenderPointF raked, flat;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 50.0f, 0, &raked, NULL, NULL));
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 50.0f, 1, &flat, NULL, NULL));
  CHECK(Near(raked.x - flat.x, 25.0f));
  CHECK(Near(raked.y, flat.y));

  projection.object_planes[0].rake = 0.0f;
  projection.object_planes[0].bow = 0.25f;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 50.0f, 0, &raked, NULL, NULL));
  CHECK(Near(raked.x - flat.x, 12.5f));
}

static void TestOutputViewportOriginIsApplied(void) {
  DioramaProjection projection = Projection();
  projection.output_x = 120;
  projection.output_y = 40;
  ArRenderPointF point;
  float scale_x = 0.0f, scale_y = 0.0f;
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 25.0f, 0, &point, &scale_x, &scale_y));
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
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 30.0f, 25.0f, 0, &point, &scale_x, &scale_y));
  CHECK(Near(point.x, 50.0f));
  CHECK(Near(point.y, 50.0f));
  CHECK(Near(scale_x, 100.0f / 60.0f));
  CHECK(Near(scale_y, 1.0f));
}

static void TestBg1PlaneShapeIsIndependent(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f;  /* make source-plane depth visible in X */
  projection.bg1_plane.z_world = 0.40f;
  projection.bg1_plane.rake = 0.20f;
  projection.object_planes[0].z_world = 0.0f;
  ArRenderPointF wall, object;
  CHECK(Diorama_ProjectCapturedBg1Point(
      &projection, 50.0f, 25.0f, &wall, NULL, NULL));
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 50.0f, 25.0f, 0, &object, NULL, NULL));
  /* z 0.40 plus rake 0.20 at texture midpoint t=0.5 => depth 0.50;
   * identity projection maps that to 25 output pixels. */
  CHECK(Near(wall.x - object.x, 25.0f));
  CHECK(Near(wall.y, object.y));

  projection.bg1_plane.valid = false;
  CHECK(!Diorama_ProjectCapturedBg1Point(
      &projection, 50.0f, 25.0f, &wall, NULL, NULL));
}

static void TestBg2PlaneShapeAndWindowAreIndependent(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f;
  projection.bg2_plane.z_world = -0.30f;
  projection.bg2_plane.rake = 0.10f;
  projection.bg2_plane.u0 = 0.20f;
  projection.bg2_plane.u1 = 0.80f;
  ArRenderPointF backdrop, playfield;
  CHECK(Diorama_ProjectCapturedBg2Point(
      &projection, 50.0f, 25.0f, &backdrop, NULL, NULL));
  CHECK(Diorama_ProjectCapturedBg1Point(
      &projection, 50.0f, 25.0f, &playfield, NULL, NULL));
  CHECK(Near(backdrop.x - playfield.x, -12.5f));
  CHECK(Near(backdrop.y, playfield.y));
  projection.bg2_plane.valid = false;
  CHECK(!Diorama_ProjectCapturedBg2Point(
      &projection, 50.0f, 25.0f, &backdrop, NULL, NULL));
}

static void TestBg2FoldedOverflowProjection(void) {
  DioramaProjection projection = Projection();
  projection.matrix[8] = 1.0f;  /* make folded Z visible in screen X */
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
  CHECK(Diorama_ProjectCapturedBg2Point(
      &projection, 50.0f, 56.25f, &folded, NULL, NULL));
  CHECK(Near(folded.x, 53.75f));
  CHECK(Near(folded.y, 74.125f));

  /* Disabling the continuation returns to ordinary flat extrapolation. This
   * guards the production seam used by the waterfall veil and mist, not only
   * the mesh arithmetic. */
  projection.bg2_plane.overflow_valid = false;
  ArRenderPointF flat;
  CHECK(Diorama_ProjectCapturedBg2Point(
      &projection, 50.0f, 56.25f, &flat, NULL, NULL));
  CHECK(!Near(folded.x, flat.x));
  CHECK(!Near(folded.y, flat.y));
}

static void TestInvalidInputsFailClosed(void) {
  DioramaProjection projection = Projection();
  ArRenderPointF point = { 17.0f, 29.0f };
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 1, &point, NULL, NULL));
  CHECK(point.x == 17.0f && point.y == 29.0f);
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 4, &point, NULL, NULL));
  projection.bg1_plane.valid = false;
  CHECK(!Diorama_ProjectCapturedBg1Point(
      &projection, 0.0f, 0.0f, &point, NULL, NULL));
  projection.valid = false;
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 0, &point, NULL, NULL));
  CHECK(!Diorama_ProjectCapturedPoint(
      NULL, 0.0f, 0.0f, 0, &point, NULL, NULL));
  CHECK(!Diorama_ProjectCapturedPoint(
      &projection, 0.0f, 0.0f, 0, NULL, NULL, NULL));
}

static void TestPlaneEligibilityMatchesDrawableInputs(void) {
  CHECK(Diorama_PlaneEligible(
      SR_PPU_OVERLAY_BG2, true, true, true, false, false));
  CHECK(!Diorama_PlaneEligible(
      SR_PPU_OVERLAY_BG2, false, true, true, false, false));
  CHECK(!Diorama_PlaneEligible(
      SR_PPU_OVERLAY_BG2, true, false, true, false, false));
  CHECK(!Diorama_PlaneEligible(
      SR_PPU_OVERLAY_BG2, true, true, false, false, false));
  CHECK(!Diorama_PlaneEligible(
      SR_PPU_OVERLAY_BG2, true, true, true, false, true));
  CHECK(!Diorama_PlaneEligible(
      kDioramaPlane_Bg2Far, true, true, true, false, true));
  CHECK(!Diorama_PlaneEligible(
      SR_PPU_OVERLAY_BG3, true, true, true, true, false));

  /* A current attached effect supplies current projection content for its
   * exact BG or OBJ plane. It needs no source texture when that isolated
   * hardware band is empty, but cannot bypass visibility or skybox policy. */
  CHECK(Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_OBJ, true, true, false, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_OBJ, false, true, false, true, false, false));
  CHECK(Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_OBJ, true, false, false, true, false, false));
  CHECK(Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_BG2, true, true, false, true, false, false));
  CHECK(Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_BG1, true, false, false, true, false, false));
  CHECK(Diorama_PlaneProjectable(
      kDioramaPlane_Bg1Hi, true, false, false, true, false, false));
  CHECK(Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_BG2, true, true, true, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      kDioramaPlane_Bg2Hi, true, true, false, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_BG2, true, true, false, true, false, true));
  CHECK(!Diorama_PlaneProjectable(
      SR_PPU_OVERLAY_OBJ, true, true, false, false, false, false));
}

static void TestObjEffectMaskDistinguishesEmptyFromFailedUpload(void) {
  const uint32_t obj0 = 1u << SR_PPU_OVERLAY_OBJ;
  const uint32_t obj2 = 1u << kDioramaPlane_Obj2;
  const uint8_t required = (1u << 0) | (1u << 2);

  /* Empty OBJ0 needs its actor transform without a texture upload; OBJ2 had
   * pixels and uploaded, so both exact priorities remain available. */
  CHECK(Diorama_FilterObjEffectProjectionMask(
      required, obj0 | obj2, obj2, obj2) == required);
  /* Content without a successful upload is a real resource failure, not an
   * empty-band actor case, and therefore fails closed. */
  CHECK(Diorama_FilterObjEffectProjectionMask(
      required, obj0 | obj2, obj0 | obj2, obj2) == (1u << 2));
  CHECK(Diorama_FilterObjEffectProjectionMask(
      required, obj2, 0, 0) == (1u << 2));
  CHECK(Diorama_FilterObjEffectProjectionMask(
      0xFFu, 0, 0, 0) == 0);
}

static void TestBgEffectMaskDistinguishesEmptyFromFailedUpload(void) {
  const uint32_t bg1 = 1u << SR_PPU_OVERLAY_BG1;
  const uint32_t bg2 = 1u << SR_PPU_OVERLAY_BG2;
  const uint32_t bg1hi = 1u << kDioramaPlane_Bg1Hi;
  const uint32_t bg2hi = 1u << kDioramaPlane_Bg2Hi;
  const uint32_t required = bg1 | bg2 | bg1hi | bg2hi;

  CHECK(Diorama_FilterBgEffectProjectionMask(
      required, bg1 | bg2 | bg1hi | bg2hi, bg2, bg2) ==
      (bg1 | bg2 | bg1hi));
  CHECK(Diorama_FilterBgEffectProjectionMask(
      required, bg1 | bg2, bg1 | bg2, bg2) == bg2);
  CHECK(Diorama_FilterBgEffectProjectionMask(
      required, bg2, 0, 0) == bg2);
  CHECK(Diorama_FilterBgEffectProjectionMask(
      required, 0, 0, 0) == 0);
}

int main(void) {
  TestTiltedCameraFraming();
  TestCameraFramingProtectsNativeBand();
  TestCameraFramingRejectsUnprojectableMesh();
  TestRegisteredProjectionAndScale();
  TestPriorityPlaneShapeIsApplied();
  TestOutputViewportOriginIsApplied();
  TestCapturedTextureOriginIsApplied();
  TestBg1PlaneShapeIsIndependent();
  TestBg2PlaneShapeAndWindowAreIndependent();
  TestBg2FoldedOverflowProjection();
  TestInvalidInputsFailClosed();
  TestPlaneEligibilityMatchesDrawableInputs();
  TestObjEffectMaskDistinguishesEmptyFromFailedUpload();
  TestBgEffectMaskDistinguishesEmptyFromFailedUpload();
  if (g_failures) {
    fprintf(stderr, "%d diorama projection test(s) failed\n", g_failures);
    return 1;
  }
  puts("diorama projection: all tests passed");
  return 0;
}
