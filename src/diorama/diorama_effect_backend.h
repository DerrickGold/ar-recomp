#ifndef AR_DIORAMA_EFFECT_BACKEND_H
#define AR_DIORAMA_EFFECT_BACKEND_H

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
  kDioramaEffect_PrioritySurface,
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

/* Coplanar opaque BG priority bands retain separate draw slots while sharing
 * filtering coverage. The texture holds low above high at native resolution;
 * mesh UVs address one band. Both draws must use this effect as a pair.
 * Additive pairs sum coverage directly instead of conditioning source-over
 * alpha; they require the capture's disjoint subscreen winner masks. */
typedef struct DioramaPrioritySurfaceEffectParams {
  float width;
  float height;
  bool high_band;
  bool additive;
} DioramaPrioritySurfaceEffectParams;

/* Incoming light is stored at half strength in an RGBA8 target (range 0..2).
 * Ambient exposure remains in vertex RGB. The material shades original art
 * with ambient + local light, retaining alpha and the normal layer filtering. */
typedef struct DioramaSceneryLightEffectParams {
  ArRenderTexture illumination;
  ArRenderTexture attributes, enemy_illumination;
  ArRenderRectF attributes_uv;
  float rim_strength;
  float texel_width, texel_height, blur_radius, lower_content_v_max;
  DioramaPrioritySurfaceEffectParams priority;
  ArRenderColorF shade;
  ArRenderRectF light_uv; /* Region of the full-output map; zero means all. */
  unsigned mode; /* 0: ordinary texture, 1: DOF, 2: priority pair */
} DioramaSceneryLightEffectParams;
bool DioramaEffectBackend_BindSceneryLight(ArRenderDevice *device,
    const DioramaSceneryLightEffectParams *params);
bool DioramaEffectBackend_PrepareSceneryLight(ArRenderDevice *device);

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
bool DioramaEffectBackend_BindPrioritySurface(
    ArRenderDevice *device, const DioramaPrioritySurfaceEffectParams *params);
bool DioramaEffectBackend_Unbind(ArRenderDevice *device);

/* Release native resources before their render device is destroyed or
 * replaced. Effects are recreated lazily on the next availability query. */
void DioramaEffectBackend_Reset(ArRenderDevice *device);

#endif /* AR_DIORAMA_EFFECT_BACKEND_H */
