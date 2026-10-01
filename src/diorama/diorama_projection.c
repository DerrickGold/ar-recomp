#include "diorama.h"

#include <math.h>

#include "constants.h"
#include "diorama_depth_shapes.h"
#include "render/scene3d_math.h"

static bool CameraProjectionValid(
    const float matrix[16], float aspect_x, float height_scale,
    float z_world, float rake, float bow) {
  if (!matrix || !isfinite(aspect_x) || aspect_x <= 0.0f ||
      !isfinite(height_scale) || height_scale <= 0.0f ||
      !isfinite(z_world) || !isfinite(rake) || !isfinite(bow))
    return false;
  for (int i = 0; i < 16; i++)
    if (!isfinite(matrix[i])) return false;
  return true;
}

static void CameraShiftVertically(float matrix[16], float shift) {
  /* A clip-space translation gives every depth the same screen displacement. */
  const float clip_shift = -2.0f * shift;
  for (int c = 0; c < 4; c++)
    matrix[c * 4 + 1] += clip_shift * matrix[c * 4 + 3];
}

/* A projected triangle reaches its Y extrema at its vertices while entirely
 * in front of the camera. Across each mesh row only the two side vertices are
 * needed. At an authentic-band boundary between rows, interpolate the rendered
 * mesh's depth rather than resampling its underlying curve. */
static bool CameraVerticalBounds(
    const float matrix[16], float aspect_x, float height_scale,
    float z_world, float rake, float bow,
    float t0, float t1, float *top, float *bottom) {
  const int subdiv_y = kDioramaPlaneSubdivY;
  *top = INFINITY;
  *bottom = -INFINITY;
  for (int row = -1; row <= subdiv_y + 1; row++) {
    const float t = row < 0 ? t0 : row > subdiv_y ? t1
        : (float)row / (float)subdiv_y;
    if (t < t0 || t > t1) continue;
    float z;
    if (row >= 0 && row <= subdiv_y) {
      z = DioramaTiltedRowDepth(z_world, rake, bow, t);
    } else {
      const int lower = (int)floorf(t * (float)subdiv_y);
      const int upper = lower < subdiv_y ? lower + 1 : lower;
      const float a = DioramaTiltedRowDepth(
          z_world, rake, bow, (float)lower / (float)subdiv_y);
      const float b = DioramaTiltedRowDepth(
          z_world, rake, bow, (float)upper / (float)subdiv_y);
      z = a + (b - a) * (t * (float)subdiv_y - (float)lower);
    }
    for (int side = 0; side < 2; side++) {
      Scene3DPoint p;
      /* A unit viewport gives normalized screen coordinates, independent of
       * resolution, pixel aspect, or the viewport's output origin. */
      if (!Scene3D_ProjectWorldPoint(
              matrix, ((float)side - 0.5f) * aspect_x,
              (0.5f - t) * height_scale, z, 1, 1, &p))
        return false;
      *top = fminf(*top, p.y);
      *bottom = fmaxf(*bottom, p.y);
    }
  }
  return true;
}

bool Diorama_CenterCameraVertically(
    float matrix[16], float aspect_x, float height_scale,
    float z_world, float rake, float bow,
    float authentic_t0, float authentic_t1) {
  if (!CameraProjectionValid(matrix, aspect_x, height_scale, z_world, rake, bow) ||
      !isfinite(authentic_t0) || !isfinite(authentic_t1) ||
      authentic_t0 < 0.0f || authentic_t1 > 1.0f ||
      authentic_t0 >= authentic_t1)
    return false;
  float top, bottom;
  if (!CameraVerticalBounds(matrix, aspect_x, height_scale,
                           z_world, rake, bow,
                           authentic_t0, authentic_t1, &top, &bottom))
    return false;
  const float shift = 0.5f - 0.5f * (top + bottom);

  /* Clip Y += offset * clip W is a uniform screen translation after the
   * perspective divide. Apply it to the shared matrix so every depth plane,
   * skirt, aperture and attached effect receives exactly the same shift. */
  CameraShiftVertically(matrix, shift);
  return true;
}

bool Diorama_AlignCaptureToNativeCamera(
    float matrix[16], int capture_height, int authentic_y0) {
  if (!matrix || capture_height < kActRaiserAuthenticHeight ||
      authentic_y0 < 0 ||
      authentic_y0 > capture_height - kActRaiserAuthenticHeight)
    return false;
  for (int i = 0; i < 16; i++)
    if (!isfinite(matrix[i])) return false;
  const float offset = ((float)authentic_y0 +
      0.5f * (float)kActRaiserAuthenticHeight - 0.5f * (float)capture_height) /
      (float)kActRaiserAuthenticHeight;
  /* Translate the shared world origin before projection. A screen-space
   * correction at BG1 depth alone would still let BG2 and OBJ drift as rows
   * transfer between margins, especially while the camera is tilted. */
  for (int row = 0; row < 4; row++)
    matrix[12 + row] += offset * matrix[4 + row];
  return true;
}

DioramaVerticalBounds DioramaVerticalBounds_Resolve(
    int plane, int camera_y, int world_height,
    int authentic_y0, int capture_height) {
  DioramaVerticalBounds bounds = {0};
  if ((plane != SR_PPU_OVERLAY_BG1 && plane != SR_PPU_OVERLAY_BG2) ||
      world_height < kActRaiserActionCameraViewportHeight ||
      camera_y < 0 || camera_y > world_height - kActRaiserActionCameraViewportHeight ||
      authentic_y0 < 0 || authentic_y0 > camera_y ||
      capture_height < kActRaiserAuthenticHeight ||
      authentic_y0 > capture_height - kActRaiserAuthenticHeight)
    return bounds;
  const int bottom_rows =
      capture_height - authentic_y0 - kActRaiserAuthenticHeight;
  const int available_bottom =
      world_height - kActRaiserActionCameraViewportHeight - camera_y;
  if (bottom_rows > available_bottom) return bounds;
  bounds.valid = true;
  bounds.plane = plane;
  bounds.top_reached = authentic_y0 == camera_y;
  bounds.bottom_reached = bottom_rows == available_bottom;
  return bounds;
}

static float CameraEdgeCorrection(float distance) {
  /* Start easing while the finite edge still lies outside the viewport.
   * This smooth positive part stays above the required hard correction, so
   * slowing into the stop never exposes beyond the level. It joins both
   * ordinary following and the pinned edge with continuous velocity. */
  const float ease = 8.0f / (float)kActRaiserAuthenticHeight;
  if (distance <= -ease) return 0.0f;
  if (distance >= ease) return distance;
  const float blend = distance + ease;
  return blend * blend / (4.0f * ease);
}

DioramaHorizontalBounds DioramaHorizontalBounds_Resolve(
    int plane, int camera_x, int world_x0, int world_width,
    int capture_width, int apron) {
  DioramaHorizontalBounds bounds = {0};
  const int64_t camera = (int64_t)camera_x - world_x0;
  if ((plane != SR_PPU_OVERLAY_BG1 && plane != SR_PPU_OVERLAY_BG2) ||
      world_width < kActRaiserAuthenticWidth || camera < 0 ||
      camera > world_width - kActRaiserAuthenticWidth ||
      capture_width < kActRaiserAuthenticWidth || apron < 0)
    return bounds;
  const float margin = 0.5f * (capture_width - kActRaiserAuthenticWidth);
  const float left = margin - (float)camera;
  const float right = left + world_width;
  bounds.valid = true;
  bounds.plane = plane;
  bounds.left = left / capture_width;
  bounds.right = right / capture_width;
  bounds.left_reached = left >= -apron;
  bounds.right_reached = right <= (float)capture_width + apron;
  return bounds;
}

bool Diorama_ClampCameraHorizontally(
    float matrix[16], float aspect_x, float height_scale,
    float z_world, float rake, float bow,
    const DioramaHorizontalBounds *bounds) {
  if (!bounds || !bounds->valid || !isfinite(bounds->left) ||
      !isfinite(bounds->right) || bounds->left >= bounds->right ||
      !CameraProjectionValid(matrix, aspect_x, height_scale, z_world, rake, bow))
    return false;
  float lower = -INFINITY, upper = INFINITY;
  for (int edge = 0; edge < 2; ++edge) {
    if (edge ? !bounds->right_reached : !bounds->left_reached) continue;
    const float x = ((edge ? bounds->right : bounds->left) - 0.5f) * aspect_x;
    /* Each side is a piecewise-linear mesh edge. Its projected X extrema
     * occur at row vertices, including the rake/bow extrema between ends. */
    for (int row = 0; row <= kDioramaPlaneSubdivY; ++row) {
      const float t = (float)row / kDioramaPlaneSubdivY;
      Scene3DPoint p;
      if (!Scene3D_ProjectWorldPoint(matrix, x, (0.5f - t) * height_scale,
              DioramaTiltedRowDepth(z_world, rake, bow, t), 1, 1, &p))
        return false;
      if (edge) lower = fmaxf(lower, 1.0f - p.x);
      else upper = fminf(upper, -p.x);
    }
  }
  if (lower > upper) return true;
  const float shift = fmaxf(lower, fminf(0.0f, upper));
  if (shift == 0.0f) return true;
  for (int c = 0; c < 4; ++c)
    matrix[c * 4] += 2.0f * shift * matrix[c * 4 + 3];
  return true;
}

bool Diorama_ClampCameraVertically(
    float matrix[16], float aspect_x, float height_scale,
    float z_world, float rake, float bow,
    bool clamp_top, bool clamp_bottom, float *world_y_offset) {
  if (world_y_offset) *world_y_offset = 0.0f;
  if (!CameraProjectionValid(matrix, aspect_x, height_scale, z_world, rake, bow))
    return false;
  if (!clamp_top && !clamp_bottom) return true;
  float lower = -INFINITY, upper = INFINITY;
  for (int edge = 0; edge < 2; edge++) {
    if (edge ? !clamp_bottom : !clamp_top) continue;
    const float t = (float)edge;
    const float target = edge ? -1.0f : 1.0f;
    const float slope = matrix[5] - target * matrix[7];
    if (slope <= 0.0f) return false;
    for (int side = 0; side < 2; side++) {
      Scene3DClipPoint point;
      if (!Scene3D_TransformToClip(
              matrix, ((float)side - 0.5f) * aspect_x,
              (0.5f - t) * height_scale,
              DioramaTiltedRowDepth(z_world, rake, bow, t), &point) ||
          point.w <= kScene3DMinimumProjectionDepth)
        return false;
      const float shift = (target * point.w - point.y) / slope;
      if (edge) upper = fminf(upper, shift);
      else lower = fmaxf(lower, shift);
    }
  }
  /* A small room or deliberate zoom-out cannot satisfy both edges by moving
   * the camera alone. Keep its existing fit rather than stretching the art. */
  if (lower > upper) return true;
  float shift = clamp_top ? CameraEdgeCorrection(lower) : 0.0f;
  if (clamp_bottom) shift -= CameraEdgeCorrection(-upper);
  shift = fmaxf(lower, fminf(shift, upper));
  Diorama_TranslateCameraWorldY(matrix, shift);
  if (world_y_offset) *world_y_offset = shift;
  return true;
}

void Diorama_TranslateCameraWorldY(float matrix[16], float shift) {
  for (int row = 0; row < 4; row++)
    matrix[12 + row] += shift * matrix[4 + row];
}

bool Diorama_OffsetCamera(float matrix[16], int x, int y, float pixel_aspect) {
  if (!matrix || !isfinite(pixel_aspect) || pixel_aspect <= 0.0f ||
      x < -64 || x > 64 || y < -64 || y > 64)
    return false;
  for (int i = 0; i < 16; i++)
    if (!isfinite(matrix[i])) return false;
  const float dx = -(float)x * pixel_aspect / kActRaiserAuthenticHeight;
  const float dy = (float)y / kActRaiserAuthenticHeight;
  for (int row = 0; row < 4; row++)
    matrix[12 + row] += dx * matrix[row] + dy * matrix[4 + row];
  return true;
}

static float BackgroundCameraAt(float camera_y, uint8_t ratio, int world_height) {
  const unsigned denominator = ratio & 15u;
  float camera = denominator ? camera_y *
      (float)(ratio >> 4) / (float)denominator : 0.0f;
  /* Match the native command-3 parallax policy. Small maps may wrap; only
   * maps of at least 0x300 pixels have a native finite-camera stop. */
  if (world_height >= 0x300)
    camera = fminf(camera, (float)(world_height - kActRaiserAuthenticHeight));
  return camera;
}

float Diorama_BackgroundClampOffset(
    int camera_y, int bg_camera_y, uint8_t ratio, int world_height,
    float world_y_offset) {
  if (!isfinite(world_y_offset)) return 0.0f;
  const float native = BackgroundCameraAt((float)camera_y, ratio, world_height);
  const float framed = BackgroundCameraAt(
      (float)camera_y + world_y_offset * kActRaiserAuthenticHeight,
      ratio, world_height);
  float delta = framed - native;
  /* Remove the native ratio's integer staircase when it describes this
   * captured camera. Preserve any separately authored BG camera offset. */
  const int native_integer = (int)floorf(native);
  if ((native_integer & 0x3ff) == bg_camera_y)
    delta += native - (float)native_integer;
  return delta / (float)kActRaiserAuthenticHeight - world_y_offset;
}

bool Diorama_PlaneEligible(int plane, bool visible, bool has_texture,
                           bool has_pixels, bool hud_flat, bool skybox_only,
                           uint32_t additive_plane_mask) {
  if (!visible || !has_texture || !has_pixels) return false;
  if (plane == SR_PPU_OVERLAY_BG3 && hud_flat) return false;
  /* Skybox-only replaces the distant backdrop, not BG2's foreground water
   * and other high-priority scenery. Those retain their native depth, alpha
   * and attached effects (including when their low-priority band is empty).
   * In disjoint full-add scenes such as Marahna, main-screen BG2 is instead
   * the color base for the subscreen scenery/actors. Replacing it with ROM
   * skybox art drops the water tint and adds the scenery to itself. Keep that
   * base at its authored transform; an additive BG2 is not a main input. */
  const uint32_t bg2_planes = (1u << SR_PPU_OVERLAY_BG2) |
      (1u << kDioramaPlane_Bg2Hi) | (1u << kDioramaPlane_Bg2Far);
  const bool bg2_color_base = additive_plane_mask &&
      !(additive_plane_mask & bg2_planes);
  if (skybox_only &&
      (plane == kDioramaPlane_Backdrop ||
       (!bg2_color_base && (plane == SR_PPU_OVERLAY_BG2 ||
                           plane == kDioramaPlane_Bg2Far))))
    return false;
  return true;
}

bool Diorama_PlaneProjectable(int plane, bool visible, bool has_texture,
                              bool has_pixels, bool has_attached_effect,
                              bool hud_flat, bool skybox_only,
                              uint32_t additive_plane_mask) {
  const bool accepts_attached_effect =
      DioramaPlaneIsObjectPriority(plane) ||
      plane == SR_PPU_OVERLAY_BG1 ||
      plane == SR_PPU_OVERLAY_BG2 ||
      plane == kDioramaPlane_Bg1Hi || plane == kDioramaPlane_Bg2Hi;
  const bool has_effect_content =
      has_attached_effect && accepts_attached_effect;
  return Diorama_PlaneEligible(
      plane, visible, has_texture || has_effect_content,
      has_pixels || has_effect_content,
      hud_flat, skybox_only, additive_plane_mask);
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
      (1u << kDioramaPlane_Bg1Hi) | (1u << kDioramaPlane_Bg2Hi);
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
    float x = capture_x + plane->capture_offset.x + (sample == 1 ? 1.0f : 0.0f);
    float y = capture_y + plane->capture_offset.y + (sample == 2 ? 1.0f : 0.0f);
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
            projection->matrix, wx, wy + plane->world_y_offset, wz,
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

bool Diorama_ProjectCapturedBg2HighPoint(
    const DioramaProjection *projection,
    float capture_x, float capture_y, ArRenderPointF *point,
    float *scale_x, float *scale_y) {
  if (!projection || !point) return false;
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->bg2_high_plane, point, scale_x, scale_y);
}

static bool ValidSkyboxBand(const DioramaSkyboxBandProjection *band) {
  return isfinite(band->x0) && isfinite(band->x1) &&
      isfinite(band->y0) && isfinite(band->y1) &&
      isfinite(band->output_y0) && isfinite(band->output_y1) &&
      band->x1 > band->x0 && band->y1 > band->y0 &&
      band->output_y0 >= 0 && band->output_y1 <= 1 &&
      band->output_y1 > band->output_y0;
}

bool Diorama_SkyboxCaptureBounds(const DioramaProjection *projection,
                                 float *x0, float *y0, float *x1, float *y1) {
  if (!projection || !projection->valid || !x0 || !y0 || !x1 || !y1)
    return false;
  const DioramaSkyboxProjection *sky = &projection->bg2_skybox;
  if (!sky->count || sky->count > kDioramaBgMaxValidSpans ||
      sky->active_band < -1 || sky->active_band >= (int)sky->count)
    return false;
  const unsigned first = sky->active_band < 0 ? 0 : (unsigned)sky->active_band;
  const unsigned end = sky->active_band < 0 ? sky->count : first + 1;
  float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
  for (unsigned i = first; i < end; i++) {
    const DioramaSkyboxBandProjection *band = &sky->bands[i];
    if (!ValidSkyboxBand(band)) return false;
    left = fminf(left, band->x0);
    right = fmaxf(right, band->x1);
    top = fminf(top, band->y0);
    bottom = fmaxf(bottom, band->y1);
  }
  *x0 = left; *y0 = top; *x1 = right; *y1 = bottom;
  return true;
}

static int SkyboxBandAt(const DioramaSkyboxProjection *sky, float y) {
  if (!sky->count || sky->count > kDioramaBgMaxValidSpans ||
      sky->active_band < -1 || sky->active_band >= (int)sky->count || !isfinite(y))
    return -1;
  int selected = 0;
  float distance = INFINITY;
  for (unsigned i = 0; i < sky->count; i++) {
    const DioramaSkyboxBandProjection *band = &sky->bands[i];
    if (!ValidSkyboxBand(band)) return -1;
    const float d = fmaxf(0, fmaxf(band->y0 - y, y - band->y1));
    if (d < distance) {
      distance = d;
      selected = (int)i;
    }
  }
  return selected;
}

bool Diorama_SkyboxAnchorBounds(const DioramaProjection *projection, float anchor_y,
                                float *x0, float *y0, float *x1, float *y1) {
  if (!projection || !projection->valid || !x0 || !y0 || !x1 || !y1) return false;
  const DioramaSkyboxProjection *sky = &projection->bg2_skybox;
  const int selected = SkyboxBandAt(sky, anchor_y);
  if (selected < 0) return false;
  const DioramaSkyboxBandProjection *source = &sky->bands[selected];
  const unsigned first = sky->active_band < 0 ? 0 : (unsigned)sky->active_band;
  const unsigned end = sky->active_band < 0 ? sky->count : first + 1;
  float top = INFINITY, bottom = -INFINITY;
  for (unsigned i = first; i < end; i++) {
    top = fminf(top, sky->bands[i].output_y0);
    bottom = fmaxf(bottom, sky->bands[i].output_y1);
  }
  const float height = (source->y1 - source->y0) /
      (source->output_y1 - source->output_y0);
  *x0 = source->x0;
  *x1 = source->x1;
  *y0 = source->y0 + (top - source->output_y0) * height;
  *y1 = source->y0 + (bottom - source->output_y0) * height;
  return true;
}

static bool ProjectSkyboxBandPoint(const DioramaProjection *projection,
    const DioramaSkyboxBandProjection *band, float x, float y,
    ArRenderPointF *point, float *scale_x, float *scale_y) {
  if (!projection->valid || !ValidSkyboxBand(band) || !point ||
      projection->output_width <= 0 || projection->output_height <= 0 ||
      !isfinite(x) || !isfinite(y)) return false;
  const float sx = projection->output_width / (band->x1 - band->x0);
  const float sy = projection->output_height *
      (band->output_y1 - band->output_y0) / (band->y1 - band->y0);
  *point = (ArRenderPointF){
      projection->output_x + (x - band->x0) * sx,
      projection->output_y + band->output_y0 * projection->output_height +
          (y - band->y0) * sy};
  if (scale_x) *scale_x = sx;
  if (scale_y) *scale_y = sy;
  return true;
}

bool Diorama_ProjectSkyboxAnchorPoint(const DioramaProjection *projection, float anchor_y,
                                      float x, float y, ArRenderPointF *point) {
  if (!projection || !isfinite(anchor_y)) return false;
  if (projection->bg2_skybox.world_plane.valid)
    return ProjectCapturedPlanePoint(projection, x, y,
        &projection->bg2_skybox.world_plane, point, NULL, NULL);
  const int selected = SkyboxBandAt(&projection->bg2_skybox, anchor_y);
  return selected >= 0 && ProjectSkyboxBandPoint(
      projection, &projection->bg2_skybox.bands[selected], x, y, point, NULL, NULL);
}

static bool ProjectSkyboxPoint(const DioramaProjection *projection,
    float x, float y, ArRenderPointF *point, float *scale_x, float *scale_y) {
  const DioramaSkyboxProjection *sky = &projection->bg2_skybox;
  const int nearest = SkyboxBandAt(sky, y);
  if (nearest < 0) return false;
  /* Direction probes may extrapolate beyond the visible rectangle. Extend
   * the nearest band; geometry itself is clipped before projection. */
  const int selected = sky->active_band < 0 ? nearest : sky->active_band;
  return ProjectSkyboxBandPoint(
      projection, &sky->bands[selected], x, y, point, scale_x, scale_y);
}

bool Diorama_ProjectCapturedBg2Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     ArRenderPointF *point,
                                     float *scale_x, float *scale_y) {
  if (!projection || !point) return false;
  if (projection->bg2_skybox.world_plane.valid)
    return ProjectCapturedPlanePoint(projection, capture_x, capture_y,
        &projection->bg2_skybox.world_plane, point, scale_x, scale_y);
  if (projection->bg2_skybox.count)
    return ProjectSkyboxPoint(projection, capture_x, capture_y,
                               point, scale_x, scale_y);
  return ProjectCapturedPlanePoint(
      projection, capture_x, capture_y,
      &projection->bg2_plane, point, scale_x, scale_y);
}
