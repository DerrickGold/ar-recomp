#ifndef AR_ACTION_EFFECT_PREVIEW_H
#define AR_ACTION_EFFECT_PREVIEW_H
#include "action_effects.h"
/* Explicit visual events for authoring. No WRAM, AI, collision or native object
 * allocation: only an anchor, clock, seed and one documented visual phase. */
typedef struct ActionEffectPreviewEvent {
  uint16_t kind,start,duration;
  int16_t x,y,velocity_x,velocity_y;
  uint32_t seed;
} ActionEffectPreviewEvent;
unsigned ActionEffectPreview_Count(void);
unsigned ActionEffectPreview_Kind(unsigned index);
bool ActionEffectPreview_Build(const ActionEffectPreviewEvent *event,uint16_t clock,ActionEffectInstance *out);
#endif
