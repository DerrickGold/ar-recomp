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

/* Scoped ownership for a temporary render target. Optional effects may omit
 * themselves when capture/bind is rejected before use; a failed restoration
 * is different because the caller no longer knows where subsequent draws
 * would land. The tri-state begin result preserves that distinction without
 * making every effect duplicate renderer-state bookkeeping. */
typedef struct PresentationTargetState {
  SDL_Texture *target;
  SDL_Rect viewport;
  SDL_Rect clip;
  SDL_BlendMode draw_blend;
  Uint8 draw_r;
  Uint8 draw_g;
  Uint8 draw_b;
  Uint8 draw_a;
  bool viewport_set;
  bool clip_enabled;
  bool valid;
} PresentationTargetState;

typedef enum PresentationTargetBeginResult {
  kPresentationTargetBegin_Ready,
  kPresentationTargetBegin_Omitted,
  kPresentationTargetBegin_StateLost,
} PresentationTargetBeginResult;

PresentationTargetBeginResult PresentationGeometry_BeginTarget(
    SDL_Renderer *renderer, SDL_Texture *target,
    PresentationTargetState *state);
/* Attempts every restore step even after one fails. */
bool PresentationGeometry_EndTarget(
    SDL_Renderer *renderer, const PresentationTargetState *state);

SDL_Rect PresentationGeometry_CalculateViewport(int output_width,
                                                int output_height,
                                                bool stretch,
                                                bool crt_pixel_aspect,
                                                int visible_width,
                                                int snes_height);

#endif  /* PRESENTATION_GEOMETRY_H */
