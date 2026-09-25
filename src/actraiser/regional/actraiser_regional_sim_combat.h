#ifndef AR_ACTRAISER_REGIONAL_SIM_COMBAT_H
#define AR_ACTRAISER_REGIONAL_SIM_COMBAT_H
/* Regional SIM combat hooks, named in recomp/bank*.cfg: the town actor cache load and save,
 * actor birth, collision threshold and contact damage for the selected region.
 * Phase: capture (runs on CPU state).
 * Tests: tests/actraiser_sim_combat_test.c */
#include "actraiser/actraiser_sim_combat.h"
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
