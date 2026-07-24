#ifndef FRAME_SLOT_H
#define FRAME_SLOT_H

/* Q3: FrameSlot_Capture — the sole game-thread FrameSlot producer — was
 * extracted out of main.c into frame_slot.c. FrameSlot_Capture itself is
 * declared in present.h:248; this header exposes the main.c-owned globals it
 * reads and the shared Sim3DTuning builder it and DrawAndPresentFrame share. */

#include "types.h"
#include "present.h"   /* InspectorPresentationSelection, FrameSlot */
#include "sim3d.h"     /* Sim3DTuning */

/* Owned/defined in main.c; read by FrameSlot_Capture in frame_slot.c. */
extern uint8 g_turbo;
extern InspectorPresentationSelection g_scene_inspector_presentation;
extern int g_snes_width, g_snes_height;
extern int g_active_pixel_aspect;

/* The Sim3DTuning snapshot builder. Lives in frame_slot.c (with its only other
 * caller's twin in main.c's DrawAndPresentFrame reaching it through here). */
Sim3DTuning BuildSim3DTuning(void);

#endif /* FRAME_SLOT_H */
