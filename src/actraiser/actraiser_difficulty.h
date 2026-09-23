#ifndef ACTRAISER_DIFFICULTY_H
#define ACTRAISER_DIFFICULTY_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_DifficultySpawnEntry(CpuState *cpu);
RecompReturn ActRaiser_DifficultySpawn(CpuState *cpu);
bool ActRaiser_DifficultyContactEntry(CpuState *cpu);
RecompReturn ActRaiser_DifficultyContact(CpuState *cpu);
bool ActRaiser_DifficultyTimerEntry(CpuState *cpu);
RecompReturn ActRaiser_DifficultyTimer(CpuState *cpu);
bool ActRaiser_DifficultyDragonEntry(CpuState *cpu);
RecompReturn ActRaiser_DifficultyDragon(CpuState *cpu);
#endif
