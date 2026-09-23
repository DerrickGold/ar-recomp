#include "actraiser_level_goals.h"
#include "actraiser_cpu_hle_internal.h"

bool ActRaiserLevelGoals_PrefixEntry(const CpuState *cpu) {
  return cpu && cpu->PB==3 && !cpu->D && !cpu->m_flag && !cpu->x_flag &&
      !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D) &&
      !(cpu->X&1) && cpu->X<2*kArRegionalLevelGoals_Count;
}
bool ActRaiserLevelGoals_Compare(CpuState *cpu,bool japanese) {
  uint16_t threshold;
  if (!ActRaiserLevelGoals_PrefixEntry(cpu) || !ArRegionalLevelGoals_Threshold(japanese,cpu->X/2,&threshold)) return false;
  cpu->_flag_C=cpu->A>=threshold;
  cpu->P=(cpu->P & ~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0);
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-threshold));return true;
}
bool ActRaiserLevelGoals_Load(CpuState *cpu,bool japanese) {
  uint16_t threshold;
  if (!ActRaiserLevelGoals_PrefixEntry(cpu) || !ArRegionalLevelGoals_Threshold(japanese,cpu->X/2,&threshold)) return false;
  cpu->A=threshold;ActRaiserCpuHle_SetNegativeZero16(cpu,threshold);return true;
}
bool ActRaiserLevelGoals_RefreshDisplay(CpuState *cpu,bool japanese) {
  uint16_t threshold;
  if (!cpu || !ArRegionalLevelGoals_Display(japanese,cpu_read16(cpu,0,0x0291),&threshold)) return false;
  cpu_write16(cpu,0,0x0297,threshold);return true;
}
