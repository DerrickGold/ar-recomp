#ifndef AR_DIORAMA_DEPTH_GPU_H
#define AR_DIORAMA_DEPTH_GPU_H

#include "render/render_device.h"
#include "diorama_depth_pass.h"

/* Present-thread lifetime for Voxel world visibility. Prepare warms shared
 * pipelines before gameplay. Begin prepares attachments for the bound target;
 * the compositor finishes every begun scope, including failures.
 * Off and ordinary Diorama never begin this scope. */
typedef enum DioramaDepthTarget {
  kDioramaDepthTarget_Output,
  kDioramaDepthTarget_Crt,
  kDioramaDepthTarget_SceneScale,
} DioramaDepthTarget;
typedef enum DioramaDepthPreparation {
  kDioramaDepthPreparation_Idle,
  kDioramaDepthPreparation_Pending,
  kDioramaDepthPreparation_Ready,
  kDioramaDepthPreparation_Failed,
  kDioramaDepthPreparation_Cancelled,
} DioramaDepthPreparation;
/* The render owner starts and polls an isolated native GPU preparation.
 * The worker never borrows a renderer, scene, or swapchain texture. */
DioramaDepthPreparation DioramaDepthGpu_Request(ArRenderDevice *, DioramaDepthTarget);
DioramaDepthPreparation DioramaDepthGpu_Poll(ArRenderDevice *);
void DioramaDepthGpu_CancelPreparation(void);
/* An optional depth setup failure requires a fresh Diorama capture. */
bool DioramaDepthGpu_DrawFailed(void);
bool DioramaDepthGpu_TakeDrawFailure(void);
const DioramaDepthSink *DioramaDepthGpu_Begin(ArRenderDevice *, bool needs_subscreen);
void DioramaDepthGpu_Reset(ArRenderDevice *);

#endif
