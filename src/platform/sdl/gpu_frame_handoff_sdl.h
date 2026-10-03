#ifndef AR_GPU_FRAME_HANDOFF_SDL_H
#define AR_GPU_FRAME_HANDOFF_SDL_H
#include "action_scene_pass_sdl.h"

enum { kArGpuFrameHandoffMaximumPlanes = 12 };

/* Native, presenter-owned resources for one frame pair. Pack normalizes
 * sampled RGBA/BGRA sources into an RGBA8 atlas in one render pass. Unpack
 * copies RGBA8 atlas bands into RGBA8 destinations in one copy pass. The
 * caller submits producers first, then records analysis/warp in this same
 * command stream. Neither helper submits, waits, or downloads. */
typedef struct ArGpuFramePack {
  ArGpuActionSceneTexture source;
  ArRenderRectI destination;
} ArGpuFramePack;

typedef struct ArGpuFrameUnpack {
  SDL_GPUTexture *source, *destination;
  unsigned source_width, source_height, destination_width, destination_height;
  ArRenderRectI source_region;
  ArRenderPointI destination_origin;
  /* Clear only at allocation/extent change. All later writes must stay in
   * this rectangle until invalidated; cycling would discard its padding. */
  bool clear_destination;
} ArGpuFrameUnpack;

bool ArGpuFrameHandoff_EncodePack(ArGpuActionScenePass *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *atlas, unsigned width, unsigned height, SDL_GPUBuffer *motion,
    const ArGpuFramePack *, unsigned count);
bool ArGpuFrameHandoff_EncodeUnpack(SDL_GPUCommandBuffer *,
    const ArGpuFrameUnpack *, unsigned count);
#endif
