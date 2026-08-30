#ifndef DIORAMA_EFFECT_BACKEND_H
#define DIORAMA_EFFECT_BACKEND_H

#include <stdbool.h>

#include "render/render_device.h"

/* Optional compositor polish supplied by the active platform backend. The
 * game owns effect policy and semantic parameters; the backend owns shader
 * formats, native render states, and their lifetime. A backend may decline
 * any effect and the compositor must retain its ordinary draw path. */
typedef enum DioramaEffectKind {
  kDioramaEffect_Blur,
  kDioramaEffect_RimLight,
  kDioramaEffect_DofEdge,
  kDioramaEffect_Count,
} DioramaEffectKind;

typedef struct DioramaBlurEffectParams {
  float texel_width;
  float texel_height;
  float radius;
} DioramaBlurEffectParams;

typedef struct DioramaRimLightEffectParams {
  float texel_width;
  float texel_height;
  float strength;
} DioramaRimLightEffectParams;

typedef struct DioramaDofEdgeEffectParams {
  float texel_width;
  float texel_height;
  float blur_radius;
  float u_min;
  float u_max;
  float v_min;
  float v_max;
  /* Deprecated shader-blob ABI slot. The compositor always supplies zero;
   * screen-space edge coverage is geometry-owned now. */
  float edge_feather;
  float lower_content_v_max;
} DioramaDofEdgeEffectParams;

/* Availability may lazily create backend resources. Successful Bind calls
 * affect subsequent submissions until Unbind and must be paired with it.
 * Failed binds do not promise a clean native state, so callers also Unbind
 * before continuing on the fallback path. */
bool DioramaEffectBackend_IsAvailable(ArRenderDevice *device,
                                      DioramaEffectKind effect);
bool DioramaEffectBackend_BindBlur(
    ArRenderDevice *device, const DioramaBlurEffectParams *params);
bool DioramaEffectBackend_BindRimLight(
    ArRenderDevice *device, const DioramaRimLightEffectParams *params);
bool DioramaEffectBackend_BindDofEdge(
    ArRenderDevice *device, const DioramaDofEdgeEffectParams *params);
bool DioramaEffectBackend_Unbind(ArRenderDevice *device);

/* Release native resources before their render device is destroyed or
 * replaced. Effects are recreated lazily on the next availability query. */
void DioramaEffectBackend_Reset(ArRenderDevice *device);

#endif /* DIORAMA_EFFECT_BACKEND_H */
