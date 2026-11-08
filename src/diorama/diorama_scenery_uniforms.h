#ifndef AR_DIORAMA_SCENERY_UNIFORMS_H
#define AR_DIORAMA_SCENERY_UNIFORMS_H

#include "diorama_effect_backend.h"
#include <string.h>

/* Shared std140 packing for SDL's 2D and shared-depth material adapters.
 * Dimensions describe the target addressed by gl_FragCoord; light_uv selects
 * the matching portion of the resident illumination map. */
enum { kDioramaSceneryUniformFloats = 180 };
typedef struct DioramaSceneryUniforms {
  float values[kDioramaSceneryUniformFloats];
} DioramaSceneryUniforms;

static inline void DioramaSceneryUniforms_Init(DioramaSceneryUniforms *out,
    const DioramaSceneryLightEffectParams *params, int width, int height) {
  const ArRenderRectF uv = params->light_uv.w > 0 ? params->light_uv : (ArRenderRectF){0,0,1,1};
  const ArRenderRectF source = params->attributes_uv.w > 0 ? params->attributes_uv : (ArRenderRectF){0,0,1,1};
  *out = (DioramaSceneryUniforms){{
    params->texel_width, params->texel_height, params->blur_radius, params->lower_content_v_max,
    params->priority.width, params->priority.height,
    params->priority.high_band, params->priority.additive,
    uv.w / width, uv.h / height, uv.x, uv.y,
    params->shade.r, params->shade.g, params->shade.b, params->mode,
    params->grade.strength, params->grade.saturation, 0, 0,
    ((params->grade.shadows>>16)&255)/255.f, ((params->grade.shadows>>8)&255)/255.f,
    (params->grade.shadows&255)/255.f, 0,
    ((params->grade.highlights>>16)&255)/255.f, ((params->grade.highlights>>8)&255)/255.f,
    (params->grade.highlights&255)/255.f, 0,
    source.x, source.y, source.w, source.h,
  }};
  _Static_assert(sizeof(DioramaHeat)==36*sizeof(float), "scenery heat std140 layout");
  _Static_assert(sizeof(DioramaCloudLight)==108*sizeof(float), "scenery cloud std140 layout");
  memcpy(out->values+32, &params->heat, sizeof(params->heat));
  memcpy(out->values+68,&params->cloud_light,sizeof(params->cloud_light));
  _Static_assert(sizeof(DioramaHeatOwned)==4*sizeof(float), "owned heat std140 layout");
  memcpy(out->values+176,&params->heat_owned,sizeof(params->heat_owned));
}

typedef struct DioramaActorUniforms { float values[16]; } DioramaActorUniforms;
static inline void DioramaActorUniforms_Init(DioramaActorUniforms *out,
    const DioramaSceneryLightEffectParams *params, int width, int height) {
  const ArRenderRectF uv = params->light_uv.w > 0 ? params->light_uv : (ArRenderRectF){0,0,1,1};
  const ArRenderRectF source = params->attributes_uv.w > 0 ? params->attributes_uv : (ArRenderRectF){0,0,1,1};
  *out = (DioramaActorUniforms){{
    params->texel_width, params->texel_height, params->blur_radius, params->rim_strength,
    uv.w/width, uv.h/height, uv.x, uv.y,
    source.x, source.y, source.w, source.h, params->lower_content_v_max, 0, 0, 0,
  }};
}

#endif
