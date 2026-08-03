#ifndef RENDER_CAPABILITIES_H
#define RENDER_CAPABILITIES_H

#include <stdbool.h>

/* Read-only renderer capability boundary used by Settings availability gates.
 * The present thread owns detection and reset; callers must not mutate the
 * underlying latches or depend on which backend operation rejected them. */
bool Present_SimRimMaskSupported(void);
bool Present_SimEffectRendererSupported(void);

#endif
