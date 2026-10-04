#ifndef AR_CHURCH_EXTERIOR_H
#define AR_CHURCH_EXTERIOR_H
/* Static ground shadows for the captured SIM town, in its native atlas space. */
#include "sim/church/church_scene.h"

/* Reads the immutable 512-square ARGB atlas; writes a separate shaded copy.
 * Uses the same Ultra geometry, proportions, terrain anchor and sun as SIM. */
bool ChurchExterior_BakeGround(const SimBackgroundVoxelScene *town,
                               const ChurchSceneOptions *options, const uint32_t *source,
                               uint32_t *output);
#endif
