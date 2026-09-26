#ifndef AR_PRESENT_DIORAMA_H
#define AR_PRESENT_DIORAMA_H
/* Diorama presentation owns plane textures, skybox uploads, coverage/revision
 * caches, camera response and frame-generation composition. Game inputs come
 * from the completed FrameSlot. Pure layer rendering remains in diorama.c. */

#include "render/render_device.h"

typedef struct FrameSlot FrameSlot;

/* Host boot and full device reset: planes require initialized transparent
 * padding beyond each uploaded capture. Destroy before recreating them. */
void PresentDiorama_CreatePlanes(ArRenderDevice *device);
void PresentDiorama_DestroyPlanes(ArRenderDevice *device);
/* Call for every uploaded frame, including non-diorama frames. Borrowed PPU
 * pixels are consumed synchronously. With an unavailable device this only
 * invalidates the published skybox view; no producer pixels are read. */
void PresentDiorama_Upload(ArRenderDevice *device, const FrameSlot *slot);
/* Scene -> action effects -> heat resolve -> anchored HUD. The caller has
 * already selected diorama and handled hardware forced blank. */
void PresentDiorama_Draw(ArRenderDevice *device, const FrameSlot *slot, float alpha);
/* Target/device reset or shutdown: drop derived GPU resources and interpolation
 * history. Camera response and plane allocation have independent lifetimes. */
void PresentDiorama_Reset(ArRenderDevice *device);

#endif
