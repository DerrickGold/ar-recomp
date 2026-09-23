#ifndef ACTRAISER_FIRE_ENEMY_H
#define ACTRAISER_FIRE_ENEMY_H
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
