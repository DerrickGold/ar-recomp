#ifndef AR_ACTION_EFFECT_CAPTURE_H
#define AR_ACTION_EFFECT_CAPTURE_H
/* Action-effect capture adapter: gameplay timing, observation history,
 * diagnostic reports and the detected diorama room section. The underlying
 * classifiers in action_effects.c consume explicit WRAM views and tick deltas. */

typedef struct FrameSlot FrameSlot;

/* Called on the game thread after PPU drawing. Fills spell/scene effects,
 * effect settings and the diorama map/section key, and publishes that section
 * for the layer editor. Other FrameSlot fields are untouched. Captures still
 * observe identities when effect drawing is disabled. */
void ActionEffectCapture_CaptureFrame(FrameSlot *frame);

#endif /* AR_ACTION_EFFECT_CAPTURE_H */
