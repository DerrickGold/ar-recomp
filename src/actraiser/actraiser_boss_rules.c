#include "actraiser_boss_rules.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"

static bool RootShape(CpuState *cpu,uint16_t original,uint16_t rematch,unsigned area,unsigned state,unsigned narrow) {
  if(!cpu || cpu->PB || cpu->DB || cpu->D || cpu->m_flag!=narrow || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  if(x<kActRaiserWram_ActionObjectTable ||
      x>=kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride ||
      (x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride)return false;
  const uint16_t source=cpu_read16(cpu,0,x+kActRaiserActionObject_SourceDescriptor);
  const unsigned map=cpu_read8(cpu,0,kActRaiserWram_MapGroup);
  return ((map==area && source==original) || (map==7 && source==rematch)) &&
      cpu_read16(cpu,0,x+kActRaiserActionObject_AnimationAddress)==0x5000 &&
      cpu_read8(cpu,0,x+kActRaiserActionObject_AnimationBank)==0x7e &&
      cpu_read16(cpu,0,x+kActRaiserActionObject_AnimationState)==state;
}
bool ActRaiser_WizardPauseEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_WizardPause)==0 &&
      RootShape(cpu,0xbdff,0xf6e2,2,0x0b,0);
}
RecompReturn ActRaiser_WizardPause(CpuState *cpu) {
  /* Only the new hold is skipped. Native position/HP decisions, first/second
   * form transitions and all rematch animation differences remain unchanged. */
  if(!ActRaiser_WizardPauseEntry(cpu) || !cpu_hle_tailcall_request(0x00be7e,0x00be78))
    ActRaiserHleFatal("Unsupported Wizard post-spread pause");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_MinotaurAxeOffsetEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_MinoAxeOffset)==48 &&
      RootShape(cpu,0xaf5d,0xf6ca,1,1,0);
}
RecompReturn ActRaiser_MinotaurAxeOffset(CpuState *cpu) {
  if(!ActRaiser_MinotaurAxeOffsetEntry(cpu))ActRaiserHleFatal("Unsupported Minotaur axe offset");
  cpu->A=(uint16_t)-48;
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  /* The original 8709 facing-relative helper owns Y's destination, including
   * the allocator's scratch record on a dropped shot. Do not allocate again
   * or create a host actor for that out-of-pool address. */
  if(!cpu_hle_tailcall_request(0x00afde,0x00afdb))
    ActRaiserHleFatal("Minotaur offset has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_TanzraClockEntry(CpuState *cpu) {
  if(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_TanzraClock)!=0 ||
      !RootShape(cpu,0xf80f,0xf80f,7,0xff,1))return false;
  const unsigned x=cpu->X;
  return cpu_read16(cpu,0,kActRaiserWram_MapGroup)==0x0807 &&
      cpu_read16(cpu,0,x)==0x0800 && cpu_read16(cpu,0,x+0x12)==0xf8f5 &&
      cpu_read16(cpu,0,x+0x14)==0xf8f5 && !cpu_read16(cpu,0,x+0x2c) &&
      cpu_read16(cpu,0,x+0x30)==0x32;
}
RecompReturn ActRaiser_TanzraClock(CpuState *cpu) {
  /* Skip only the 8-bit STZ E8. Keep elapsed time/divider, the high gate byte,
   * existing collision/cast clears, and the native REP/second-form entry. */
  if(!ActRaiser_TanzraClockEntry(cpu) || !cpu_hle_tailcall_request(0x00f8fe,0x00f8fc))
    ActRaiserHleFatal("Unsupported Tanzra second-form clock transition");
  return RECOMP_RETURN_TAILCALL;
}
