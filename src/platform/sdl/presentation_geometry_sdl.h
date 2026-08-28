#ifndef AR_PRESENTATION_GEOMETRY_SDL_H
#define AR_PRESENTATION_GEOMETRY_SDL_H

#include <stdbool.h>

#include <SDL3/SDL.h>

/* SDL host-output policy. Portable content layout is resolved by
 * render/presentation_layout.h; this adapter owns SDL logical presentation
 * and temporary full-output state. */
bool ArSdlPresentation_ApplyLogical(SDL_Renderer *renderer, bool stretch,
                                    bool crt_pixel_aspect,
                                    int visible_width, int visible_height);

typedef struct ArSdlPresentationOutputState {
  int logical_width;
  int logical_height;
  SDL_RendererLogicalPresentation logical_mode;
  SDL_Rect viewport;
  SDL_Rect clip;
  bool viewport_set;
  bool clip_enabled;
  bool valid;
} ArSdlPresentationOutputState;

bool ArSdlPresentation_PushFullOutput(
    SDL_Renderer *renderer, ArSdlPresentationOutputState *state);
/* Attempts every restore step and reports whether the renderer accepted all
 * of them. */
bool ArSdlPresentation_PopFullOutput(
    SDL_Renderer *renderer, const ArSdlPresentationOutputState *state);

#endif /* AR_PRESENTATION_GEOMETRY_SDL_H */
