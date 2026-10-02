#ifndef AR_DIORAMA_BG_GPU_H
#define AR_DIORAMA_BG_GPU_H

#include "diorama/diorama_planes.h"
#include "render/render_device.h"

/* Render-owner adapter. Substitutes only complete, supported captures. A zero
 * result leaves every CPU texture intact. No pixel readback is performed. */
uint32_t DioramaBgGpu_Resolve(ArRenderDevice *device, const SrPpuBgPacket *packet,
    uint32_t requested_mask, ArRenderTexture textures[kDioramaPlane_Count],
    ArRenderTexture *skybox, uint32_t *changed_mask);
void DioramaBgGpu_Reset(ArRenderDevice *device);

#endif
