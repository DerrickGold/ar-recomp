#ifndef AR_ACTRAISER_DIFFICULTY_H
#define AR_ACTRAISER_DIFFICULTY_H
/* ActRaiser difficulty hooks: enemy spawn and contact, the stage timer and the
 * dragon, under the regional difficulty rules.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_difficulty_test.c */
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
