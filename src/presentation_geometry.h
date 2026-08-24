#ifndef PRESENTATION_GEOMETRY_H
#define PRESENTATION_GEOMETRY_H

#include <stdbool.h>

#include <SDL3/SDL.h>

/* Shared host-output policy for every presentation path. "Stretch" is the
 * sole switch that may fill a differently shaped output; widescreen rendering
 * is content geometry and must not decide whether aspect correction applies. */
bool PresentationGeometry_ApplyLogical(SDL_Renderer *renderer, bool stretch,
                                       bool crt_pixel_aspect,
                                       int visible_width, int snes_height);

/* Scoped renderer view state for host UI that must cover the physical output
 * rather than inherit the game's logical presentation, viewport, or clip.
 * The target and draw scale are not changed. Push/Pop must run on the same
 * renderer target. */
typedef struct PresentationOutputState {
  int logical_width;
  int logical_height;
  SDL_RendererLogicalPresentation logical_mode;
  SDL_Rect viewport;
  SDL_Rect clip;
  bool viewport_set;
  bool clip_enabled;
  bool valid;
} PresentationOutputState;

bool PresentationGeometry_PushFullOutput(
    SDL_Renderer *renderer, PresentationOutputState *state);
/* Attempts every restore step and reports whether the renderer accepted all
 * of them. Selected rendering paths must propagate failure rather than leak
 * full-output state into the next stage. */
bool PresentationGeometry_PopFullOutput(
    SDL_Renderer *renderer, const PresentationOutputState *state);

SDL_Rect PresentationGeometry_CalculateViewport(int output_width,
                                                int output_height,
                                                bool stretch,
                                                bool crt_pixel_aspect,
                                                int visible_width,
                                                int snes_height);

#endif  /* PRESENTATION_GEOMETRY_H */
