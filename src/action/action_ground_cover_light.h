#ifndef AR_ACTION_GROUND_COVER_LIGHT_H
#define AR_ACTION_GROUND_COVER_LIGHT_H

#include "action_effects.h"
#include "render/render_types.h"

/* Unprojected samples from supported snow caps. Lighting uses the original
 * authored snow response at native world coordinates, before the renderer
 * mixes body/crest or applies the captured scene fade. */
typedef struct ActionGroundCoverLightSample {
  float native_x, native_floor_y, height;
  ArRenderColorF body, crest;
} ActionGroundCoverLightSample;

/* Shares the existing GroundCover incident-light implementation. One writer
 * per batch retains its light transport cache. No terrain or projection is
 * inferred here; ordinary GroundCover does not call this adapter. */
typedef bool (*ActionGroundCoverLightSamplesFn)(const ActionSceneEffectFrame *,
    const ActionEffectInstance *, bool lighting, bool atmosphere_unlit,
    ActionGroundCoverLightSample *, unsigned count);
bool ActionGroundCover_LightSamples(const ActionSceneEffectFrame *,
    const ActionEffectInstance *, bool lighting, bool atmosphere_unlit,
    ActionGroundCoverLightSample *, unsigned count);

#endif
