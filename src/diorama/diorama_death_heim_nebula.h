#ifndef AR_DIORAMA_DEATH_HEIM_NEBULA_H
#define AR_DIORAMA_DEATH_HEIM_NEBULA_H

#include <math.h>
#include <stdbool.h>

/* A source window, not a new sky texture: the final arena's native opposed
 * horizontal waves remain in the captured image. Its nonperiodic page bottom
 * must not become an atmospheric edge inside the rotated viewport. */
typedef struct DioramaDeathHeimNebulaWindow {
  float u0, v0, u1, v1;
  float depths[4]; /* Shared inverse-plane preflight: TL,TR,BL,BR. */
} DioramaDeathHeimNebulaWindow;

static inline bool DioramaDeathHeimNebula_Window(int width, int height,
    int authentic_y0, int apron, int visible_width, float motion_x,
    float motion_y, DioramaDeathHeimNebulaWindow *out) {
  if (!out || width <= 0 || width > 640 || height < 224 || height > 352 ||
      authentic_y0 < 0 || authentic_y0 > height - 224 || apron < 0 ||
      apron > (640 - width) / 2 || visible_width <= 0 || visible_width > width ||
      !isfinite(motion_x) || !isfinite(motion_y)) return false;
  const float x0 = .5f * (width - visible_width) - motion_x;
  const float y0 = authentic_y0 - motion_y;
  /* A generated capture may translate its pixels. Use only its real current
   * window; an invalid/unsupported offset falls back to the existing layer. */
  if (x0 < 0 || x0 + visible_width > width || y0 < 0 || y0 + 224 > height)
    return false;
  *out = (DioramaDeathHeimNebulaWindow){
    (apron + x0) / 640.f, y0 / 352.f,
    (apron + x0 + visible_width) / 640.f, (y0 + 224) / 352.f, {0}};
  return true;
}
#endif
