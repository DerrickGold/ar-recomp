#include "actraiser_sim_ai.h"
#include "actraiser_sim_actor.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_native_call.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_01_D072_M0X0(CpuState *cpu);
extern RecompReturn bank_03_AF65_M1X0(CpuState *cpu);
extern RecompReturn bank_03_BDE1_M1X0(CpuState *cpu);
static const struct {
  uint16_t pc,tail;
  ArRegionalSimAiRule rule;
  bool narrow;
  uint8_t species; /* zero = any combat species */
} kSeams[kActRaiserSimAi_Count]={
  {0xba67,0xba6a,kArRegionalSimAi_DragonSearch,false,0x12},
  {0xba6a,0xba6d,kArRegionalSimAi_DragonSearch,false,0x12},
  {0xbb5c,0xbb60,kArRegionalSimAi_DragonExtraPass,false,0x12},
  {0xbcbc,0xbd45,kArRegionalSimAi_TargetCoordinates,false,0},
  {0xbd46,0xbd4a,kArRegionalSimAi_TargetPool,true,0},
  {0xbee3,0xbee5,kArRegionalSimAi_BatFallback,true,0x13},
  {0xbf72,0xbf75,kArRegionalSimAi_BatWait,false,0x13},
};
static bool Shape(const CpuState *cpu) {
  return cpu && cpu->PB==1 && cpu->DB==1 && !cpu->D && !cpu->x_flag && !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D);
}
bool ActRaiserSimAi_Entry(CpuState *cpu,ActRaiserSimAiSeam seam,unsigned *town,unsigned *slot) {
  if ((unsigned)seam>=kActRaiserSimAi_Count || !Shape(cpu) || cpu->m_flag!=kSeams[seam].narrow) return false;
  /* B778 has just allocated a strike effect. The Dragon's PHX remains on the
   * native stack; a global "current monster" would break nested actor passes. */
  const unsigned actor=seam==kActRaiserSimAi_DragonRecursion?cpu_read16(cpu,0,(uint16_t)(cpu->S+1)):cpu->X;
  return ActRaiserSimActor_Locate(cpu,actor,town,slot) &&
      (!kSeams[seam].species || cpu_read8(cpu,1,(uint16_t)(actor+0x0e))==kSeams[seam].species);
}
ArRegionalSimAiRule ActRaiserSimAi_Rule(ActRaiserSimAiSeam seam) {
  return (unsigned)seam<kActRaiserSimAi_Count?kSeams[seam].rule:kArRegionalSimAi_Count;
}
uint32_t ActRaiserSimAi_SourcePC(ActRaiserSimAiSeam seam) {
  return (unsigned)seam<kActRaiserSimAi_Count?0x010000u|kSeams[seam].pc:0;
}
static void A8(CpuState *cpu,uint8_t value) { cpu->A=(cpu->A&0xff00)|value;ActRaiserCpuHle_SetNegativeZero8(cpu,value); }
static void A16(CpuState *cpu,uint16_t value) { cpu->A=value;ActRaiserCpuHle_SetNegativeZero16(cpu,value); }
static void Carry(CpuState *cpu,bool value) { cpu->_flag_C=value;cpu->P=(cpu->P&~CPU_P_C)|(value?CPU_P_C:0); }
static void Compare16(CpuState *cpu,uint16_t value) {
  Carry(cpu,cpu->A>=value);ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-value));
}
static RecompReturn Random(CpuState *cpu,uint16_t caller) {
  A8(cpu,32);
  const RecompReturn result=ActRaiserNativeCall(cpu,bank_03_AF65_M1X0,3,caller,true);
  if (result==RECOMP_RETURN_NORMAL && (!Shape(cpu) || !cpu->m_flag)) ActRaiserHleFatal("SIM candidate RNG changed its ABI");
  return result;
}
static void Aligned(CpuState *cpu,bool field) {
  A8(cpu,(uint8_t)cpu->A&0x1c);
  if (!field) {
    /* CLC; ADC #4; DEC A. The masked operand is at most 28. */
    Carry(cpu,false);cpu->_flag_V=0;cpu->P&=(uint8_t)~CPU_P_V;
    A8(cpu,(uint8_t)(cpu->A+4));A8(cpu,(uint8_t)(cpu->A-1));
  }
}
static RecompReturn Candidate(CpuState *cpu) {
  A16(cpu,cpu_read16(cpu,0x7f,0x7c05));Compare16(cpu,2);
  const bool field=cpu->_flag_Z;
  cpu->m_flag=1;cpu->P|=CPU_P_M;
  bool x_aligned=true;
  if (!field) { A8(cpu,cpu_read8(cpu,1,0x0aee));A8(cpu,(uint8_t)cpu->A&2);x_aligned=!cpu->_flag_Z; }
  const uint16_t first=field?0xbccc:x_aligned?0xbcef:0xbd08;
  const uint16_t second=field?0xbcd8:x_aligned?0xbcfc:0xbd12;
  RecompReturn result=Random(cpu,first);
  if (result!=RECOMP_RETURN_NORMAL) return result;
  if (field || x_aligned) Aligned(cpu,field);
  cpu_write8(cpu,0x7f,0x7c11,(uint8_t)cpu->A);
  result=Random(cpu,second);
  if (result!=RECOMP_RETURN_NORMAL) return result;
  if (field || !x_aligned) Aligned(cpu,field);
  cpu_write8(cpu,0x7f,0x7c13,(uint8_t)cpu->A);
  return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiserSimAi_Run(CpuState *cpu,ActRaiserSimAiSeam seam,uint32_t *continuation) {
  unsigned town,slot;
  if (!continuation || !ActRaiserSimAi_Entry(cpu,seam,&town,&slot)) ActRaiserHleFatal("Invalid SIM AI prefix");
  RecompReturn result=RECOMP_RETURN_NORMAL;
  uint16_t tail=kSeams[seam].tail;
  switch (seam) {
    case kActRaiserSimAi_DragonReset:
      result=ActRaiserNativeCall(cpu,bank_01_D072_M0X0,1,0xba69,false);
      if (result!=RECOMP_RETURN_NORMAL) break;
      if (!Shape(cpu) || cpu->m_flag) ActRaiserHleFatal("SIM animation helper changed its ABI");
      cpu_write16(cpu,1,(uint16_t)(cpu->X+0x14),0);break;
    case kActRaiserSimAi_DragonGate: {
      const uint16_t count=(uint16_t)(cpu_read16(cpu,1,(uint16_t)(cpu->X+0x14))+1);
      cpu_write16(cpu,1,(uint16_t)(cpu->X+0x14),count);A16(cpu,count);Compare16(cpu,8);
      if (!cpu->_flag_C) tail=0xba79;
      else { cpu_write16(cpu,1,(uint16_t)(cpu->X+0x14),0);A16(cpu,0); }
      break;
    }
    case kActRaiserSimAi_DragonRecursion: break; /* skip only the nested pass */
    case kActRaiserSimAi_Candidate: result=Candidate(cpu);break;
    case kActRaiserSimAi_Pool:
      result=ActRaiserNativeCall(cpu,bank_03_BDE1_M1X0,3,0xbd49,true);
      if (result==RECOMP_RETURN_NORMAL && (!Shape(cpu) || !cpu->m_flag)) ActRaiserHleFatal("SIM lookup changed its ABI");
      break;
    case kActRaiserSimAi_BatChance:
      Carry(cpu,(uint8_t)cpu->A>=250);ActRaiserCpuHle_SetNegativeZero8(cpu,(uint8_t)(cpu->A-250));break;
    case kActRaiserSimAi_BatWait: A16(cpu,60);break;
    default: ActRaiserHleFatal("Unknown SIM AI prefix");
  }
  if (result==RECOMP_RETURN_NORMAL) *continuation=0x010000u|tail;
  /* Raw child result: runtime applies the same one-level SKIP propagation as
   * the native caller, without running the continuation or fabricating RTS. */
  return result;
}
