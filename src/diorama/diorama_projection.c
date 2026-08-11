#include "diorama.h"

#include <math.h>

#include "diorama_depth_shapes.h"
#include "scene3d_math.h"

static bool ProjectCapturedPlanePoint(
    const DioramaProjection *projection, float capture_x, float capture_y,
    const DioramaObjectPlaneProjection *plane, SDL_FPoint *point,
    float *scale_x, float *scale_y) {
  if (!projection || !projection->valid || !point || !plane || !plane->valid ||
      projection->texture_width <= 0 || projection->texture_height <= 0 ||
      projection->output_width <= 0 || projection->output_height <= 0)
    return false;
  float du = projection->object_u1 - projection->object_u0;
  float dv = projection->object_v1 - projection->object_v0;
  if (du == 0.0f || dv == 0.0f) return false;

  SDL_FPoint projected[3];
  int sample_count = (scale_x || scale_y) ? 3 : 1;
  for (int sample = 0; sample < sample_count; sample++) {
    float x = capture_x + (sample == 1 ? 1.0f : 0.0f);
    float y = capture_y + (sample == 2 ? 1.0f : 0.0f);
    float u = (x + (float)projection->texture_x_origin) /
        (float)projection->texture_width;
    float v = y / (float)projection->texture_height;
    float s = (u - projection->object_u0) / du;
    float t = (v - projection->object_v0) / dv;
    float wx = (s - 0.5f) * projection->aspect_x;
    float wy = (0.5f - t) * projection->height_scale;
    float wz = DioramaTiltedRowDepth(
        plane->z_world, plane->rake, plane->bow, t);
    Scene3DPoint projected_point;
    if (!Scene3D_ProjectWorldPoint(
            projection->matrix, wx, wy, wz,
            projection->output_width, projection->output_height,
            &projected_point))
      return false;
    projected[sample] = (SDL_FPoint){
      (float)projection->output_x + projected_point.x,
      (float)projection->output_y + projected_point.y,
    };
  }
  *point = projected[0];
  if (scale_x)
    *scale_x = hypotf(projected[1].x - projected[0].x,
                      projected[1].y - projected[0].y);
  if (scale_y)
    *scale_y = hypotf(projected[2].x - projected[0].x,
                      projected[2].y - projected[0].y);
  return true;
}

bool Diorama_ProjectCapturedPoint(const DioramaProjection *projection,
                                  float capture_x, float capture_y,
                                  unsigned obj_priority, SDL_FPoint *point,
                                  float *scale_x, float *scale_y) {
  if (!projection || obj_priority >= kDioramaObjectPriorityCount) return false;
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->object_planes[obj_priority], point, scale_x, scale_y);
}

bool Diorama_ProjectCapturedBg1Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     SDL_FPoint *point,
                                     float *scale_x, float *scale_y) {
  if (!projection) return false;
  return ProjectCapturedPlanePoint(projection, capture_x, capture_y,
                                   &projection->bg1_plane, point,
                                   scale_x, scale_y);
}
