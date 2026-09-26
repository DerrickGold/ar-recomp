#ifndef AR_ACTRAISER_FIRE_ENEMY_H
#define AR_ACTRAISER_FIRE_ENEMY_H
/* ActRaiser fire-enemy hooks: the fire enemy's close, hover, child and bounce
 * behavior under the regional fire rules.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_fire_enemy_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_FireCloseEntry(CpuState *cpu);
RecompReturn ActRaiser_FireClose(CpuState *cpu);
bool ActRaiser_FireHoverEntry(CpuState *cpu);
RecompReturn ActRaiser_FireHover(CpuState *cpu);
bool ActRaiser_FireChildEntry(CpuState *cpu);
RecompReturn ActRaiser_FireChild(CpuState *cpu);
bool ActRaiser_FireBounceEntry(CpuState *cpu);
RecompReturn ActRaiser_FireBounce(CpuState *cpu);
#endif
