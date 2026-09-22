#ifndef ACTRAISER_SCROLL_CAST_H
#define ACTRAISER_SCROLL_CAST_H

#include "regional/regional_costs.h"
#include "snesrecomp/game/cpu.h"

/* $00:9DE1..9E0D, reached by a native branch, not a JSR. This gate owns
 * cooldown/equipment/state checks and one generic-scroll debit. It returns
 * the exact native continuation; effects, allocation, interruption and HUD
 * remain native. European Action LIFO inventory is a separate policy. */
bool ActRaiserScrollCast_Entry(const CpuState *cpu);
uint16_t ActRaiserScrollCast_Gate(CpuState *cpu,
                                 const ArRegionalCostSnapshot *prices);

#endif
