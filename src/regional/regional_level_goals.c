#include "regional_level_goals.h"

static const ArRegionalLevelGoalsDescriptor kRule={"level_goals_japanese",{0,1,0}};
static const uint16_t kThresholds[2][kArRegionalLevelGoals_Count]={
  {0,80,200,400,700,950,1200,1500,1700,1900,2200,2500,2900,3300,3700,4100,4600,9999},
  {0,80,200,400,550,650,750,1050,1400,1600,1800,1900,2000,2200,2400,2600,3000,9999}
};
const ArRegionalLevelGoalsDescriptor *ArRegionalLevelGoals_Descriptor(void) { return &kRule; }
bool ArRegionalLevelGoals_Resolve(ArRegionalSource source,bool *japanese) {
  if (!japanese || (unsigned)source>=kArRegionalSource_Count) return false;
  *japanese=kRule.japanese[source]!=0;return true;
}
bool ArRegionalLevelGoals_Threshold(bool japanese,unsigned level,uint16_t *threshold) {
  if (!threshold || level>=kArRegionalLevelGoals_Count) return false;
  *threshold=kThresholds[japanese][level];return true;
}
bool ArRegionalLevelGoals_Display(bool japanese,unsigned level,uint16_t *threshold) {
  if (!ArRegionalLevelGoals_Threshold(japanese,level,threshold)) return false;
  if (level==kArRegionalLevelGoals_MaxLevel) *threshold=0;
  return true;
}
