#ifndef ACTRAISER_LEVEL_GOALS_H
#define ACTRAISER_LEVEL_GOALS_H
#include "snesrecomp/game/cpu.h"
#include "regional/towns/regional_level_goals.h"

bool ActRaiserLevelGoals_PrefixEntry(const CpuState *cpu);
bool ActRaiserLevelGoals_Compare(CpuState *cpu,bool japanese);
bool ActRaiserLevelGoals_Load(CpuState *cpu,bool japanese);
/* Derived report field only; never calls the award or heal routines. */
bool ActRaiserLevelGoals_RefreshDisplay(CpuState *cpu,bool japanese);
#endif
