#ifndef AR_DIORAMA_BG_GPU_H
#define AR_DIORAMA_BG_GPU_H

#include "diorama/diorama_planes.h"
#include "render/render_device.h"

/* Render-owner adapter. Substitutes only complete, supported captures. A zero
 * result leaves every CPU texture intact. No pixel readback is performed. */
uint32_t DioramaBgGpu_Resolve(ArRenderDevice *device, const SrPpuBgPacket *packet,
    uint32_t requested_mask, ArRenderTexture textures[kDioramaPlane_Count],
    ArRenderTexture *skybox, uint32_t *changed_mask);
struct OwnedBgPacket;
/* An optional retained result borrows the exact input snapshot from this
 * resolve. Take a reference before keeping it beyond the current upload. */
uint32_t DioramaBgGpu_ResolveOwned(ArRenderDevice *device, const SrPpuBgPacket *packet,
    struct OwnedBgPacket *owned,
    uint32_t requested_mask, ArRenderTexture textures[kDioramaPlane_Count],
    ArRenderTexture *skybox, uint32_t *changed_mask, struct OwnedBgPacket **retained);
/* Builds the capture decode pipeline ahead of the first stage. Repeat calls
 * are free; false leaves the CPU path in use. */
bool DioramaBgGpu_Prepare(ArRenderDevice *device);
void DioramaBgGpu_Reset(ArRenderDevice *device);

#endif
