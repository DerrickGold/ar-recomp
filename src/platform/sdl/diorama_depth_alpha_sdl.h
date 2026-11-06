#ifndef AR_DIORAMA_DEPTH_ALPHA_SDL_H
#define AR_DIORAMA_DEPTH_ALPHA_SDL_H

#include "diorama_depth_pass_sdl.h"

/* Production bounded transparency policy. Each uninterrupted world band first
 * writes opaque fragments, then resolves its four nearest fractional layers.
 * Remaining layers use the existing painter order behind those four: overflow
 * never silently drops artwork and needs no GPU readback. Native actors and
 * explicit overlays keep their original ordered slots and pixel depth policy.
 * The opaque depth attachment survives calls. Fractional history does not:
 * ordinary SDL/effect boundaries keep their existing painter relationship.
 * This is not an exact whole-frame transparency solver. */
bool ArGpuDioramaDepthPass_PrepareHybrid(ArGpuDioramaDepthPass *, unsigned width,
    unsigned height, unsigned vertices, unsigned indices);
bool ArGpuDioramaDepthPass_EncodeHybridMaterials(ArGpuDioramaDepthPass *,
    SDL_GPUCommandBuffer *, SDL_GPUTexture *target, unsigned width, unsigned height,
    const DioramaDepthDraw *, SDL_GPUTexture *const *sources,
    const ArGpuDioramaDepthMaterial *, unsigned count,
    SDL_GPULoadOp color_load, ArRenderColorF clear_color, bool clear_depth);
void ArGpuDioramaDepthAlpha_Destroy(ArGpuDioramaDepthPass *);

#endif
