#ifndef AR_SMAA_SDL_H
#define AR_SMAA_SDL_H

#include <SDL3/SDL.h>
#include "render/render_types.h"

typedef struct ArSdlSmaa {
  SDL_Texture *area, *search;
  SDL_Texture *edges, *weights;
  SDL_GPUSampler *sampler;
  SDL_GPURenderState *edge_pass, *weight_pass, *blend_pass;
  /* Last attempted size. A failed allocation uses FXAA until the size changes. */
  int width, height;
  bool unavailable;
} ArSdlSmaa;

struct ArSdlRenderBackend;
/* Omitted permits FXAA fallback. StateLost means the caller must stop drawing. */
ArRenderTargetBeginResult ArSdlSmaa_Draw(struct ArSdlRenderBackend *backend,
    SDL_Texture *scene, const SDL_FRect *source, const SDL_FRect *destination);
void ArSdlSmaa_Reset(struct ArSdlRenderBackend *backend);

#endif
