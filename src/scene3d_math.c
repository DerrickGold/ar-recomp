#include "scene3d_math.h"

#include <math.h>

const float kScene3DMinimumProjectionDepth = 0.0001f;
/* Below this the camera is looking straight down and the ground has no depth
 * gradient to take a direction from. */
static const float kMinimumGroundDepthGradient = 0.0001f;

static void Mat4Mul(const float a[16], const float b[16], float out[16]) {
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++)
        sum += a[k * 4 + row] * b[column * 4 + k];
      out[column * 4 + row] = sum;
    }
  }
}

void Scene3D_BuildViewProjection(const Scene3DCamera *camera,
                                 int output_width, int output_height,
                                 float out_matrix[16]) {
  const float near_plane = 0.1f, far_plane = 100.0f;
  float aspect = output_height > 0
      ? (float)output_width / (float)output_height : 1.0f;
  float f = 1.0f / tanf(camera->fov_y * 0.5f);
  float projection[16] = {
    f / aspect, 0, 0, 0,
    0, f, 0, 0,
    0, 0, (far_plane + near_plane) / (near_plane - far_plane), -1,
    0, 0, (2 * far_plane * near_plane) / (near_plane - far_plane), 0,
  };
  float cx = cosf(camera->tilt_x), sx = sinf(camera->tilt_x);
  float cy = cosf(camera->tilt_y), sy = sinf(camera->tilt_y);
  float rotate_y[16] = {
    cy,0,sy,0,  0,1,0,0,  -sy,0,cy,0,  0,0,0,1,
  };
  float rotate_x[16] = {
    1,0,0,0,  0,cx,sx,0,  0,-sx,cx,0,  0,0,0,1,
  };
  float translate[16] = {
    1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,-camera->distance,1,
  };
  float rotation[16], view[16];
  Mat4Mul(rotate_x, rotate_y, rotation);
  Mat4Mul(translate, rotation, view);
  Mat4Mul(projection, view, out_matrix);
}

bool Scene3D_ProjectWorldPoint(const float matrix[16],
                               float x, float y, float z,
                               int output_width, int output_height,
                               Scene3DPoint *out_point) {
  if (!out_point) return false;
  float clip_x = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
  float clip_y = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
  float clip_w = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
  if (clip_w <= kScene3DMinimumProjectionDepth) return false;
  float inverse_w = 1.0f / clip_w;
  Scene3DPoint projected = {
    (clip_x * inverse_w * 0.5f + 0.5f) * output_width,
    (1.0f - (clip_y * inverse_w * 0.5f + 0.5f)) * output_height,
  };
  if (!isfinite(projected.x) || !isfinite(projected.y)) return false;
  *out_point = projected;
  return true;
}

bool Scene3D_ProjectWorldPointWithDepth(
    const float matrix[16], float x, float y, float z,
    int output_width, int output_height,
    Scene3DPoint *out_point, float *out_depth) {
  if (!out_point || !out_depth) return false;
  const float clip_x =
      matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
  const float clip_y =
      matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
  const float clip_z =
      matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
  const float clip_w =
      matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
  if (clip_w <= kScene3DMinimumProjectionDepth) return false;
  const float inverse_w = 1.0f / clip_w;
  const Scene3DPoint projected = {
    (clip_x * inverse_w * 0.5f + 0.5f) * output_width,
    (1.0f - (clip_y * inverse_w * 0.5f + 0.5f)) * output_height,
  };
  const float depth = clip_z * inverse_w * 0.5f + 0.5f;
  if (!isfinite(projected.x) || !isfinite(projected.y) || !isfinite(depth))
    return false;
  *out_point = projected;
  *out_depth = depth;
  return true;
}

float Scene3D_ProjectBillboardScale(const float matrix[16],
                                    float x, float y, float z,
                                    float reference_depth) {
  float clip_w = matrix[3] * x + matrix[7] * y +
                 matrix[11] * z + matrix[15];
  if (clip_w <= kScene3DMinimumProjectionDepth || reference_depth <= 0.0f)
    return 0.0f;
  return reference_depth / clip_w;
}

bool Scene3D_ProjectShadowPoint(const float matrix[16],
                                float x, float y, float z,
                                float light_x, float light_y,
                                int output_width, int output_height,
                                Scene3DPoint *out_point) {
  return Scene3D_ProjectWorldPoint(matrix, x + z * light_x, y + z * light_y,
                                   0.0f, output_width, output_height,
                                   out_point);
}

float Scene3D_ShadowFootprintScale(float z, float shrink) {
  if (z <= 0.0f || shrink <= 0.0f) return 1.0f;
  return 1.0f / (1.0f + z * shrink);
}

float Scene3D_AutoFitDistance(float fov_y) {
  return 0.5f / tanf(fov_y * 0.5f);
}

void Scene3D_CappedTargetSize(int viewport_width, int viewport_height,
                              uint64_t maximum_pixels,
                              int *target_width, int *target_height) {
  int width = viewport_width > 0 ? viewport_width : 1;
  int height = viewport_height > 0 ? viewport_height : 1;
  while ((uint64_t)width * (uint64_t)height > maximum_pixels &&
         width > 1 && height > 1) {
    width = width / 2 + width % 2;
    height = height / 2 + height % 2;
  }
  if (target_width) *target_width = width;
  if (target_height) *target_height = height;
}

float Scene3D_WrappedTextureOffset(uint64_t elapsed_ms,
                                   float units_per_second,
                                   float speed_scale) {
  double offset = (double)elapsed_ms * 0.001 *
      (double)units_per_second * (double)speed_scale;
  double wrapped = fmod(offset, 1.0);
  if (wrapped < 0.0) wrapped += 1.0;
  return (float)wrapped;
}

float Scene3D_ClipDepth(const float matrix[16], float x, float y, float z) {
  return matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
}

bool Scene3D_NormalizedDepth(const float matrix[16],
                             float x, float y, float z,
                             float *out_depth) {
  if (!out_depth) return false;
  float clip_w = Scene3D_ClipDepth(matrix, x, y, z);
  if (clip_w <= kScene3DMinimumProjectionDepth) return false;
  float clip_z = matrix[2] * x + matrix[6] * y +
      matrix[10] * z + matrix[14];
  float depth = clip_z / clip_w * 0.5f + 0.5f;
  if (!isfinite(depth)) return false;
  *out_depth = depth;
  return true;
}

bool Scene3D_GroundDepthDirection(const float matrix[16], float aspect,
                                  float source_width, float source_height,
                                  float *out_x, float *out_y,
                                  float *out_length) {
  if (!matrix || source_width <= 0.0f || source_height <= 0.0f) return false;
  /* Clip W is the depth gradient, so its X and Y rows are the direction in
   * which the ground recedes. Expressing them in authored source pixels
   * rather than raw matrix coefficients keeps the result independent of the
   * window's aspect ratio, which matrix X carries and matrix Y does not. */
  float x = matrix[3] * aspect / source_width;
  float y = -matrix[7] / source_height;
  float length = sqrtf(x * x + y * y);
  if (!isfinite(length) || length < kMinimumGroundDepthGradient) return false;
  if (out_x) *out_x = x / length;
  if (out_y) *out_y = y / length;
  if (out_length) *out_length = length;
  return true;
}

bool Scene3D_DepthBoundaryY(const float matrix[16], float x, float z,
                            float minimum_depth, float *boundary_y,
                            bool *increasing) {
  float slope = matrix[7];  /* depth gained per unit of world y */
  if (slope == 0.0f) return false;
  float base = matrix[3] * x + matrix[11] * z + matrix[15];
  if (boundary_y) *boundary_y = (minimum_depth - base) / slope;
  if (increasing) *increasing = slope > 0.0f;
  return true;
}

bool Scene3D_GroundDepthBoundaryY(const float matrix[16], float x,
                                  float minimum_depth, float *boundary_y,
                                  bool *increasing) {
  return Scene3D_DepthBoundaryY(matrix, x, 0.0f, minimum_depth, boundary_y,
                                increasing);
}

bool Scene3D_GroundHorizonScreenY(const float matrix[16], int output_height,
                                  float *screen_y) {
  /* On the ground plane (z = 0), as y grows the x and constant terms become
   * negligible against the y terms, so clip_y/clip_w tends to a ratio of two
   * matrix entries. */
  float slope_y = matrix[5];
  float slope_w = matrix[7];
  if (slope_w == 0.0f) return false;
  float ndc_y = slope_y / slope_w;
  if (screen_y)
    *screen_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)output_height;
  return true;
}
