#ifndef AR_ACTRAISER_SIM_COMBAT_H
#define AR_ACTRAISER_SIM_COMBAT_H
/* ActRaiserSimCombat: SIM-mode combat under the regional rules: birth and
 * collision slots, contact and thresholds, with a per-town cache.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_sim_combat_test.c */
#include "regional/towns/regional_sim_combat.h"
#include "snesrecomp/game/cpu.h"
bool ActRaiserSimCombat_CacheTown(CpuState *cpu,unsigned *town);
bool ActRaiserSimCombat_BirthSlot(CpuState *cpu,unsigned *town,unsigned *slot);
bool ActRaiserSimCombat_CollisionSlot(CpuState *cpu,unsigned *town,unsigned *slot);
bool ActRaiserSimCombat_Threshold(CpuState *cpu,uint16_t snapshot);
bool ActRaiserSimCombat_Contact(CpuState *cpu,uint16_t snapshot);
#endif
