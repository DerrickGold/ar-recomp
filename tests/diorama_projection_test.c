#include <math.h>
#include <stdio.h>
#include <string.h>

#include "diorama.h"

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

static void TestRegisteredProjectionAndScale(void) {
  DioramaProjection projection = Projection();
  SDL_FPoint point;
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
  SDL_FPoint raked, flat;
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
  SDL_FPoint point;
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
  SDL_FPoint point;
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
  SDL_FPoint wall, object;
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
  SDL_FPoint backdrop, playfield;
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

static void TestInvalidInputsFailClosed(void) {
  DioramaProjection projection = Projection();
  SDL_FPoint point = { 17.0f, 29.0f };
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
      kPpuOverlaySource_Bg2, true, true, true, false, false));
  CHECK(!Diorama_PlaneEligible(
      kPpuOverlaySource_Bg2, false, true, true, false, false));
  CHECK(!Diorama_PlaneEligible(
      kPpuOverlaySource_Bg2, true, false, true, false, false));
  CHECK(!Diorama_PlaneEligible(
      kPpuOverlaySource_Bg2, true, true, false, false, false));
  CHECK(!Diorama_PlaneEligible(
      kPpuOverlaySource_Bg2, true, true, true, false, true));
  CHECK(!Diorama_PlaneEligible(
      kPpuOverlaySource_Bg3, true, true, true, true, false));

  /* A current actor effect supplies current projection content for its exact
   * visible OBJ band. It must not weaken the BG2 drawable-pixel contract that
   * keeps the waterfall hidden when its source backdrop is skipped. */
  CHECK(Diorama_PlaneProjectable(
      kPpuOverlaySource_Obj, true, true, false, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      kPpuOverlaySource_Obj, false, true, false, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      kPpuOverlaySource_Obj, true, false, false, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      kPpuOverlaySource_Bg2, true, true, false, true, false, false));
  CHECK(Diorama_PlaneProjectable(
      kPpuOverlaySource_Bg2, true, true, true, true, false, false));
  CHECK(!Diorama_PlaneProjectable(
      kPpuOverlaySource_Obj, true, true, false, false, false, false));
}

static void TestObjEffectMaskDistinguishesEmptyFromFailedUpload(void) {
  const uint32_t obj0 = 1u << kPpuOverlaySource_Obj;
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

int main(void) {
  TestRegisteredProjectionAndScale();
  TestPriorityPlaneShapeIsApplied();
  TestOutputViewportOriginIsApplied();
  TestCapturedTextureOriginIsApplied();
  TestBg1PlaneShapeIsIndependent();
  TestBg2PlaneShapeAndWindowAreIndependent();
  TestInvalidInputsFailClosed();
  TestPlaneEligibilityMatchesDrawableInputs();
  TestObjEffectMaskDistinguishesEmptyFromFailedUpload();
  if (g_failures) {
    fprintf(stderr, "%d diorama projection test(s) failed\n", g_failures);
    return 1;
  }
  puts("diorama projection: all tests passed");
  return 0;
}
