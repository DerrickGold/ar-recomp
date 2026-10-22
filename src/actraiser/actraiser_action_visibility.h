#ifndef AR_ACTRAISER_ACTION_VISIBILITY_H
#define AR_ACTRAISER_ACTION_VISIBILITY_H

#include "snesrecomp/game/cpu.h"

/* Game-thread, read-only visibility check for wave-retention candidates.
 * Compacts valid object-record addresses in place, preserving their order,
 * and returns the surviving count. Uses one current draw-window snapshot for
 * the whole batch, including diorama margins/aprons. Returns zero when extended
 * object drawing is disabled. Never changes WRAM, activation or OAM ownership. */
unsigned ActRaiser_FilterVisibleWaveObjects(CpuState *cpu, uint16_t *objects, unsigned count);

#endif
