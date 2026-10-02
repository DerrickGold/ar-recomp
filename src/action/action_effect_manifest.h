#ifndef AR_ACTION_EFFECT_MANIFEST_H
#define AR_ACTION_EFFECT_MANIFEST_H
#include "action_effects.h"
#include "action_effect_receivers.h"
/* Capture-thread ownership. Reload before publishing subsequent frames; old
 * frames retain their own tuning values and never refer to mutable recipes. */
void ActionEffectManifest_SurfaceFields(unsigned,unsigned,const ActionSurfaceField **);
bool ActionEffectManifest_Load(void);
bool ActionEffectManifest_NeedsBg2Mask(unsigned group,unsigned room);
bool ActionEffectManifest_ReplacesRayField(unsigned group,unsigned room);
bool ActionEffectManifest_ReplacesGlowField(unsigned,unsigned);
bool ActionEffectManifest_ReplacesCastleField(unsigned group,unsigned room);
bool ActionEffectManifest_ReplacesMarshField(unsigned group,unsigned room);
bool ActionEffectManifest_ReplacesMoonField(unsigned group,unsigned room);
bool ActionEffectManifest_ReplacesWaterField(unsigned group,unsigned room);
bool ActionEffectManifest_ReplacesAtmosphereField(unsigned group,unsigned room);
bool ActionEffectManifest_NeedsBg1Mask(unsigned group,unsigned room);
const ActionReceiverLighting *ActionEffectManifest_PrepareReceivers(unsigned group,unsigned room,
    const uint8_t *ram,size_t size);
void ActionEffectManifest_Apply(unsigned group, unsigned room, uint16_t clock, const uint8_t *ram, size_t size, ActionSceneEffectFrame *frame);
#endif
