#ifndef ACTRAISER_ACTOR_STATS_H
#define ACTRAISER_ACTOR_STATS_H
#include "snesrecomp/game/cpu.h"
/* Fresh base-stat seam, before native professional-mode promotion. Composes
 * with the independently selected platform-skull flags/reward projection. */
bool ActRaiser_ActorStatsEntry(CpuState *cpu);
RecompReturn ActRaiser_ActorStats(CpuState *cpu);
/* Explicit controller writes after native allocation/inheritance/promotion.
 * Only these named child initializers own the override, never a running child. */
bool ActRaiser_TanzraMinionHpEntry(CpuState *cpu);
bool ActRaiser_TanzraMinionRewardEntry(CpuState *cpu);
bool ActRaiser_TanzraProjectileAttackEntry(CpuState *cpu);
RecompReturn ActRaiser_TanzraMinionHp(CpuState *cpu);
RecompReturn ActRaiser_TanzraMinionReward(CpuState *cpu);
RecompReturn ActRaiser_TanzraProjectileAttack(CpuState *cpu);
#endif
