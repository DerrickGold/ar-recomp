#ifndef FRAME_SLOT_H
#define FRAME_SLOT_H

/* Internal dependencies of the sole FrameSlot producer. The public capture API
 * and FrameSlot type remain in present.h. */

#include "snesrecomp/game/types.h"
#include "present.h"   /* InspectorPresentationSelection, FrameSlot */
#include "sim/sim3d.h"     /* Sim3DTuning */

/* Host/game state read by FrameSlot_Capture. main.c owns turbo, the inspector
 * selection, and the frame dimensions; host_display.c owns pixel-aspect
 * policy. */
extern uint8 g_turbo;
extern InspectorPresentationSelection g_scene_inspector_presentation;
extern int g_snes_width, g_snes_height;
extern int g_active_pixel_aspect;

/* Shared by DrawAndPresentFrame's canonical annotation and FrameSlot_Capture's
 * fallback annotation. */
Sim3DTuning BuildSim3DTuning(void);

/* Publish DrawAndPresentFrame's already-annotated canonical sim for the
 * duration of its HostDisplay_SubmitFrame call; FrameSlot_Capture copies it
 * instead of recomputing the identical annotation. Pass NULL to clear —
 * every other FrameSlot_Capture caller must see NULL and self-annotate. */
void FrameSlot_SetPendingAnnotatedSim(const SimFrameData *sim);

/* Clear presentation-only action-effect lifecycle history at discontinuities
 * such as savestate loads. The next capture starts fresh from restored WRAM. */
void FrameSlot_ResetActionEffects(void);

#endif /* FRAME_SLOT_H */
