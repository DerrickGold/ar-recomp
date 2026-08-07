#ifndef SIM_VISUAL_PATCHES_H
#define SIM_VISUAL_PATCHES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Apply deterministic, presentation-only adjustments to the live cart image
 * before any subsystem snapshots it as a restore baseline. Every patch first
 * validates its complete source-data signature and leaves the image untouched
 * on mismatch. */
bool SimVisualPatches_Apply(uint8_t *rom_data, size_t rom_size);

#endif /* SIM_VISUAL_PATCHES_H */
