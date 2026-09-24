#include "actraiser_fire_enemy.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"
static bool Shape(CpuState *cpu,unsigned state) {
  if(!cpu || cpu->PB || cpu->DB || cpu->D || cpu->m_flag || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  return x>=kActRaiserWram_ActionObjectTable &&
      x<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride &&
      !((x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride) &&
      cpu_read8(cpu,0,kActRaiserWram_MapGroup)==3 && cpu_read16(cpu,0,x+0x32)==0xc3a5 &&
      cpu_read16(cpu,0,x+0x16)==0x4000 && cpu_read8(cpu,0,x+0x18)==0x7e && cpu_read16(cpu,0,x+0x1a)==state;
}
static bool Japanese(unsigned rule) {
  const uint8_t bits=ActRaiserRegional_FireSnapshot();return bits<=15 && (bits&(1u<<rule));
}
static RecompReturn Tail(uint32_t next,uint32_t from) {
  if(!cpu_hle_tailcall_request(next,from))ActRaiserHleFatal("Fire-enemy prefix has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
static void Compare(CpuState *cpu,unsigned value);
bool ActRaiser_FireCloseEntry(CpuState *cpu) {
  return Japanese(kArRegionalFire_CloseStrategy) && Shape(cpu,12) &&
      (cpu_read16(cpu,0,cpu->X+0x28)&kActRaiserObjectFlip_Horizontal) &&
      cpu->_flag_N && cpu->A<96;
}
RecompReturn ActRaiser_FireClose(CpuState *cpu) {
  if(!ActRaiser_FireCloseEntry(cpu))ActRaiserHleFatal("Unsupported fire-enemy close branch");
  /* Native85BE already computed absolute distance and retained its signed
   * direction in N. Preserve the skipped CMP96 before selecting JP state14.
   * Unmirrored movement still uses native13 then the separate hover bypass. */
  Compare(cpu,96);
  return Tail(0x00c3f0,0x00c3dd);
}
bool ActRaiser_FireHoverEntry(CpuState *cpu) {
  return Japanese(kArRegionalFire_CloseStrategy) && Shape(cpu,13);
}
RecompReturn ActRaiser_FireHover(CpuState *cpu) {
  if(!ActRaiser_FireHoverEntry(cpu))ActRaiserHleFatal("Unsupported fire-enemy hover bypass");
  return Tail(0x00c3f6,0x00c3ea);
}
bool ActRaiser_FireChildEntry(CpuState *cpu) {
  return Japanese(kArRegionalFire_ChildThreshold) && Shape(cpu,12) && cpu->A<=255;
}
bool ActRaiser_FireBounceEntry(CpuState *cpu) {
  return Japanese(kArRegionalFire_BounceThreshold) && Shape(cpu,12) && cpu->A<=255;
}
static void Compare(CpuState *cpu,unsigned value) {
  cpu->_flag_C=cpu->A>=value;
  cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-value));
}
RecompReturn ActRaiser_FireChild(CpuState *cpu) {
  if(!ActRaiser_FireChildEntry(cpu))ActRaiserHleFatal("Unsupported fire-enemy child threshold");
  Compare(cpu,128);return Tail(0x00c408,0x00c405);
}
RecompReturn ActRaiser_FireBounce(CpuState *cpu) {
  if(!ActRaiser_FireBounceEntry(cpu))ActRaiserHleFatal("Unsupported fire-enemy bounce threshold");
  Compare(cpu,210);return Tail(0x00c40d,0x00c40a);
}
