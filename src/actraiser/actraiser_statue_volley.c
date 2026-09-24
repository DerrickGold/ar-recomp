#include "actraiser_statue_volley.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"

/* This controller and its ordinary 8657 animation helper do not otherwise
 * use the native local/repeat counter (+38). Initialize it at EVERY first
 * wind-up, not at a spawn callback or from a host slot-identity guess. Native
 * death/reuse discards it with the actor; children inherit it but never use
 * these root-only boundaries (their states are 21/23, not 0E/0F). */
static bool RootShape(CpuState *cpu, unsigned state) {
  if (!ActRaiserRegional_DoubleStatueVolley() || !cpu || cpu->PB || cpu->DB || cpu->D ||
      cpu->m_flag || cpu->x_flag || cpu->emulation || cpu->_flag_D || (cpu->P & CPU_P_D)) return false;
  const unsigned x = cpu->X;
  if (x < kActRaiserWram_ActionObjectTable ||
      x >= kActRaiserWram_ActionObjectTable + kActRaiserActionObjectCount * kActRaiserActionObjectStride ||
      (x - kActRaiserWram_ActionObjectTable) % kActRaiserActionObjectStride) return false;
  const uint16_t source = cpu_read16(cpu, 0, x + kActRaiserActionObject_SourceDescriptor);
  return cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == 2 &&
      (source == 0xbd76 || source == 0xbd84) &&
      cpu_read16(cpu, 0, x + kActRaiserActionObject_AnimationAddress) == 0x4000 &&
      cpu_read8(cpu, 0, x + kActRaiserActionObject_AnimationBank) == 0x7e &&
      cpu_read16(cpu, 0, x + kActRaiserActionObject_AnimationState) == state;
}
bool ActRaiser_StatueVolleyBeginEntry(CpuState *cpu) { return RootShape(cpu, 0x0e); }
bool ActRaiser_StatueVolleyRepeatEntry(CpuState *cpu) {
  return RootShape(cpu, 0x0f) && cpu_read16(cpu, 0, cpu->X + kActRaiserActionObject_LocalCounter) == 1;
}
static RecompReturn WindUp(CpuState *cpu, uint16_t remaining, uint32_t origin) {
  cpu_write16(cpu, 0, cpu->X + kActRaiserActionObject_LocalCounter, remaining);
  cpu->A = 0x0f;
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->A);
  /* BDA2 is the real JSR 8657. It owns the stack/yield and later returns to
   * BDA5's original spawn helper, which may drop a shot on pool exhaustion.
   * Do not re-enter BD90: JP has no second activation/offscreen check. */
  if (!cpu_hle_tailcall_request(0x00bda2, origin))
    ActRaiserHleFatal("Statue wind-up has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_StatueVolleyBegin(CpuState *cpu) {
  if (!ActRaiser_StatueVolleyBeginEntry(cpu)) ActRaiserHleFatal("Unsupported statue first wind-up");
  return WindUp(cpu, 1, 0x00bd9f);
}
RecompReturn ActRaiser_StatueVolleyRepeat(CpuState *cpu) {
  if (!ActRaiser_StatueVolleyRepeatEntry(cpu)) ActRaiserHleFatal("Unsupported statue second wind-up");
  return WindUp(cpu, 0, 0x00bda8);
}
