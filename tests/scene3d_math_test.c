#include "scene3d_math.h"

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

int main(void) {
  const int width = 256, height = 224;
  const float aspect = (float)width / (float)height;
  Scene3DCamera camera = {
    .distance = Scene3D_AutoFitDistance(0.4f),
    .fov_y = 0.4f,
  };
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint top_left = Scene3D_ProjectWorldPoint(
      matrix, -aspect * 0.5f, 0.5f, 0.0f, width, height);
  Scene3DPoint bottom_right = Scene3D_ProjectWorldPoint(
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
  Scene3DPoint tilted_top_left = Scene3D_ProjectWorldPoint(
      matrix, -aspect * 0.5f, 0.5f, 0.0f, width, height);
  Scene3DPoint tilted_bottom_right = Scene3D_ProjectWorldPoint(
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

  /* D3c virtual height: the shipped SIM pitch is negative, i.e. the camera
   * looks down on the ground plane, so lifting a billboard along +Z must move
   * it up-screen and closer to the camera. Zero height must reproduce the
   * D3b ground anchor exactly, which is the documented VirtualHeight bypass. */
  camera.tilt_x = -0.35f;
  camera.tilt_y = 0.0f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint grounded = Scene3D_ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 0.0f, width, height);
  Scene3DPoint zero_height = Scene3D_ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 0.0f, width, height);
  Scene3DPoint lifted = Scene3D_ProjectWorldPoint(
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
  Scene3DPoint contact = Scene3D_ProjectShadowPoint(
      matrix, 0.1f, -0.2f, 0.0f, 0.35f, 0.12f, width, height);
  CHECK(Near(contact.x, grounded.x) && Near(contact.y, grounded.y));
  float shadow_height = 24.0f / 224.0f;
  Scene3DPoint offset_shadow = Scene3D_ProjectShadowPoint(
      matrix, 0.1f, -0.2f, shadow_height, 0.35f, 0.12f, width, height);
  Scene3DPoint sheared_ground = Scene3D_ProjectWorldPoint(
      matrix, 0.1f + shadow_height * 0.35f, -0.2f + shadow_height * 0.12f,
      0.0f, width, height);
  CHECK(Near(offset_shadow.x, sheared_ground.x) &&
        Near(offset_shadow.y, sheared_ground.y));
  CHECK(offset_shadow.x > contact.x);
  /* Higher casters throw their shadow further; the relation is monotonic so a
   * slewed height cannot make a shadow jitter back and forth. */
  Scene3DPoint higher_shadow = Scene3D_ProjectShadowPoint(
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
  Scene3DPoint overhead = Scene3D_ProjectShadowPoint(
      matrix, 0.1f, -0.2f, shadow_height, 0.0f, 0.0f, width, height);
  CHECK(Near(overhead.x, grounded.x) && Near(overhead.y, grounded.y));

  /* A ground plane seen exactly edge-on still yields a finite anchor. */
  camera.tilt_x = -1.5707963f;
  Scene3D_BuildViewProjection(&camera, width, height, matrix);
  Scene3DPoint edge_on = Scene3D_ProjectWorldPoint(
      matrix, 0.1f, -0.2f, 24.0f / 224.0f, width, height);
  CHECK(isfinite(edge_on.x) && isfinite(edge_on.y));
  Scene3DPoint edge_on_shadow = Scene3D_ProjectShadowPoint(
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
        Scene3D_ProjectWorldPoint(matrix, 0.0f, y, 0.0f, width, height);
    CHECK(far_point.y > horizon);
    CHECK(far_point.y < previous);
    previous = far_point.y;
  }
  CHECK(previous - horizon < 1.0f);

  /* The reason the backdrop does not anchor to it: across the whole settable
   * pitch range the vanishing line is outside a 224-row viewport, so there is
   * no horizon in frame to put a sky on. Asserted rather than commented, so
   * widening the range trips this and forces the backdrop to be revisited. */
  for (int mrad = -700; mrad <= 700; mrad += 25) {
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
        Scene3D_ProjectWorldPoint(matrix, x, 100000.0f, 0.0f, width, height);
    CHECK(fabsf(far_point.y - yawed) < 1.0f);
  }

  /* Scales with the output, since it is reported in destination pixels. */
  float doubled = 0.0f;
  CHECK(Scene3D_GroundHorizonScreenY(matrix, height * 2, &doubled));
  CHECK(Near(doubled, yawed * 2.0f));

  if (!failures) puts("scene3d math tests passed");
  return failures ? 1 : 0;
}
