#ifndef AR_EFFECT_BATCH_H
#define AR_EFFECT_BATCH_H

#include "render/render_device.h"

/* Untextured enhancement geometry shared by scene presenters. The process-wide
 * support latches are read by settings and reset when the render device resets.
 * Feature eligibility and geometry construction stay with each feature. */
typedef struct EffectBatch {
  ArRenderVertex2D *vertices;
  int32_t *indices;
  int vertex_count, index_count;
  int vertex_capacity, index_capacity;
  bool overflow;
} EffectBatch;

bool EffectRenderer_Available(void);
void EffectRenderer_DisableBlend(ArRenderDevice *device, const char *operation);
bool EffectRenderer_Submit(ArRenderDevice *device, const EffectBatch *batch,
                           ArRenderBlendMode blend);
void EffectRenderer_Reset(void);

#endif /* AR_EFFECT_BATCH_H */
