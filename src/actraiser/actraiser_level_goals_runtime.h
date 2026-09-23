#ifndef ACTRAISER_LEVEL_GOALS_RUNTIME_H
#define ACTRAISER_LEVEL_GOALS_RUNTIME_H
#include "actraiser_level_goals.h"

/* Campaign-owner bridge. Reads don't activate pending changes. */
bool ActRaiserRegional_LevelGoalsSnapshot(bool activate,bool *japanese);
void ActRaiserLevelGoalsRuntime_Reset(void);
void ActRaiserLevelGoalsRuntime_RefreshReport(CpuState *cpu);
bool ActRaiser_RegionalLevelAwardEntry(CpuState *cpu);
bool ActRaiser_RegionalLevelLeafEntry(CpuState *cpu);
bool ActRaiser_RegionalLevelPrefixEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelAward(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelLeaf(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelCompare(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelLoad(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelNext(CpuState *cpu);
#endif
