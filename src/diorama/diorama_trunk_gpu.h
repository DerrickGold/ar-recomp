#ifndef AR_DIORAMA_TRUNK_GPU_H
#define AR_DIORAMA_TRUNK_GPU_H
#include "render/render_device.h"
#include "diorama_planes.h"
struct FrameSlot;
bool DioramaTrunkGpu_Prepare(ArRenderDevice *);
bool DioramaTrunkGpu_Upload(ArRenderDevice *,const struct FrameSlot *,
    ArRenderTexture [kDioramaPlane_Count],uint32_t source_changed,uint32_t *changed);
void DioramaTrunkGpu_Reset(ArRenderDevice *);
#endif
