#ifndef AR_DIORAMA_DEPTH_GPU_H
#define AR_DIORAMA_DEPTH_GPU_H

#include "render/render_device.h"
#include "diorama_depth_pass.h"

/* Present-thread lifetime for Voxel world visibility. Prepare warms the
 * output target before gameplay. Begin also prepares a frame-generation
 * target when needed; the compositor finishes every begun scope, including
 * failures. Off and ordinary Diorama never begin this scope. */
bool DioramaDepthGpu_Prepare(ArRenderDevice *);
const DioramaDepthSink *DioramaDepthGpu_Begin(ArRenderDevice *);
void DioramaDepthGpu_Reset(ArRenderDevice *);

#endif
