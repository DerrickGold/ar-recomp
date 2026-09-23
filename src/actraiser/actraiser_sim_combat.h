#ifndef ACTRAISER_SIM_COMBAT_H
#define ACTRAISER_SIM_COMBAT_H
#include "regional/regional_sim_combat.h"
#include "snesrecomp/game/cpu.h"
bool ActRaiserSimCombat_CacheTown(CpuState *cpu,unsigned *town);
bool ActRaiserSimCombat_BirthSlot(CpuState *cpu,unsigned *town,unsigned *slot);
bool ActRaiserSimCombat_CollisionSlot(CpuState *cpu,unsigned *town,unsigned *slot);
bool ActRaiserSimCombat_Threshold(CpuState *cpu,uint16_t snapshot);
bool ActRaiserSimCombat_Contact(CpuState *cpu,uint16_t snapshot);
#endif
