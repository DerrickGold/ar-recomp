#ifndef AR_ACTION_EFFECT_MANIFEST_H
#define AR_ACTION_EFFECT_MANIFEST_H
#include "action_effects.h"
#include "action_effect_receivers.h"
/* Capture-thread ownership. Reload before publishing subsequent frames; old
 * frames retain their own tuning values and never refer to mutable recipes. */
bool ActionEffectManifest_Load(void);
bool ActionEffectManifest_HasAuthored(unsigned group,unsigned room);
bool ActionEffectManifest_NeedsBg1Mask(unsigned group,unsigned room);
const ActionReceiverLighting *ActionEffectManifest_PrepareReceivers(unsigned group,unsigned room,
    const uint8_t *ram,size_t size);
void ActionEffectManifest_Apply(unsigned group, unsigned room, uint16_t clock, const uint8_t *ram, size_t size, ActionSceneEffectFrame *frame);
#endif
