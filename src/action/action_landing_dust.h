#ifndef AR_ACTION_LANDING_DUST_H
#define AR_ACTION_LANDING_DUST_H
/* Read-only contact observation for Fillmore's temple landing clouds. */
#include "action_effects.h"
#include "action_bg_world.h"

void ActionLandingDust_Capture(ActionLandingDustState *state, ActionSceneEffectFrame *frame,
    const uint8_t *wram, size_t size, const ActionBgMapView *map,
    unsigned room, uint16_t clock);
#endif
