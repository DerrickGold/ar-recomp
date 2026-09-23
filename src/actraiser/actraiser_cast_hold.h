#ifndef ACTRAISER_CAST_HOLD_H
#define ACTRAISER_CAST_HOLD_H
#include "snesrecomp/game/cpu.h"
/* Fresh Fillmore linked-prop flags only. Native cast gate/dispatcher owns the
 * hold and later resumption; no per-frame replacement or parent movement. */
bool ActRaiser_CastHoldSpawnEntry(CpuState *cpu);
RecompReturn ActRaiser_CastHoldSpawn(CpuState *cpu);
#endif
