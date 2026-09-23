#include "actraiser_level_goals_runtime.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_03_E414_M0X0(CpuState *cpu);
extern RecompReturn bank_03_B3BA_M0X0(CpuState *cpu);
extern RecompReturn bank_03_B3BA_M1X0(CpuState *cpu);
static bool s_active,s_japanese;
void ActRaiserLevelGoalsRuntime_Reset(void) { s_active=s_japanese=false; }
bool ActRaiser_RegionalLevelLeafEntry(CpuState *cpu) {
  bool unused;
  return !s_active && cpu && cpu->PB==3 && !cpu->D && !cpu->x_flag &&
      !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D) &&
      ActRaiserRegional_LevelGoalsSnapshot(false,&unused);
}
bool ActRaiser_RegionalLevelAwardEntry(CpuState *cpu) {
  return ActRaiser_RegionalLevelLeafEntry(cpu) && !cpu->m_flag;
}
static RecompReturn Run(CpuState *cpu,RecompReturn (*native)(CpuState *)) {
  if (s_active || !ActRaiserRegional_LevelGoalsSnapshot(true,&s_japanese))
    ActRaiserHleFatal("Cannot capture level-award rules");
  s_active=true;const RecompReturn result=native(cpu);s_active=false;return result;
}
RecompReturn ActRaiser_RegionalLevelAward(CpuState *cpu) { return Run(cpu,bank_03_E414_M0X0); }
RecompReturn ActRaiser_RegionalLevelLeaf(CpuState *cpu) {
  return Run(cpu,cpu->m_flag?bank_03_B3BA_M1X0:bank_03_B3BA_M0X0);
}
bool ActRaiser_RegionalLevelPrefixEntry(CpuState *cpu) {
  return s_active && s_japanese && ActRaiserLevelGoals_PrefixEntry(cpu);
}
static RecompReturn Tail(bool ok,uint32_t target,uint32_t source) {
  if (!ok || !cpu_hle_tailcall_request(target,source)) ActRaiserHleFatal("Level prefix has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalLevelCompare(CpuState *cpu) {
  return Tail(ActRaiserLevelGoals_Compare(cpu,s_japanese),0x03b3cb,0x03b3c7);
}
RecompReturn ActRaiser_RegionalLevelLoad(CpuState *cpu) {
  return Tail(ActRaiserLevelGoals_Load(cpu,s_japanese),0x03b3d1,0x03b3cd);
}
RecompReturn ActRaiser_RegionalLevelNext(CpuState *cpu) {
  return Tail(ActRaiserLevelGoals_Load(cpu,s_japanese),0x03b40b,0x03b407);
}
void ActRaiserLevelGoalsRuntime_RefreshReport(CpuState *cpu) {
  bool previous=s_japanese,current=s_japanese;
  if (!cpu) return;
  if (!s_active && (!ActRaiserRegional_LevelGoalsSnapshot(false,&previous) ||
      !ActRaiserRegional_LevelGoalsSnapshot(true,&current))) return;
  /* Keep pure-US report behavior untouched. A transition back to US replaces
   * its previously derived Japanese threshold, without running any awards. */
  if (previous || current) (void)ActRaiserLevelGoals_RefreshDisplay(cpu,current);
}
