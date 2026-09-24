#include "actraiser_cast_hold.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_game.h"
#include "actraiser_hle_fatal.h"
bool ActRaiser_CastHoldSpawnEntry(CpuState *cpu) {
  const uint8_t hold=ActRaiserRegional_CastHoldSnapshot();
  if(!hold || hold>7 || !cpu || cpu->PB || cpu->DB || cpu->D || cpu->m_flag || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  unsigned rule;
  if(cpu->Y==0xac02)rule=kArRegionalCastHold_Left;
  else if(cpu->Y==0xac32)rule=kArRegionalCastHold_Upper;
  else if(cpu->Y==0xac5e)rule=kArRegionalCastHold_Right;
  else return false;
  if(!(hold&(1u<<rule)))return false;
  const unsigned x=cpu->X;
  return x>=kActRaiserWram_ActionObjectTable &&
      x<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride &&
      !((x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride) &&
      cpu_read8(cpu,0,kActRaiserWram_MapGroup)==1 && cpu_read16(cpu,0,x+0x16)==0x4000 &&
      cpu_read8(cpu,0,x+0x18)==0x7e && cpu_read16(cpu,0,x+0x1a)==38+rule &&
      cpu_read16(cpu,0,x+0x30)==0x32 && !cpu_read16(cpu,0,x+0x2a) &&
      !cpu_read16(cpu,0,x+0x2c) && !cpu_read16(cpu,0,x+0x2e);
}
RecompReturn ActRaiser_CastHoldSpawn(CpuState *cpu) {
  if(!ActRaiser_CastHoldSpawnEntry(cpu))ActRaiserHleFatal("Unsupported linked-prop cast-hold initialization");
  cpu_write16(cpu,0,cpu->X+0x30,0x8032);
  /* Y owns this fresh descriptor; +32 can still belong to a reused slot.
   * Native 8943 uses F9 to hold actors bearing bit8000, then naturally resumes
   * their original linked-position handler. Preserve the other flag bits. */
  cpu->A=0x8032;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00966f,0x00966c))ActRaiserHleFatal("Linked prop has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
