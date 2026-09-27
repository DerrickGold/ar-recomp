#ifndef AR_ACTRAISER_WORLD_RESUME_H
#define AR_ACTRAISER_WORLD_RESUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Game-thread adapter, not a presentation override. Begin runs once after the
 * accepted native title routine returns, before the first Palace assets load.
 * Observe records actual SIM visits, not locations merely flown over. Neither
 * entry writes files; the save host flushes the optional bookmark. Protected
 * record/replay runs must pass permitted=false to both entries. */
void ActRaiserWorldResume_Begin(uint8_t *wram, size_t size, bool continuing,
                               bool remember_last_town, bool permitted);
void ActRaiserWorldResume_Observe(const uint8_t *wram, size_t size, bool permitted);

#endif
