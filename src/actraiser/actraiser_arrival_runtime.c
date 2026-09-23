#include "actraiser_arrival_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_00_A343_M0X0(CpuState *cpu);
extern RecompReturn bank_01_861E_M1X0(CpuState *cpu);
static bool s_departure_delegate,s_palace_delegate;
void ActRaiserArrivalRuntime_Reset(void){s_departure_delegate=s_palace_delegate=false;}
static bool Shape(CpuState *cpu,unsigned bank,bool narrow) {
  bool unused;
  return cpu && cpu->PB==bank && cpu->DB==bank && !cpu->D && cpu->m_flag==narrow &&
      !cpu->x_flag && !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D) &&
      !cpu_read8(cpu,0,0x0347) && ActRaiserRegional_ArrivalSnapshot(false,false,&unused);
}
bool ActRaiser_RegionalArrivalDepartureEntry(CpuState *cpu) {
  if(s_departure_delegate){s_departure_delegate=false;return false;}
  return Shape(cpu,0,false) && cpu_read8(cpu,0,0x0341)>=1 && cpu_read8(cpu,0,0x0341)<=6;
}
bool ActRaiser_RegionalArrivalPalaceEntry(CpuState *cpu) {
  if(s_palace_delegate){s_palace_delegate=false;return false;}
  return Shape(cpu,1,true);
}
static bool Complete(CpuState *cpu,bool bytes) {
  for(unsigned town=0;town<6;++town) {
    const uint16_t value=bytes?cpu_read8(cpu,0x7f,0x6b18+2*town):cpu_read16(cpu,0x7f,0x6b18+2*town);
    if(value!=2)return false;
  }
  return true;
}
/* JP $01:85C8 eligibility, expressed over the baseline's shared count/flag
 * addresses. Preserve its byte comparisons and 16-bit X/Y clobbers; malformed
 * bit0-with-incomplete-counts does not accidentally announce via the US gate. */
static bool JapanesePalaceGuard(CpuState *cpu,uint8_t flags) {
  cpu->A=(cpu->A&0xff00)|(flags&2);
  ActRaiserCpuHle_SetNegativeZero8(cpu,(uint8_t)cpu->A);
  if(flags&2)return false;
  cpu->Y=6;cpu->X=0;
  for(unsigned town=0;town<6;++town) {
    const uint8_t count=cpu_read8(cpu,0x7f,0x6b18+2*town);
    cpu->A=(cpu->A&0xff00)|count;
    cpu->_flag_C=count>=2;cpu->P=(cpu->P&~CPU_P_C)|(count>=2?CPU_P_C:0);
    ActRaiserCpuHle_SetNegativeZero8(cpu,(uint8_t)(count-2));
    if(count!=2)return false;
    cpu->X+=2;--cpu->Y;
    ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->Y);
  }
  /* Native US suffix will atomically add announced bit1 before its first
   * dialogue yield. Its original message/cleanup owns the remaining ABI. */
  cpu_write8(cpu,0x7f,0x9101,flags|1);
  return true;
}
RecompReturn ActRaiser_RegionalArrivalDeparture(CpuState *cpu) {
  bool japanese;
  const bool continuing=(cpu_read8(cpu,0x7f,0x9101)&1)!=0;
  if(!ActRaiserRegional_ArrivalSnapshot(continuing || Complete(cpu,false),continuing,&japanese))
    ActRaiserHleFatal("Cannot capture final-island departure policy");
  if(!japanese) {
    s_departure_delegate=true;
    const RecompReturn result=bank_00_A343_M0X0(cpu);
    s_departure_delegate=false;return result;
  }
  /* JP $00:A335: SEP/LDA current town/STA next scene/STZ subscene/REP/RTS.
   * No unlock bit, reveal marker, music selection, score or reward writes.
   * This replaces an RTS leaf, not a fabricated caller or stack unwind. */
  const uint8_t town=cpu_read8(cpu,0,0x0341);
  cpu->A=(cpu->A&0xff00)|town;
  ActRaiserCpuHle_SetNegativeZero8(cpu,town);
  cpu_write8(cpu,0,0x001a,town);cpu_write8(cpu,0,0x001b,0);
  cpu->S=(uint16_t)(cpu->S+2);
  return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiser_RegionalArrivalPalace(CpuState *cpu) {
  const uint8_t flags=cpu_read8(cpu,0x7f,0x9101);
  bool japanese;
  if(!ActRaiserRegional_ArrivalSnapshot((flags&1)!=0,(flags&1)!=0,&japanese))
    ActRaiserHleFatal("Cannot resume final-island arrival policy");
  if(japanese) {
    if(!(flags&2) && Complete(cpu,true) && !ActRaiserRegional_ArrivalSnapshot(true,false,&japanese))
      ActRaiserHleFatal("Cannot capture final-island Palace policy");
    /* Adapt the JP eligibility guard to the US announcement owner. The native
     * body sets announced bit1, runs the translated message, clears BG3 and
     * its pending-message byte. Existing unlock/announcement bits never clear. */
    if(!JapanesePalaceGuard(cpu,flags)) {
      cpu->S=(uint16_t)(cpu->S+2);
      return RECOMP_RETURN_NORMAL;
    }
  }
  s_palace_delegate=true;
  const RecompReturn result=bank_01_861E_M1X0(cpu);
  s_palace_delegate=false;return result;
}
