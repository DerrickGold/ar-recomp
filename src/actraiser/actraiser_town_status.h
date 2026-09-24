#ifndef ACTRAISER_TOWN_STATUS_H
#define ACTRAISER_TOWN_STATUS_H

#include "regional/towns/regional_town_status.h"
#include "snesrecomp/game/cpu.h"

/* Prefix adapters for the US ABI. A captured transaction owns selection;
 * these functions neither activate rules nor publish menu state. */
bool ActRaiserTownStatus_Entry(const CpuState *cpu, bool town_indexed);
/* LDA civilization has already run. Substitute fixed 4 for ASL/CLC/ADC2;
 * the native continuation owns its scratch store and growth comparison. */
bool ActRaiserTownStatus_FixedThreshold(CpuState *cpu);
/* CMP selected expected plot count; preserve A, V and all native memory. */
bool ActRaiserTownStatus_ComparePlots(CpuState *cpu);
/* Replace the US per-town report prefix; the native six-town loop still
 * publishes the selected code, restores flags/bank and owns the return. */
bool ActRaiserTownStatus_JapaneseReport(CpuState *cpu);
/* Additional JP marker before the shared food-allocation path, even when
 * budget is zero. Then perform the original LDA $7C19. */
bool ActRaiserTownStatus_FoodAttempt(CpuState *cpu);

#endif
