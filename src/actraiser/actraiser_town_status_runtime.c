#include "actraiser_town_status_runtime.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_03_82DB_M0X0(CpuState *cpu);
extern RecompReturn bank_03_BF8C_M0X0(CpuState *cpu);
extern RecompReturn bank_03_BF8C_M1X0(CpuState *cpu);
extern RecompReturn bank_03_91AE_M0X0(CpuState *cpu);
extern RecompReturn bank_03_91BC_M0X0(CpuState *cpu);
static bool s_active;
static ArRegionalTownStatusSnapshot s_snapshot;

void ActRaiserTownStatusRuntime_Reset(void) {
  s_active=false;
  s_snapshot=(ArRegionalTownStatusSnapshot){0};
}
bool ActRaiser_RegionalTownStatusCycleEntry(CpuState *cpu) {
  ArRegionalTownStatusSnapshot unused;
  return !s_active && ActRaiserTownStatus_Entry(cpu,false) &&
      ActRaiserRegional_TownStatusSnapshot(false,&unused);
}
bool ActRaiser_RegionalTownStatusReportEntry(CpuState *cpu) {
  ArRegionalTownStatusSnapshot unused;
  return !s_active && cpu && cpu->PB==3 && !cpu->D && !cpu->x_flag && !cpu->emulation &&
      !cpu->_flag_D && !(cpu->P & CPU_P_D) && ActRaiserRegional_TownStatusSnapshot(false,&unused);
}
RecompReturn ActRaiserTownStatusRuntime_Run(CpuState *cpu, RecompReturn (*native)(CpuState *)) {
  if (s_active || !ActRaiserRegional_TownStatusSnapshot(true,&s_snapshot))
    ActRaiserHleFatal("Cannot capture town status transaction");
  s_active=true;
  const RecompReturn result=native(cpu);
  s_active=false;
  return result;
}
RecompReturn ActRaiser_RegionalTownStatusCycle(CpuState *cpu) { return ActRaiserTownStatusRuntime_Run(cpu,bank_03_82DB_M0X0); }
RecompReturn ActRaiser_RegionalTownStatusPlot(CpuState *cpu) { return ActRaiserTownStatusRuntime_Run(cpu,bank_03_91AE_M0X0); }
RecompReturn ActRaiser_RegionalTownStatusVisiblePlot(CpuState *cpu) { return ActRaiserTownStatusRuntime_Run(cpu,bank_03_91BC_M0X0); }
RecompReturn ActRaiser_RegionalTownStatusReport(CpuState *cpu) {
  return ActRaiserTownStatusRuntime_Run(cpu,cpu->m_flag?bank_03_BF8C_M1X0:bank_03_BF8C_M0X0);
}
static bool Selected(CpuState *cpu, ArRegionalTownStatusRule rule, bool indexed) {
  return s_active && s_snapshot.japanese[rule] && ActRaiserTownStatus_Entry(cpu,indexed);
}
bool ActRaiser_RegionalTownStatusLowEntry(CpuState *cpu) { return Selected(cpu,kArRegionalTownStatus_LowGrowth,true); }
bool ActRaiser_RegionalTownStatusPlotCountEntry(CpuState *cpu) { return Selected(cpu,kArRegionalTownStatus_PlotCount,true); }
bool ActRaiser_RegionalTownStatusMergeEntry(CpuState *cpu) { return Selected(cpu,kArRegionalTownStatus_PersistentFlags,true); }
bool ActRaiser_RegionalTownStatusFoodEntry(CpuState *cpu) { return Selected(cpu,kArRegionalTownStatus_FoodAttempt,false); }
bool ActRaiser_RegionalTownStatusClassifierEntry(CpuState *cpu) { return Selected(cpu,kArRegionalTownStatus_Classifier,true); }
static RecompReturn Tail(bool valid, uint32_t target, uint32_t source) {
  if (!valid || !cpu_hle_tailcall_request(target,source)) ActRaiserHleFatal("Town status prefix has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalTownStatusLow(CpuState *cpu) {
  return Tail(ActRaiserTownStatus_FixedThreshold(cpu),0x03856b,0x038566);
}
RecompReturn ActRaiser_RegionalTownStatusReportLow(CpuState *cpu) {
  return Tail(ActRaiserTownStatus_FixedThreshold(cpu),0x03bfa7,0x03bfa2);
}
RecompReturn ActRaiser_RegionalTownStatusPlotCount(CpuState *cpu) {
  return Tail(ActRaiserTownStatus_ComparePlots(cpu),0x0385a7,0x0385a3);
}
RecompReturn ActRaiser_RegionalTownStatusMerge(CpuState *cpu) {
  return Tail(ActRaiser_RegionalTownStatusMergeEntry(cpu),0x0385c6,0x0385c3);
}
RecompReturn ActRaiser_RegionalTownStatusFood(CpuState *cpu) {
  return Tail(ActRaiserTownStatus_FoodAttempt(cpu),0x039274,0x039271);
}
RecompReturn ActRaiser_RegionalTownStatusClassifier(CpuState *cpu) {
  return Tail(ActRaiserTownStatus_JapaneseReport(cpu),0x03c022,0x03bf9e);
}
