#include "actraiser_development.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_native_call.h"
#include "actraiser_hle_fatal.h"

#define LEAF(bank,pc,m) extern RecompReturn bank_##bank##_##pc##_M##m##X0(CpuState *)
LEAF(00,8519,0); LEAF(03,8238,0); LEAF(03,C147,0); LEAF(03,826D,0);
LEAF(03,F5BE,0); LEAF(03,8271,0); LEAF(03,8E0C,0); LEAF(03,89F7,0);
LEAF(03,86F1,0); LEAF(03,9DE4,0); LEAF(03,B97F,0); LEAF(01,B898,0);
LEAF(01,B1B7,0); LEAF(03,AF54,0); LEAF(01,9840,0); LEAF(03,AF47,0);
LEAF(01,929E,1); LEAF(02,AFF8,1); LEAF(01,B21B,1); LEAF(01,93BE,1);
LEAF(02,C206,1); LEAF(01,ACD9,0); LEAF(03,9E40,0); LEAF(01,93CB,1);
#undef LEAF

static bool Shape(const CpuState *cpu) { return cpu && !cpu->D && !cpu->emulation && !cpu->x_flag; }
bool ActRaiserDevelopment_MasterEntry(const CpuState *cpu) { return Shape(cpu) && cpu->PB==3; }
bool ActRaiserDevelopment_EffectEntry(const CpuState *cpu) { return Shape(cpu) && cpu->PB==1 && cpu->DB==1; }
static void A16(CpuState *cpu, uint16_t value) { cpu->A=value; ActRaiserCpuHle_SetNegativeZero16(cpu,value); }
static void Compare(CpuState *cpu, uint16_t value) {
  cpu->_flag_C=cpu->A>=value; cpu->P=(cpu->P&~1u)|cpu->_flag_C;
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-value));
}
static void Width(CpuState *cpu, bool narrow) {
  cpu->m_flag=narrow; cpu->P=(cpu->P&~0x20u)|(narrow?0x20:0);
}
static void RestoreP(CpuState *cpu) { cpu->P=cpu_read8(cpu,0,++cpu->S); cpu_p_to_mirrors(cpu); }
static uint16_t Read(CpuState *cpu,uint16_t address) { return cpu_read16(cpu,0x7f,address); }
static void Write(CpuState *cpu,uint16_t address,uint16_t value) { cpu_write16(cpu,0x7f,address,value); }
static void Shift4(CpuState *cpu) {
  cpu->_flag_C=(cpu->A>>3)&1; cpu->P=(cpu->P&~1u)|cpu->_flag_C;
  A16(cpu,cpu->A>>4);
}

/* Callee widths and bank ownership are audited per native call site. */
#define CALL(bank,pc,m,caller,long_call,db) do { \
  result=ActRaiserNativeCall(cpu,bank_##bank##_##pc##_M##m##X0,0x##bank,caller,long_call); \
  if(result!=RECOMP_RETURN_NORMAL) goto nonlocal; \
  if(!Shape(cpu) || cpu->m_flag != m || cpu->DB != db) \
    ActRaiserHleFatal("Development callee outside native ABI at %04x",caller); \
} while(0)

RecompReturn ActRaiserDevelopment_Master(CpuState *cpu, const ArRegionalDevelopmentSnapshot *snapshot) {
  if(!ActRaiserDevelopment_MasterEntry(cpu) || !ArRegionalDevelopment_SnapshotValid(snapshot))
    ActRaiserHleFatal("Invalid development master entry");
  cpu_mirrors_to_p(cpu);
  cpu_write8(cpu,0,cpu->S--,cpu->P); cpu_write8(cpu,0,cpu->S--,cpu->DB);
  Width(cpu,false); A16(cpu,0x7f);
  RecompReturn result;
  CALL(00,8519,0,0x819d,true,0x7f);
  CALL(03,8238,0,0x81a0,false,0x7f);
  A16(cpu,cpu_read16(cpu,0,0x0347)); Compare(cpu,7);
  if(cpu->_flag_Z) goto returned;
  A16(cpu,Read(cpu,0x9750));
  if(!cpu->_flag_Z) { CALL(03,C147,0,0x81b4,false,0x7f); Write(cpu,0x9750,0); }
  if(snapshot->service_divider!=1) {
    Write(cpu,0x7ced,(uint16_t)(Read(cpu,0x7ced)+1)); A16(cpu,Read(cpu,0x7ced));
    Compare(cpu,snapshot->service_divider);
    if(!cpu->_flag_C) goto actors;
    Write(cpu,0x7ced,0);
  }
  Write(cpu,0x9200,(uint16_t)(Read(cpu,0x9200)+1)); A16(cpu,Read(cpu,0x9200));
  Compare(cpu,1);
  if(cpu->_flag_Z) { CALL(03,826D,0,0x81d4,false,0x7f); }
  else {
    Compare(cpu,4);
    if(cpu->_flag_Z) { CALL(03,F5BE,0,0x81d9,false,0x7f); }
    else { Compare(cpu,8); if(cpu->_flag_C) Write(cpu,0x9200,0); }
  }
  Write(cpu,0x91fe,(uint16_t)(Read(cpu,0x91fe)+1)); A16(cpu,Read(cpu,0x91fe));
  Compare(cpu,snapshot->long_cycle);
  if(cpu->_flag_C) {
    Write(cpu,0x91fe,0); Write(cpu,0x9200,0); CALL(03,8271,0,0x81ed,false,0x7f);
  }
  CALL(03,8E0C,0,0x81f0,false,0x7f);
  A16(cpu,Read(cpu,0x7cfb));
  if(!cpu->_flag_Z) { CALL(03,89F7,0,0x81f8,false,0x7f); }
  else {
    CALL(03,86F1,0,0x81fd,false,0x7f); CALL(03,9DE4,0,0x8200,false,0x7f);
    CALL(03,B97F,0,0x8203,false,0x7f);
  }
actors:
  Write(cpu,0x96e8,0);
  CALL(01,B898,0,0x820a,true,0x7f); CALL(01,B1B7,0,0x820e,true,0x7f);
  A16(cpu,Read(cpu,0x96e8));
  if(!cpu->_flag_Z) {
    Write(cpu,0x90eb,cpu->A);
    A16(cpu,Read(cpu,0x96ea)); Shift4(cpu); Write(cpu,0x90e1,cpu->A);
    A16(cpu,Read(cpu,0x96ec)); Shift4(cpu); Write(cpu,0x90e5,cpu->A);
    CALL(03,AF54,0,0x822d,false,0x7f); CALL(01,9840,0,0x8231,true,0x7f);
    CALL(03,AF47,0,0x8234,false,0x7f);
  }
returned:
  cpu->DB=cpu_read8(cpu,0,++cpu->S); RestoreP(cpu);
  cpu->S=(uint16_t)(cpu->S+k65816RtlStackBytes);
  return RECOMP_RETURN_NORMAL;
nonlocal:
  return result>=RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result-1);
}

RecompReturn ActRaiserDevelopment_Effect(CpuState *cpu,
    const ArRegionalDevelopmentSnapshot *snapshot, bool world_actors) {
  if(!ActRaiserDevelopment_EffectEntry(cpu) || !ArRegionalDevelopment_SnapshotValid(snapshot))
    ActRaiserHleFatal("Invalid development effect entry");
  cpu_mirrors_to_p(cpu); cpu_write8(cpu,0,cpu->S--,cpu->P); Width(cpu,true);
  const unsigned offset=world_actors?0:0x2e;
  RecompReturn result;
  CALL(01,929E,1,0x9465+offset,false,1); CALL(02,AFF8,1,0x9469+offset,true,1);
  CALL(01,B21B,1,0x946d+offset,true,1); CALL(01,93BE,1,0x9470+offset,false,1);
  CALL(02,C206,1,0x9474+offset,true,1);
  Width(cpu,false);
  if(world_actors) {
    CALL(01,B898,0,0x947a,true,1); CALL(01,ACD9,0,0x947e,true,1); CALL(01,B1B7,0,0x9482,true,1);
  }
  bool visuals=true;
  if(snapshot->effect_divider!=1) {
    A16(cpu,Read(cpu,0x7ced)); A16(cpu,(uint16_t)(cpu->A+1)); Compare(cpu,snapshot->effect_divider);
    Write(cpu,0x7ced,cpu->A); visuals=cpu->_flag_C;
    if(visuals) { A16(cpu,0); Write(cpu,0x7ced,0); }
  }
  if(visuals) { CALL(03,9E40,0,world_actors?0x9486:0x94a8,true,1); }
  Width(cpu,true); CALL(01,93CB,1,world_actors?0x948b:0x94ad,false,1);
  RestoreP(cpu); cpu->S=(uint16_t)(cpu->S+k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
nonlocal:
  return result>=RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result-1);
}
#undef CALL
