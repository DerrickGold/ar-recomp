#include "scene3d_math.h"

#include <math.h>

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

Scene3DPoint Scene3D_ProjectWorldPoint(const float matrix[16],
                                      float x, float y, float z,
                                      int output_width, int output_height) {
  float clip_x = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
  float clip_y = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
  float clip_w = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
  float inverse_w = clip_w != 0.0f ? 1.0f / clip_w : 1.0f;
  return (Scene3DPoint){
    (clip_x * inverse_w * 0.5f + 0.5f) * output_width,
    (1.0f - (clip_y * inverse_w * 0.5f + 0.5f)) * output_height,
  };
}

float Scene3D_ProjectBillboardScale(const float matrix[16],
                                    float x, float y, float z,
                                    float reference_depth) {
  float clip_w = matrix[3] * x + matrix[7] * y +
                 matrix[11] * z + matrix[15];
  if (clip_w <= 0.0001f || reference_depth <= 0.0f) return 0.0f;
  return reference_depth / clip_w;
}

Scene3DPoint Scene3D_ProjectShadowPoint(const float matrix[16],
                                        float x, float y, float z,
                                        float light_x, float light_y,
                                        int output_width, int output_height) {
  return Scene3D_ProjectWorldPoint(matrix, x + z * light_x, y + z * light_y,
                                   0.0f, output_width, output_height);
}

float Scene3D_ShadowFootprintScale(float z, float shrink) {
  if (z <= 0.0f || shrink <= 0.0f) return 1.0f;
  return 1.0f / (1.0f + z * shrink);
}

float Scene3D_AutoFitDistance(float fov_y) {
  return 0.5f / tanf(fov_y * 0.5f);
}

float Scene3D_ClipDepth(const float matrix[16], float x, float y, float z) {
  return matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
}

bool Scene3D_GroundDepthBoundaryY(const float matrix[16], float x,
                                  float minimum_depth, float *boundary_y,
                                  bool *increasing) {
  float slope = matrix[7];  /* depth gained per unit of ground y */
  if (slope == 0.0f) return false;
  float base = matrix[3] * x + matrix[15];
  if (boundary_y) *boundary_y = (minimum_depth - base) / slope;
  if (increasing) *increasing = slope > 0.0f;
  return true;
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
