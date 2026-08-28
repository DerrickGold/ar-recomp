#include "diorama.h"

#include <math.h>

#include "diorama_depth_shapes.h"
#include "scene3d_math.h"

bool Diorama_PlaneEligible(int plane, bool visible, bool has_texture,
                           bool has_pixels, bool hud_flat, bool skybox_only) {
  if (!visible || !has_texture || !has_pixels) return false;
  if (plane == SR_PPU_OVERLAY_BG3 && hud_flat) return false;
  if (skybox_only &&
      (plane == SR_PPU_OVERLAY_BG2 ||
       plane == kDioramaPlane_Bg2Hi ||
       plane == kDioramaPlane_Bg2Far ||
       plane == kDioramaPlane_Backdrop))
    return false;
  return true;
}

bool Diorama_PlaneProjectable(int plane, bool visible, bool has_texture,
                              bool has_pixels, bool has_attached_effect,
                              bool hud_flat, bool skybox_only) {
  const bool accepts_attached_effect =
      DioramaPlaneIsObjectPriority(plane) ||
      plane == SR_PPU_OVERLAY_BG1 ||
      plane == SR_PPU_OVERLAY_BG2 ||
      plane == kDioramaPlane_Bg1Hi;
  const bool has_effect_content =
      has_attached_effect && accepts_attached_effect;
  return Diorama_PlaneEligible(
      plane, visible, has_texture || has_effect_content,
      has_pixels || has_effect_content,
      hud_flat, skybox_only);
}

uint8_t Diorama_FilterObjEffectProjectionMask(
    uint8_t required_priorities, uint32_t requested_planes,
    uint32_t content_planes, uint32_t uploaded_planes) {
  uint8_t filtered = 0;
  for (unsigned priority = 0;
       priority < kDioramaObjectPriorityCount; priority++) {
    const uint8_t priority_bit = (uint8_t)(1u << priority);
    if (!(required_priorities & priority_bit)) continue;
    const int plane = DioramaPlaneForObjectPriority(priority);
    if (plane < 0) continue;
    const uint32_t plane_bit = 1u << (unsigned)plane;
    if (!(requested_planes & plane_bit)) continue;
    if ((content_planes & plane_bit) && !(uploaded_planes & plane_bit))
      continue;
    filtered |= priority_bit;
  }
  return filtered;
}

uint32_t Diorama_FilterBgEffectProjectionMask(
    uint32_t required_planes, uint32_t requested_planes,
    uint32_t content_planes, uint32_t uploaded_planes) {
  const uint32_t valid_planes =
      (1u << SR_PPU_OVERLAY_BG1) |
      (1u << SR_PPU_OVERLAY_BG2) |
      (1u << kDioramaPlane_Bg1Hi);
  const uint32_t failed_content = content_planes & ~uploaded_planes;
  return required_planes & valid_planes & requested_planes & ~failed_content;
}

static bool ProjectCapturedPlanePoint(
    const DioramaProjection *projection, float capture_x, float capture_y,
    const DioramaPlaneProjection *plane, ArRenderPointF *point,
    float *scale_x, float *scale_y) {
  /* A published valid projection already guarantees non-zero texture/output
   * dimensions; public entry points own pointer validation once per call. */
  if (!projection->valid || !plane->valid)
    return false;
  float du = plane->u1 - plane->u0;
  float dv = plane->v1 - plane->v0;
  if (du == 0.0f || dv == 0.0f) return false;

  ArRenderPointF projected[3];
  int sample_count = (scale_x || scale_y) ? 3 : 1;
  for (int sample = 0; sample < sample_count; sample++) {
    float x = capture_x + (sample == 1 ? 1.0f : 0.0f);
    float y = capture_y + (sample == 2 ? 1.0f : 0.0f);
    float u = (x + (float)projection->texture_x_origin) /
        (float)projection->texture_width;
    float v = y / (float)projection->texture_height;
    float s = (u - plane->u0) / du;
    float t = (v - plane->v0) / dv;
    float wx = (s - 0.5f) * projection->aspect_x;
    float wy = (0.5f - t) * projection->height_scale;
    float wz = DioramaTiltedRowDepth(
        plane->z_world, plane->rake, plane->bow, t);
    if (plane->overflow_valid && t > plane->overflow_fold_t &&
        plane->overflow_height > 0.0f) {
      const float overflow_t =
          (t - plane->overflow_fold_t) * projection->height_scale /
          plane->overflow_height;
      const float y_top =
          (0.5f - plane->overflow_fold_t) * projection->height_scale;
      const float z_top = DioramaTiltedRowDepth(
          plane->z_world, plane->rake, plane->bow,
          plane->overflow_fold_t);
      DioramaOverflowFoldPoint(
          overflow_t, y_top, z_top, plane->overflow_handoff_z,
          plane->overflow_height, plane->overflow_overlap_t,
          plane->overflow_front_z, plane->overflow_front_drop,
          &wy, &wz);
    }
    Scene3DPoint projected_point;
    if (!Scene3D_ProjectWorldPoint(
            projection->matrix, wx, wy, wz,
            projection->output_width, projection->output_height,
            &projected_point))
      return false;
    projected[sample] = (ArRenderPointF){
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
                                  unsigned obj_priority, ArRenderPointF *point,
                                  float *scale_x, float *scale_y) {
  if (!projection || !point ||
      obj_priority >= kDioramaObjectPriorityCount) return false;
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->object_planes[obj_priority], point, scale_x, scale_y);
}

bool Diorama_ProjectCapturedBg1Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     ArRenderPointF *point,
                                     float *scale_x, float *scale_y) {
  if (!projection || !point) return false;
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->bg1_plane, point, scale_x, scale_y);
}

bool Diorama_ProjectCapturedBg1HighPoint(
    const DioramaProjection *projection,
    float capture_x, float capture_y, ArRenderPointF *point,
    float *scale_x, float *scale_y) {
  if (!projection || !point) return false;
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->bg1_high_plane, point, scale_x, scale_y);
}

bool Diorama_ProjectCapturedBg2Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     ArRenderPointF *point,
                                     float *scale_x, float *scale_y) {
  if (!projection || !point) return false;
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->bg2_plane, point, scale_x, scale_y);
}
