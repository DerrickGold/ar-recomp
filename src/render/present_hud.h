#ifndef AR_PRESENT_HUD_H
#define AR_PRESENT_HUD_H
/* HUD presentation: project captured status bars, icons and dialogue into
 * screen-space chunks, compose localized text, and own source/composite textures.
 * All game state comes from the completed FrameSlot, never live settings.
 * Layout arithmetic lives beside this adapter in hud_layout.c. */

#include "render/hud_layout.h"
#include "render/render_device.h"

typedef struct FrameSlot FrameSlot;

/* Boot/shutdown allocation. Destroy sources before recreating them. An allocation
 * failure releases both handles. Target resets leave these streaming textures
 * alive; PresentHud_Reset invalidates their mirrors so pixels upload again. */
bool PresentHud_CreateSources(ArRenderDevice *device, int snes_height);
void PresentHud_DestroySources(ArRenderDevice *device);
/* Borrowed handles for menu drawing, inspection and host capture availability.
 * Consumers must not destroy them. */
ArRenderTexture PresentHud_BackgroundTexture(void);
ArRenderTexture PresentHud_ObjectTexture(void);
typedef struct PresentHudUploadResult {
  uint64_t background_bytes, object_bytes;
} PresentHudUploadResult;
/* Consume borrowed runner surfaces synchronously. Separate successful transfer
 * sizes preserve the caller's byte and transfer counts; unchanged/failed sources
 * report zero. Shared by flat, diorama and SIM views. */
PresentHudUploadResult PresentHud_Upload(ArRenderDevice *device, const FrameSlot *slot);

/* The inspector uses the same chunks as drawing to locate visible HUD parts. */
int PresentHud_BuildChunks(const FrameSlot *slot, ArRenderRectI viewport,
                           HudPresentationChunk chunks[kHudPresentationChunkCapacity]);
/* Flat presentation composes only when the captured BG3 includes a body below
 * the status bar. Diorama/SIM callers request a single composite explicitly. */
void PresentHud_Draw(ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport);
void PresentHud_DrawComposited(ArRenderDevice *device, const FrameSlot *slot,
                               ArRenderRectI viewport);
/* On device reset or shutdown, while the host render device is still alive. */
void PresentHud_Reset(ArRenderDevice *device);

#endif
