#ifndef AR_PPU_GPU_PROBE_MOTION_H
#define AR_PPU_GPU_PROBE_MOTION_H
#include "platform/sdl/gpu_global_motion_sdl.h"
typedef ArGpuGlobalMotion MotionProbe;
typedef ArGpuGlobalMotionResult MotionProbeResult;
#define MotionProbe_Init ArGpuGlobalMotion_Init
#define MotionProbe_Destroy ArGpuGlobalMotion_Destroy
#define MotionProbe_Resize ArGpuGlobalMotion_Resize
#define MotionProbe_Analyze ArGpuGlobalMotion_Analyze
#define MotionProbe_Warp ArGpuGlobalMotion_Warp
/* Diagnostic readback, never part of production analysis or synthesis. */
unsigned MotionProbe_Check(MotionProbe *p, SDL_GPUTexture *previous, SDL_GPUTexture *current,
    const uint32_t *const previous_pixels[6], const uint32_t *const current_pixels[6], unsigned label);
void MotionProbe_CheckPatterns(MotionProbe *p);
#endif
