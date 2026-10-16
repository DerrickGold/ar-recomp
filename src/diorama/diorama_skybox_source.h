#ifndef AR_DIORAMA_SKYBOX_SOURCE_H
#define AR_DIORAMA_SKYBOX_SOURCE_H
#include "render/render_device.h"

/* Raw capture bands shared by native skybox sampling and effect projection.
 * std140-compatible; all values are captured before generated camera motion.
 * Refit on the GPU so entering/leaving raster bands use the same window. */
enum { kDioramaSkyboxSourceBands = 11, kDioramaSkyboxSourceMaxIndices = 4096 };
typedef struct DioramaSkyboxSourceMapping {
  float meta[4];     /* count, texture width/height, pixel aspect */
  float output[4];   /* output width/height, capture offset X/Y */
  float vertical[4]; /* capture low/high, texture low/high */
  float follow[4];   /* lower/upper clamp, camera delta, authentic row */
  float fit[4];      /* follow window height, reserved */
  float bands[kDioramaSkyboxSourceBands][4]; /* U low/high, raw row low/high */
} DioramaSkyboxSourceMapping;

/* Capture rows [y0, y1) of the drawn band, sampled `texels` further right. */
typedef struct DioramaSkyboxRowShift {
  float y0, y1, texels;
} DioramaSkyboxRowShift;

typedef struct DioramaSkyboxSourceDraw {
  const ArRenderVertex2D *vertices;
  const int32_t *indices;
  unsigned vertex_count, index_count;
  ArRenderBlendMode blend;
  int texture_width, texture_height;
  float radius;
  int motion_slot; /* -1 = static, 0 = BG1, 3 = BG2, 6 = private capture */
  bool periodic;
  /* Sample linearly whatever the texture's own filter: the blurred, magnified
   * sky then follows sub-texel band and camera motion instead of stepping a
   * whole source pixel (several output pixels) at a time. */
  bool linear;
  unsigned band;
  /* When set, the band draws as these row runs instead, each through the
   * spare slot bands[meta[0]] (same width, so the shared fit is unchanged).
   * Raster rows the band's single shift cannot follow use it. */
  const DioramaSkyboxRowShift *row_shifts;
  unsigned row_shift_count;
  DioramaSkyboxSourceMapping mapping;
} DioramaSkyboxSourceDraw;
#endif
