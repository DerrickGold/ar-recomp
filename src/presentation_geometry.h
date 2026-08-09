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

SDL_Rect PresentationGeometry_CalculateViewport(int output_width,
                                                int output_height,
                                                bool stretch,
                                                bool crt_pixel_aspect,
                                                int visible_width,
                                                int snes_height);

#endif  /* PRESENTATION_GEOMETRY_H */
