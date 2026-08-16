#ifndef SIM_BACKGROUND_MOUNTAIN_SILHOUETTE_H
#define SIM_BACKGROUND_MOUNTAIN_SILHOUETTE_H

#include <stdbool.h>
#include <stdint.h>

/* Looks up the terrain-independent foreground shape of a mountain metatile.
 * The masks were generated from the unobscured Fillmore/Aitos source art, so
 * snowy towns can reuse the exact geometry without classifying white pixels as
 * either rock or ground by colour. Returns false when the tile has no audited
 * semantic mask. */
bool SimBackgroundMountainSilhouette_Lookup(
    uint8_t tile, int pixel_x, int pixel_y, bool *opaque);

#endif  /* SIM_BACKGROUND_MOUNTAIN_SILHOUETTE_H */
