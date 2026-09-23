#ifndef ACTRAISER_CONSTRUCTION_RUNTIME_H
#define ACTRAISER_CONSTRUCTION_RUNTIME_H

#include "snesrecomp/game/cpu.h"

/* Campaign-owned value boundary. Prefixes never activate a pending policy. */
bool ActRaiserRegional_ConstructionSnapshot(bool activate, bool *japanese);
void ActRaiserConstructionRuntime_Reset(void);
bool ActRaiser_RegionalConstructionEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalConstruction(CpuState *cpu);
RecompReturn ActRaiser_RegionalOffscreenConstruction(CpuState *cpu);
bool ActRaiser_RegionalConstructionPriceEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalConstructionBudget(CpuState *cpu);
RecompReturn ActRaiser_RegionalConstructionPayment(CpuState *cpu);
RecompReturn ActRaiser_RegionalConstructionReturn(CpuState *cpu);

#endif
