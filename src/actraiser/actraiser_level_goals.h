#ifndef AR_ACTRAISER_LEVEL_GOALS_H
#define AR_ACTRAISER_LEVEL_GOALS_H
/* ActRaiserLevelGoals: the regional level-goal rules (compare, load, and the
 * report's display field), with the Japanese variants.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_level_goals_test.c */
#include "snesrecomp/game/cpu.h"
#include "regional/towns/regional_level_goals.h"

bool ActRaiserLevelGoals_PrefixEntry(const CpuState *cpu);
bool ActRaiserLevelGoals_Compare(CpuState *cpu,bool japanese);
bool ActRaiserLevelGoals_Load(CpuState *cpu,bool japanese);
/* Derived report field only; never calls the award or heal routines. */
bool ActRaiserLevelGoals_RefreshDisplay(CpuState *cpu,bool japanese);
#endif
