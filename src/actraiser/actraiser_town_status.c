#include "actraiser_town_status.h"
#include "actraiser_cpu_hle_internal.h"

bool ActRaiserTownStatus_Entry(const CpuState *cpu, bool town_indexed) {
  return cpu && cpu->PB==3 && cpu->DB==0x7f && !cpu->D && !cpu->m_flag && !cpu->x_flag &&
      !cpu->emulation && !cpu->_flag_D && !(cpu->P & CPU_P_D) &&
      (!town_indexed || (cpu->X<12 && !(cpu->X&1)));
}
bool ActRaiserTownStatus_FixedThreshold(CpuState *cpu) {
  if (!ActRaiserTownStatus_Entry(cpu,true)) return false;
  cpu->A=4; ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  return true;
}
bool ActRaiserTownStatus_ComparePlots(CpuState *cpu) {
  if (!ActRaiserTownStatus_Entry(cpu,true)) return false;
  uint16_t expected;
  if (!ArRegionalTownStatus_Plots(true,cpu->X/2,&expected)) return false;
  cpu->_flag_C=cpu->A>=expected;
  cpu->P=(cpu->P & ~CPU_P_C) | (cpu->_flag_C?CPU_P_C:0);
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-expected));
  return true;
}
bool ActRaiserTownStatus_JapaneseReport(CpuState *cpu) {
  if (!ActRaiserTownStatus_Entry(cpu,true)) return false;
  const uint16_t population=cpu_read16(cpu,0,(uint16_t)(0x021c+cpu->X));
  const uint16_t gate=cpu_read16(cpu,0x7f,(uint16_t)(0x7cef+cpu->X));
  const uint16_t flags=cpu_read16(cpu,0x7f,(uint16_t)(0x91da+cpu->X));
  cpu->A=ArRegionalTownStatus_JapaneseCode(population,gate,flags);
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  return true;
}
bool ActRaiserTownStatus_FoodAttempt(CpuState *cpu) {
  if (!ActRaiserTownStatus_Entry(cpu,false)) return false;
  cpu_write16(cpu,0x7f,0x7c3d,1);
  cpu->A=cpu_read16(cpu,0x7f,0x7c19);
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  return true;
}
