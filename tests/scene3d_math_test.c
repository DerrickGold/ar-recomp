#include "scene3d_math.h"
#include "sim/sim3d_camera_limits.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
  } \
} while (0)

static bool Near(float actual, float expected) {
  return fabsf(actual - expected) < 0.001f;
}

static Scene3DPoint ProjectWorldPoint(const float matrix[16],
                                      float x, float y, float z,
                                      int width, int height) {
  Scene3DPoint point = {0};
  CHECK(Scene3D_ProjectWorldPoint(matrix, x, y, z, width, height, &point));
  return point;
}

static Scene3DPoint ProjectShadowPoint(const float matrix[16],
                                       float x, float y, float z,
                                       float light_x, float light_y,
                                       int width, int height) {
  Scene3DPoint point = {0};
  CHECK(Scene3D_ProjectShadowPoint(matrix, x, y, z, light_x, light_y,
                                   width, height, &point));
  return point;
}

static void CheckClipDepthPreservesPainterOrder(
    const float matrix[16], const float points[][3], int point_count) {
  for (int a = 0; a < point_count; a++) {
    float normalized_a = 0.0f;
    float clip_a = Scene3D_ClipDepth(
        matrix, points[a][0], points[a][1], points[a][2]);
    CHECK(Scene3D_NormalizedDepth(
        matrix, points[a][0], points[a][1], points[a][2], &normalized_a));
    for (int b = a + 1; b < point_count; b++) {
      float normalized_b = 0.0f;
      float clip_b = Scene3D_ClipDepth(
          matrix, points[b][0], points[b][1], points[b][2]);
      CHECK(Scene3D_NormalizedDepth(
          matrix, points[b][0], points[b][1], points[b][2], &normalized_b));
      if (fabsf(clip_a - clip_b) < 0.00001f) continue;
      CHECK((clip_a < clip_b) == (normalized_a < normalized_b));

      /* A camera/map pan translates every terrain cell equally. Clip W is
       * affine, so the common term cannot change a pair's painter order. */
      const float translated_a = Scene3D_ClipDepth(
          matrix, points[a][0] + 0.37f, points[a][1] - 0.24f,
          points[a][2] + 0.11f);
      const float translated_b = Scene3D_ClipDepth(
          matrix, points[b][0] + 0.37f, points[b][1] - 0.24f,
          points[b][2] + 0.11f);
      CHECK((clip_a < clip_b) == (translated_a < translated_b));
    }
  }
}

int main(void) {
  const int width = 256, height = 224;
  const float aspect = (float)width / (float)height;
  Scene3DCamera camera = {
    .distance = Scene3D_AutoFitDistance(0.4f),
    .fov_y = 0.4f,
  };
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint top_left = ProjectWorldPoint(
      matrix, -aspect * 0.5f, 0.5f, 0.0f, width, height);
  Scene3DPoint bottom_right = ProjectWorldPoint(
      matrix, aspect * 0.5f, -0.5f, 0.0f, width, height);
  CHECK(Near(top_left.x, 0.0f));
  CHECK(Near(top_left.y, 0.0f));
  CHECK(Near(bottom_right.x, (float)width));
  CHECK(Near(bottom_right.y, (float)height));
  CHECK(Near(Scene3D_ProjectBillboardScale(
                 matrix, 0.0f, 0.0f, 0.0f, camera.distance), 1.0f));

  float fitted_distance = camera.distance;
  camera.distance = fitted_distance * 2.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  CHECK(Near(Scene3D_ProjectBillboardScale(
                 matrix, 0.0f, 0.0f, 0.0f, fitted_distance), 0.5f));
  camera.distance = fitted_distance;

  camera.tilt_x = 0.35f;
  camera.tilt_y = -0.2f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint tilted_top_left = ProjectWorldPoint(
      matrix, -aspect * 0.5f, 0.5f, 0.0f, width, height);
  Scene3DPoint tilted_bottom_right = ProjectWorldPoint(
      matrix, aspect * 0.5f, -0.5f, 0.0f, width, height);
  CHECK(isfinite(tilted_top_left.x) && isfinite(tilted_top_left.y));
  CHECK(isfinite(tilted_bottom_right.x) && isfinite(tilted_bottom_right.y));
  CHECK(!Near(tilted_top_left.x, top_left.x) ||
        !Near(tilted_top_left.y, top_left.y));
  CHECK(!Near(tilted_bottom_right.x, bottom_right.x) ||
        !Near(tilted_bottom_right.y, bottom_right.y));
  float top_scale = Scene3D_ProjectBillboardScale(
      matrix, 0.0f, 0.5f, 0.0f, camera.distance);
  float bottom_scale = Scene3D_ProjectBillboardScale(
      matrix, 0.0f, -0.5f, 0.0f, camera.distance);
  CHECK(isfinite(top_scale) && isfinite(bottom_scale));
  CHECK(top_scale > 0.0f && bottom_scale > 0.0f);
  CHECK(!Near(top_scale, bottom_scale));

  /* Projection is a visibility contract, not merely a perspective divide.
   * A point on or behind the camera plane must be rejected without exposing
   * a huge/mirrored coordinate or partially overwriting the caller's output. */
  const float depth_probe[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 0, 1,
    0, 0, 0, 0,
  };
  Scene3DPoint rejected = { 17.0f, 29.0f };
  CHECK(!Scene3D_ProjectWorldPoint(
      depth_probe, 0.0f, 0.0f, 0.0f, width, height, &rejected));
  CHECK(rejected.x == 17.0f && rejected.y == 29.0f);
  CHECK(!Scene3D_ProjectWorldPoint(
      depth_probe, 0.0f, 0.0f, -1.0f, width, height, &rejected));
  CHECK(Scene3D_ProjectWorldPoint(
      depth_probe, 0.0f, 0.0f, 1.0f, width, height, &rejected));
  CHECK(isfinite(rejected.x) && isfinite(rejected.y));

  /* D3c virtual height: the shipped SIM pitch is negative, i.e. the camera
   * looks down on the ground plane, so lifting a billboard along +Z must move
   * it up-screen and closer to the camera. Zero height must reproduce the
   * D3b ground anchor exactly, which is the documented VirtualHeight bypass. */
  camera.tilt_x = -0.35f;
  camera.tilt_y = 0.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint grounded = ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 0.0f, width, height);
  Scene3DPoint zero_height = ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 0.0f, width, height);
  Scene3DPoint lifted = ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 24.0f / 224.0f, width, height);
  CHECK(Near(zero_height.x, grounded.x) && Near(zero_height.y, grounded.y));
  CHECK(lifted.y < grounded.y);
  CHECK(isfinite(lifted.x) && isfinite(lifted.y));
  float grounded_scale = Scene3D_ProjectBillboardScale(
      matrix, 0.1f, -0.2f, 0.0f, camera.distance);
  float lifted_scale = Scene3D_ProjectBillboardScale(
      matrix, 0.1f, -0.2f, 24.0f / 224.0f, camera.distance);
  CHECK(grounded_scale > 0.0f && lifted_scale > grounded_scale);
  /* D4a shadows. A grounded caster's shadow must land exactly on its own
   * anchor, and lifting it must slide the shadow along the light direction
   * while the shadow itself stays on the ground plane — i.e. it must equal the
   * projection of the sheared ground point, never of the lifted point. */
  camera.tilt_x = -0.35f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint contact = ProjectShadowPoint(
      matrix, 0.1f, -0.2f, 0.0f, 0.35f, 0.12f, width, height);
  CHECK(Near(contact.x, grounded.x) && Near(contact.y, grounded.y));
  float shadow_height = 24.0f / 224.0f;
  Scene3DPoint offset_shadow = ProjectShadowPoint(
      matrix, 0.1f, -0.2f, shadow_height, 0.35f, 0.12f, width, height);
  Scene3DPoint sheared_ground = ProjectWorldPoint(
      matrix, 0.1f + shadow_height * 0.35f, -0.2f + shadow_height * 0.12f,
      0.0f, width, height);
  CHECK(Near(offset_shadow.x, sheared_ground.x) &&
        Near(offset_shadow.y, sheared_ground.y));
  CHECK(offset_shadow.x > contact.x);
  /* Higher casters throw their shadow further; the relation is monotonic so a
   * slewed height cannot make a shadow jitter back and forth. */
  Scene3DPoint higher_shadow = ProjectShadowPoint(
      matrix, 0.1f, -0.2f, shadow_height * 2.0f, 0.35f, 0.12f, width, height);
  CHECK(higher_shadow.x > offset_shadow.x);
  /* Footprint shrink: full size on the ground, monotonically smaller with
   * height, and never zero or inverted however high the caster goes. */
  CHECK(Near(Scene3D_ShadowFootprintScale(0.0f, 4.0f), 1.0f));
  CHECK(Near(Scene3D_ShadowFootprintScale(shadow_height, 0.0f), 1.0f));
  float near_scale = Scene3D_ShadowFootprintScale(shadow_height, 4.0f);
  float far_scale = Scene3D_ShadowFootprintScale(shadow_height * 2.0f, 4.0f);
  CHECK(near_scale < 1.0f && near_scale > 0.0f);
  CHECK(far_scale < near_scale && far_scale > 0.0f);
  CHECK(Scene3D_ShadowFootprintScale(1000.0f, 4.0f) > 0.0f);
  /* A negative height cannot enlarge a shadow past its own art. */
  CHECK(Near(Scene3D_ShadowFootprintScale(-1.0f, 4.0f), 1.0f));

  /* A zero light direction degenerates to the contact shadow, not to NaN. */
  Scene3DPoint overhead = ProjectShadowPoint(
      matrix, 0.1f, -0.2f, shadow_height, 0.0f, 0.0f, width, height);
  CHECK(Near(overhead.x, grounded.x) && Near(overhead.y, grounded.y));

  /* A ground plane seen exactly edge-on still yields a finite anchor. */
  camera.tilt_x = -1.5707963f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint edge_on = ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 24.0f / 224.0f, width, height);
  CHECK(isfinite(edge_on.x) && isfinite(edge_on.y));
  Scene3DPoint edge_on_shadow = ProjectShadowPoint(
      matrix, 0.1f, -0.2f, 24.0f / 224.0f, 0.35f, 0.12f, width, height);
  CHECK(isfinite(edge_on_shadow.x) && isfinite(edge_on_shadow.y));

  /* Depth bound for the world-map ground extension. With the ground tilted
   * away, depth grows with y: the far edge is safe however far it reaches,
   * and it is the near edge — extended toward the viewer — that crosses the
   * camera plane. Getting this backwards folds the mesh over the scene, so
   * both the boundary and its direction are asserted. */
  camera.tilt_x = -0.35f;
  camera.distance = Scene3D_AutoFitDistance(camera.fov_y);
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  CHECK(Scene3D_ClipDepth(matrix, 0.0f, 0.0f, 0.0f) > 0.0f);
  float normalized_depth = -1.0f;
  CHECK(Scene3D_NormalizedDepth(
      matrix, 0.0f, 0.0f, 0.0f, &normalized_depth));
  CHECK(normalized_depth >= 0.0f && normalized_depth <= 1.0f);
  Scene3DPoint separate_projection = ProjectWorldPoint(
      matrix, 0.17f, -0.23f, 0.11f, width, height);
  float separate_depth = 0.0f;
  CHECK(Scene3D_NormalizedDepth(
      matrix, 0.17f, -0.23f, 0.11f, &separate_depth));
  Scene3DPoint combined_projection = {0};
  float combined_depth = 0.0f;
  CHECK(Scene3D_ProjectWorldPointWithDepth(
      matrix, 0.17f, -0.23f, 0.11f, width, height,
      &combined_projection, &combined_depth));
  CHECK(Near(combined_projection.x, separate_projection.x));
  CHECK(Near(combined_projection.y, separate_projection.y));
  CHECK(Near(combined_depth, separate_depth));

  /* The terrain painter cache sorts by homogeneous clip W instead of GPU
   * normalized depth. With this perspective projection those keys are
   * strictly monotonic, which removes one divide per cell without changing
   * overlap. Exercise the complete settable pitch range, both yaw directions,
   * and non-flat terrain samples so that contract cannot regress silently. */
  const float painter_points[][3] = {
    {-0.60f, -0.45f, 0.00f}, {-0.25f, -0.20f, 0.08f},
    { 0.00f,  0.00f, 0.18f}, { 0.30f,  0.22f, 0.04f},
    { 0.58f,  0.47f, 0.13f}, {-0.50f,  0.36f, 0.24f},
    { 0.48f, -0.38f, 0.20f},
  };
  const float painter_yaws[] = {-0.60f, 0.0f, 0.60f};
  for (int mrad = kSim3DCameraPitchMinimumMrad;
       mrad <= kSim3DCameraPitchMaximumMrad; mrad += 25) {
    camera.tilt_x = (float)mrad / 1000.0f;
    for (size_t yaw = 0;
         yaw < sizeof(painter_yaws) / sizeof(painter_yaws[0]); yaw++) {
      camera.tilt_y = painter_yaws[yaw];
      Scene3D_BuildViewProjection(&camera, width, height, matrix);
      CheckClipDepthPreservesPainterOrder(
          matrix, painter_points,
          (int)(sizeof(painter_points) / sizeof(painter_points[0])));
    }
  }
  camera.tilt_x = -0.35f;
  camera.tilt_y = 0.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  float boundary = 0.0f;
  bool increasing = false;
  CHECK(Scene3D_GroundDepthBoundaryY(matrix, 0.0f, 0.35f, &boundary,
                                     &increasing));
  CHECK(increasing);
  /* The whole unit ground quad sits on the safe side of the boundary. */
  CHECK(boundary < -0.5f);
  CHECK(Near(Scene3D_ClipDepth(matrix, 0.0f, boundary, 0.0f), 0.35f));
  CHECK(Scene3D_ClipDepth(matrix, 0.0f, boundary - 1.0f, 0.0f) < 0.35f);
  CHECK(Scene3D_ClipDepth(matrix, 0.0f, boundary + 1.0f, 0.0f) > 0.35f);
  /* Lifted planes need their own boundary. Reusing the z=0 result for clouds
   * can leave a row behind the camera even though the ground row is safe. */
  float lifted_boundary = 0.0f;
  const float cloud_altitude = 72.0f / 224.0f;
  CHECK(Scene3D_DepthBoundaryY(matrix, 0.0f, cloud_altitude, 0.35f,
                               &lifted_boundary, NULL));
  CHECK(Near(Scene3D_ClipDepth(
                 matrix, 0.0f, lifted_boundary, cloud_altitude), 0.35f));
  CHECK(!Near(lifted_boundary, boundary));
  /* Tilting the other way reverses which edge is dangerous. */
  camera.tilt_x = 0.35f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  CHECK(Scene3D_GroundDepthBoundaryY(matrix, 0.0f, 0.35f, &boundary,
                                     &increasing));
  CHECK(!increasing);
  /* A camera with no pitch takes no depth from y at all: no bound exists. */
  camera.tilt_x = 0.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  CHECK(!Scene3D_GroundDepthBoundaryY(matrix, 0.0f, 0.35f, &boundary,
                                      &increasing));

  /* Horizon. A pitchless camera has no vanishing line to report -- same
   * degenerate case the depth boundary refuses, and for the same reason: the
   * ground contributes nothing to depth. */
  float horizon = 12345.0f;
  CHECK(!Scene3D_GroundHorizonScreenY(matrix, height, &horizon));
  CHECK(horizon == 12345.0f);  /* untouched on failure */

  camera.tilt_x = -0.35f;   /* the shipped default pitch */
  camera.tilt_y = 0.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  CHECK(Scene3D_GroundHorizonScreenY(matrix, height, &horizon));

  /* The horizon is the limit of the ground projection, so projecting ever
   * more distant ground must converge on it from the near side and never
   * cross it. This is the property the flat-fill fallback depends on: sky
   * above, ground below, no seam either way. */
  float previous = horizon + 1.0e6f;
  for (float y = 100.0f; y <= 1000000.0f; y *= 10.0f) {
    Scene3DPoint far_point =
        ProjectWorldPoint(matrix, 0.0f, y, 0.0f, width, height);
    CHECK(far_point.y > horizon);
    CHECK(far_point.y < previous);
    previous = far_point.y;
  }
  CHECK(previous - horizon < 1.0f);

  /* The reason the backdrop does not anchor to it: across the whole settable
   * pitch range the vanishing line is outside a 224-row viewport, so there is
   * no horizon in frame to put a sky on. Asserted rather than commented, so
   * widening the range trips this and forces the backdrop to be revisited. */
  for (int mrad = kSim3DCameraPitchMinimumMrad;
       mrad <= kSim3DCameraPitchMaximumMrad; mrad += 25) {
    camera.tilt_x = (float)mrad / 1000.0f;
    Scene3D_BuildViewProjection(&camera, width, height, matrix);
    float off = 0.0f;
    if (!Scene3D_GroundHorizonScreenY(matrix, height, &off)) continue;
    CHECK(off < 0.0f || off > (float)height);
  }
  camera.tilt_x = -0.35f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);

  /* Yaw does not tilt it: the x terms are constant in the limit and cancel in
   * the ratio, so the horizon stays a horizontal line and one screen y
   * describes it for every column. */
  camera.tilt_y = 0.35f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  float yawed = 0.0f;
  CHECK(Scene3D_GroundHorizonScreenY(matrix, height, &yawed));
  for (float x = -2.0f; x <= 2.0f; x += 1.0f) {
    Scene3DPoint far_point =
        ProjectWorldPoint(matrix, x, 100000.0f, 0.0f, width, height);
    CHECK(fabsf(far_point.y - yawed) < 1.0f);
  }

  /* Scales with the output, since it is reported in destination pixels. */
  float doubled = 0.0f;
  CHECK(Scene3D_GroundHorizonScreenY(matrix, height * 2, &doubled));
  CHECK(Near(doubled, yawed * 2.0f));

  /* Wrapping completed texture motion is continuous at the old one-hour
   * clock reset and remains bounded even after very long uptimes. */
  float before_hour =
      Scene3D_WrappedTextureOffset(3599999, 0.0060f, 1.0f);
  float at_hour =
      Scene3D_WrappedTextureOffset(3600000, 0.0060f, 1.0f);
  CHECK(fabsf(at_hour - before_hour) < 0.001f);
  float long_running = Scene3D_WrappedTextureOffset(
      UINT64_C(365) * 24 * 60 * 60 * 1000, 0.0094f, 0.73f);
  CHECK(isfinite(long_running));
  CHECK(long_running >= 0.0f && long_running < 1.0f);

  /* Working targets stay native through 1440p, halve once at 4K, and keep
   * halving for unusually large outputs. Odd sizes use ceil division so the
   * last source row/column is never silently discarded. */
  int target_width = 0, target_height = 0;
  Scene3D_CappedTargetSize(
      2560, 1440, UINT64_C(4) * 1024 * 1024,
      &target_width, &target_height);
  CHECK(target_width == 2560 && target_height == 1440);
  Scene3D_CappedTargetSize(
      3840, 2160, UINT64_C(4) * 1024 * 1024,
      &target_width, &target_height);
  CHECK(target_width == 1920 && target_height == 1080);
  Scene3D_CappedTargetSize(
      7680, 4320, UINT64_C(4) * 1024 * 1024,
      &target_width, &target_height);
  CHECK(target_width == 1920 && target_height == 1080);
  Scene3D_CappedTargetSize(
      5001, 3001, UINT64_C(4) * 1024 * 1024,
      &target_width, &target_height);
  CHECK(target_width == 2501 && target_height == 1501);

  /* The ground's away-from-camera direction. Anything a tilted camera pushes
   * "back" - the mountain relief stack, the model lean - has to follow this
   * rather than map north, or it fans out sideways as soon as the camera
   * yaws and the pushed-back copies read as sliding off the ground. */
  const float source = 512.0f;
  float ground_aspect = (float)width / height;
  camera.tilt_x = -0.575f;
  camera.tilt_y = 0.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  float away_x = 1.0f, away_y = 0.0f, gradient = 0.0f;
  CHECK(Scene3D_GroundDepthDirection(matrix, ground_aspect, source, source,
                                     &away_x, &away_y, &gradient));
  /* A camera with no yaw looks straight up the map, so the direction is due
   * north and the old fixed northward offset is reproduced exactly. */
  CHECK(Near(away_x, 0.0f) && Near(away_y, -1.0f));
  CHECK(gradient > 0.0f);

  /* Yaw rotates it, and it stays a unit vector pointing away from the
   * camera in both directions. */
  float previous_x = 0.0f;
  for (int yaw_mrad = 150; yaw_mrad <= 600; yaw_mrad += 150) {
    camera.tilt_y = yaw_mrad / 1000.0f;
    Scene3D_BuildViewProjection(&camera, width, height, matrix);
    float x = 0.0f, y = 0.0f;
    CHECK(Scene3D_GroundDepthDirection(matrix, ground_aspect, source, source,
                                       &x, &y, NULL));
    CHECK(Near(sqrtf(x * x + y * y), 1.0f));
    CHECK(y < 0.0f);
    /* Turning further from north keeps moving the direction the same way. */
    CHECK(x < previous_x);
    previous_x = x;

    camera.tilt_y = -yaw_mrad / 1000.0f;
    Scene3D_BuildViewProjection(&camera, width, height, matrix);
    float mirror_x = 0.0f, mirror_y = 0.0f;
    CHECK(Scene3D_GroundDepthDirection(matrix, ground_aspect, source, source,
                                       &mirror_x, &mirror_y, NULL));
    CHECK(Near(mirror_x, -x) && Near(mirror_y, y));
  }

  if (!failures) puts("scene3d math tests passed");
  return failures ? 1 : 0;
}
