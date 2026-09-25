#ifndef AR_ACTRAISER_REGIONAL_LEVEL_GOALS_H
#define AR_ACTRAISER_REGIONAL_LEVEL_GOALS_H
#include "actraiser/actraiser_level_goals.h"

/* Campaign-owner bridge. Reads don't activate pending changes. */
bool ActRaiserRegional_LevelGoalsSnapshot(bool activate,bool *japanese);
void ActRaiserRegionalLevelGoals_Reset(void);
void ActRaiserRegionalLevelGoals_RefreshReport(CpuState *cpu);
bool ActRaiser_RegionalLevelAwardEntry(CpuState *cpu);
bool ActRaiser_RegionalLevelLeafEntry(CpuState *cpu);
bool ActRaiser_RegionalLevelPrefixEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelAward(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelLeaf(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelCompare(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelLoad(CpuState *cpu);
RecompReturn ActRaiser_RegionalLevelNext(CpuState *cpu);
#endif
