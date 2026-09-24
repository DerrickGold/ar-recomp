#ifndef ACTRAISER_DEVELOPMENT_H
#define ACTRAISER_DEVELOPMENT_H
#include "regional/towns/regional_development.h"
#include "snesrecomp/game/cpu.h"

/* Bounded coordinator bodies. Native callees retain event/actor/census/reward
 * behavior. All three leaves are service counts, not frames or PAL seconds. */
bool ActRaiserDevelopment_MasterEntry(const CpuState *cpu);
RecompReturn ActRaiserDevelopment_Master(CpuState *cpu, const ArRegionalDevelopmentSnapshot *snapshot);
bool ActRaiserDevelopment_EffectEntry(const CpuState *cpu);
RecompReturn ActRaiserDevelopment_Effect(CpuState *cpu,
    const ArRegionalDevelopmentSnapshot *snapshot, bool world_actors);
#endif
