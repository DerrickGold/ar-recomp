#ifndef AR_DIORAMA_DEPTH_ALPHA_SDL_H
#define AR_DIORAMA_DEPTH_ALPHA_SDL_H

#include "diorama_depth_pass_sdl.h"

/* Production world transparency policy. Each uninterrupted world band first
 * writes its opaque fragments (opaque blend, or alpha that reaches 1) with
 * depth. Every other fragment in front of that depth -- coverage edges, fades
 * and additive light -- is then blended in band draw order. Write-mode alpha is
 * edge coverage and Read-mode draws are additive, so draw order is the only
 * order this needs; overlapping partial fragments are not depth sorted.
 * Native actors and explicit overlays keep their original ordered slots and
 * pixel depth policy. The opaque depth attachment survives calls. */
bool ArGpuDioramaDepthPass_PrepareHybrid(ArGpuDioramaDepthPass *, unsigned width,
    unsigned height, unsigned vertices, unsigned indices);
bool ArGpuDioramaDepthPass_EncodeHybridMaterials(ArGpuDioramaDepthPass *,
    SDL_GPUCommandBuffer *, SDL_GPUTexture *target, unsigned width, unsigned height,
    const DioramaDepthDraw *, SDL_GPUTexture *const *sources,
    const ArGpuDioramaDepthMaterial *, unsigned count,
    SDL_GPULoadOp color_load, ArRenderColorF clear_color, bool clear_depth);
void ArGpuDioramaDepthAlpha_Destroy(ArGpuDioramaDepthPass *);

#endif
