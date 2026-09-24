#include "actraiser_platform_skull.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"
static bool Shape(CpuState *cpu,bool birth) {
  if(!cpu || cpu->PB || cpu->DB || cpu->D || cpu->m_flag || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  return x>=kActRaiserWram_ActionObjectTable &&
      x<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride &&
      !((x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride) &&
      cpu_read8(cpu,0,kActRaiserWram_MapGroup)==4 &&
      (birth?cpu->Y:cpu_read16(cpu,0,x+0x32))==0xd382 && cpu_read16(cpu,0,x+0x16)==0x4000 &&
      cpu_read8(cpu,0,x+0x18)==0x7e && cpu_read16(cpu,0,x+0x1a)==47;
}
bool ActRaiser_PlatformSkullSpawnEntry(CpuState *cpu) {
  const uint8_t rules=ActRaiserRegional_PlatformSkullSnapshot();
  /* First allocation assigns +32 only AFTER 95F0 returns. Y owns the selected
   * descriptor here; stale source bytes in a reused slot are not identity. */
  return rules<16 && (rules&3) && Shape(cpu,true) &&
      !cpu_read16(cpu,0,cpu->X+0x30) && cpu_read16(cpu,0,cpu->X+0x2e)==0x20;
}
RecompReturn ActRaiser_PlatformSkullSpawn(CpuState *cpu) {
  if(!ActRaiser_PlatformSkullSpawnEntry(cpu))ActRaiserHleFatal("Unsupported platform-skull initialization");
  const uint8_t rules=ActRaiserRegional_PlatformSkullSnapshot();
  const uint16_t flags=ArRegionalPlatformSkull_Value(rules,kArRegionalPlatformSkull_Armor)?0x800:0;
  cpu_write16(cpu,0,cpu->X+0x30,flags);
  cpu_write16(cpu,0,cpu->X+0x2e,ArRegionalPlatformSkull_Value(rules,kArRegionalPlatformSkull_Reward));
  /* Replace the fresh flag load, not the initializer or its native return.
   * Professional promotion, first animation, anchoring and death remain native. */
  cpu->A=flags;ActRaiserCpuHle_SetNegativeZero16(cpu,flags);
  if(!cpu_hle_tailcall_request(0x00966f,0x00966c))ActRaiserHleFatal("Skull spawn has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
static bool RangeEntry(CpuState *cpu,unsigned rule) {
  return ArRegionalPlatformSkull_Value(ActRaiserRegional_PlatformSkullSnapshot(),rule)==24 &&
      Shape(cpu,false) && !(cpu_read16(cpu,0,cpu->X+0x30)&0x438);
}
bool ActRaiser_PlatformSkullRangeXEntry(CpuState *cpu) {return RangeEntry(cpu,kArRegionalPlatformSkull_RangeX);}
bool ActRaiser_PlatformSkullRangeYEntry(CpuState *cpu) {return RangeEntry(cpu,kArRegionalPlatformSkull_RangeY);}
static RecompReturn Range(CpuState *cpu,unsigned rule,uint32_t origin) {
  if(!RangeEntry(cpu,rule))ActRaiserHleFatal("Unsupported platform-skull proximity test");
  /* Exact 16-bit CMP24 flags. Absolute-distance helper and the following
   * branch still belong to native code; no new per-frame polling or timer. */
  const uint16_t difference=(uint16_t)(cpu->A-24);
  cpu->_flag_C=cpu->A>=24;
  cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,difference);
  if(!cpu_hle_tailcall_request(origin+3,origin))ActRaiserHleFatal("Skull range has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_PlatformSkullRangeX(CpuState *cpu) {return Range(cpu,kArRegionalPlatformSkull_RangeX,0x00d39a);}
RecompReturn ActRaiser_PlatformSkullRangeY(CpuState *cpu) {return Range(cpu,kArRegionalPlatformSkull_RangeY,0x00d3a2);}
