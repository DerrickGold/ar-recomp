#include "actraiser_sim_combat_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include <stdio.h>
#include <stdlib.h>

extern RecompReturn bank_03_813F_M0X0(CpuState *cpu);
extern RecompReturn bank_03_813F_M1X0(CpuState *cpu);
extern RecompReturn bank_03_8168_M0X0(CpuState *cpu);
extern RecompReturn bank_03_8168_M1X0(CpuState *cpu);
static bool s_cache_delegate,s_trace;
bool ActRaiser_RegionalSimCacheEntry(CpuState *cpu) {
  unsigned town;
  return !s_cache_delegate && ActRaiserRegional_SimActorsReady() && ActRaiserSimCombat_CacheTown(cpu,&town);
}
static RecompReturn Cache(CpuState *cpu,bool load) {
  unsigned town;
  if (!ActRaiserSimCombat_CacheTown(cpu,&town)) ActRaiserHleFatal("Invalid SIM actor cache owner");
  s_trace=getenv("AR_REGIONAL_TRACE")!=NULL; /* once per cache copy, not per hit */
  s_cache_delegate=true;
  const RecompReturn result=load?(cpu->m_flag?bank_03_813F_M1X0(cpu):bank_03_813F_M0X0(cpu)):
      (cpu->m_flag?bank_03_8168_M1X0(cpu):bank_03_8168_M0X0(cpu));
  s_cache_delegate=false;
  if (result==RECOMP_RETURN_NORMAL) ActRaiserRegional_SimActorCache(load,town);
  return result;
}
RecompReturn ActRaiser_RegionalSimCacheLoad(CpuState *cpu) { return Cache(cpu,true); }
RecompReturn ActRaiser_RegionalSimCacheSave(CpuState *cpu) { return Cache(cpu,false); }
bool ActRaiser_RegionalSimBirthEntry(CpuState *cpu) {
  unsigned town,slot;
  return ActRaiserRegional_SimActorsReady() && ActRaiserSimCombat_BirthSlot(cpu,&town,&slot);
}
static RecompReturn Tail(uint32_t pc,uint32_t source) {
  if (!cpu_hle_tailcall_request(pc,source)) ActRaiserHleFatal("SIM combat has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalSimBirth(CpuState *cpu) {
  unsigned town,slot;
  if (!ActRaiserSimCombat_BirthSlot(cpu,&town,&slot)) ActRaiserHleFatal("Invalid SIM birth owner");
  ActRaiserRegional_SimActorBirth(town,slot);
  /* Displaced LDA #0 only; all eight native actor stores still execute. */
  cpu->A=0;ActRaiserCpuHle_SetNegativeZero16(cpu,0);return Tail(0x03b9f1,0x03b9ee);
}
static bool Snapshot(CpuState *cpu,uint16_t *snapshot) {
  unsigned town,slot;
  return ActRaiserSimCombat_CollisionSlot(cpu,&town,&slot) && ActRaiserRegional_SimActorSnapshot(town,slot,snapshot);
}
bool ActRaiser_RegionalSimCollisionEntry(CpuState *cpu) {
  uint16_t snapshot;
  return Snapshot(cpu,&snapshot) && snapshot;
}
RecompReturn ActRaiser_RegionalSimThreshold(CpuState *cpu) {
  uint16_t snapshot;
  if (!Snapshot(cpu,&snapshot) || !ActRaiserSimCombat_Threshold(cpu,snapshot)) ActRaiserHleFatal("Invalid SIM threshold owner");
  if (s_trace) fprintf(stderr,"[regional] SIM arrow species=%u actor=%04x damage=%u threshold=%u rules=%u\n",
      cpu->X,cpu->Y,cpu_read8(cpu,1,(uint16_t)(cpu->Y+0x24)),(uint8_t)cpu->A,snapshot);
  return Tail(0x01b01c,0x01b018);
}
RecompReturn ActRaiser_RegionalSimContact(CpuState *cpu) {
  uint16_t snapshot;const uint8_t hp=(uint8_t)cpu->A;
  if (!Snapshot(cpu,&snapshot) || !ActRaiserSimCombat_Contact(cpu,snapshot)) ActRaiserHleFatal("Invalid SIM contact owner");
  if (s_trace) fprintf(stderr,"[regional] SIM contact species=%u actor=%04x hp=%u raw-result=%u rules=%u\n",
      cpu->X,cpu->Y,hp,(uint8_t)cpu->A,snapshot);
  return Tail(0x01b0d0,0x01b0cc);
}
