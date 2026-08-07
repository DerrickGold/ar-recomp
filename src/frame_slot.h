#ifndef FRAME_SLOT_H
#define FRAME_SLOT_H

/* Q3: FrameSlot_Capture — the sole game-thread FrameSlot producer — was
 * extracted out of main.c into frame_slot.c. FrameSlot_Capture itself is
 * declared in present.h (with the FrameSlot type); this header exposes the
 * main.c-owned globals it reads and the shared Sim3DTuning builder it and
 * DrawAndPresentFrame share. */

#include "types.h"
#include "present.h"   /* InspectorPresentationSelection, FrameSlot */
#include "sim3d.h"     /* Sim3DTuning */

/* Host/game state read by FrameSlot_Capture. main.c owns turbo, the inspector
 * selection, and the frame dimensions; host_display.c owns pixel-aspect
 * policy. */
extern uint8 g_turbo;
extern InspectorPresentationSelection g_scene_inspector_presentation;
extern int g_snes_width, g_snes_height;
extern int g_active_pixel_aspect;

/* The Sim3DTuning snapshot builder. Lives in frame_slot.c (with its only other
 * caller's twin in main.c's DrawAndPresentFrame reaching it through here). */
Sim3DTuning BuildSim3DTuning(void);

/* #16: publish DrawAndPresentFrame's already-annotated canonical sim for the
 * duration of its SubmitFrameToPresent call; FrameSlot_Capture copies it
 * instead of recomputing the identical annotation. Pass NULL to clear —
 * every other FrameSlot_Capture caller must see NULL and self-annotate. */
void FrameSlot_SetPendingAnnotatedSim(const SimFrameData *sim);

/* Clear presentation-only action-effect lifecycle history at discontinuities
 * such as savestate loads. The next capture starts fresh from restored WRAM. */
void FrameSlot_ResetActionEffects(void);

#endif /* FRAME_SLOT_H */
