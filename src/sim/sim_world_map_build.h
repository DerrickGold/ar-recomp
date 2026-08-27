#ifndef SIM_WORLD_MAP_BUILD_H
#define SIM_WORLD_MAP_BUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runner_next.h"

/* Bind the opaque runner used only by the opt-in transactional comparison
 * oracle. The production compositor does not query runner state. */
void SimWorldMapBuild_BindRunner(SrRunnerHandle *runner);

/* Load the immutable translation and destination tables used by the pure HLE.
 * Safe to fail: BuildIfNeeded then leaves the pristine SimWorldMap unpublished
 * and the renderer falls back to the authentic path. */
bool SimWorldMapBuild_Init(const uint8_t *rom_data, size_t rom_size);

/* Main/game thread, once per rendered game frame. On simulation-town or
 * world-navigation entry, and whenever construction inputs change, composes and
 * publishes one complete developed tilemap. During navigation it also
 * synchronizes the owned ROM water art to the authentic four-phase DMA source;
 * town underlays continue the same eight-tick cycle from the global game
 * clock. It never observes $7E:C000 or PPU VRAM and the production path
 * mutates no emulator state. */
void SimWorldMap_BuildIfNeeded(void);

#endif /* SIM_WORLD_MAP_BUILD_H */
