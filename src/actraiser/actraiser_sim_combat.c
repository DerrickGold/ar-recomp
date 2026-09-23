#include "actraiser_sim_combat.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_sim_actor.h"

static bool Mode(const CpuState *cpu) {
  return cpu && !cpu->D && !cpu->x_flag && !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D);
}
bool ActRaiserSimCombat_CacheTown(CpuState *cpu,unsigned *town) {
  return town && Mode(cpu) && cpu->PB==3 && ActRaiserSimActor_Town(cpu,town);
}
bool ActRaiserSimCombat_BirthSlot(CpuState *cpu,unsigned *town,unsigned *slot) {
  return town && slot && Mode(cpu) && cpu->PB==3 && cpu->DB==0x7f && !cpu->m_flag &&
      ActRaiserSimActor_Locate(cpu,cpu->X,town,slot) && cpu->Y==(*town*4+*slot)*2 &&
      (cpu_read16(cpu,1,(uint16_t)(cpu->X+0x10))&0x8000);
}
bool ActRaiserSimCombat_CollisionSlot(CpuState *cpu,unsigned *town,unsigned *slot) {
  return town && slot && Mode(cpu) && cpu->PB==1 && cpu->DB==1 && cpu->m_flag && cpu->X<4 &&
      ActRaiserSimActor_Locate(cpu,cpu->Y,town,slot) && cpu_read8(cpu,1,(uint16_t)(cpu->Y+0x0e))==cpu->X+0x12;
}
bool ActRaiserSimCombat_Threshold(CpuState *cpu,uint16_t snapshot) {
  unsigned town,slot;uint8_t threshold,contact;
  if (!ActRaiserSimCombat_CollisionSlot(cpu,&town,&slot) ||
      !ArRegionalSimCombat_Values(snapshot,cpu->X,&threshold,&contact)) return false;
  cpu->A=(cpu->A&0xff00)|threshold;ActRaiserCpuHle_SetNegativeZero8(cpu,threshold);return true;
}
bool ActRaiserSimCombat_Contact(CpuState *cpu,uint16_t snapshot) {
  unsigned town,slot;uint8_t threshold,contact;
  if (!ActRaiserSimCombat_CollisionSlot(cpu,&town,&slot) ||
      !ArRegionalSimCombat_Values(snapshot,cpu->X,&threshold,&contact)) return false;
  const uint8_t before=(uint8_t)cpu->A;
  const unsigned subtrahend=contact+(cpu->_flag_C?0u:1u);
  const uint8_t result=(uint8_t)(before-subtrahend);
  cpu->_flag_C=before>=subtrahend;
  cpu->_flag_V=((before^contact)&(before^result)&0x80)!=0;
  cpu->P=(cpu->P&~(CPU_P_C|CPU_P_V))|(cpu->_flag_C?CPU_P_C:0)|(cpu->_flag_V?CPU_P_V:0);
  cpu->A=(cpu->A&0xff00)|result;ActRaiserCpuHle_SetNegativeZero8(cpu,result);return true;
}
