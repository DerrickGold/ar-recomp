#ifndef ACTRAISER_SIM_COMBAT_RUNTIME_H
#define ACTRAISER_SIM_COMBAT_RUNTIME_H
#include "actraiser_sim_combat.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
bool ActRaiser_RegionalSimCacheEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSimCacheLoad(CpuState *cpu);
RecompReturn ActRaiser_RegionalSimCacheSave(CpuState *cpu);
bool ActRaiser_RegionalSimBirthEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSimBirth(CpuState *cpu);
bool ActRaiser_RegionalSimCollisionEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSimThreshold(CpuState *cpu);
RecompReturn ActRaiser_RegionalSimContact(CpuState *cpu);
#endif
