#include "actraiser_quake.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_native_call.h"

extern RecompReturn bank_03_AF65_M1X0(CpuState *cpu);
static const struct { uint16_t entry, keep, destroy; } kSelectors[kArRegionalQuake_Count] = {
  {0xa066, 0xa06f, 0xa075}, {0xa144, 0xa14b, 0xa151},
  {0xa1e8, 0xa346, 0xa1ee}, {0xa284, 0xa346, 0xa28a},
  {0xa2e3, 0xa2e9, 0xa34c},
};

bool ActRaiserQuake_SelectorEntry(const CpuState *cpu) {
  return cpu && cpu->PB == 3 && cpu->DB == 0x7f && !cpu->D &&
      !cpu->emulation && cpu->m_flag && !cpu->x_flag;
}
uint32_t ActRaiserQuake_SelectorPC(ArRegionalQuakeRule rule) {
  return (unsigned)rule < kArRegionalQuake_Count ? 0x030000u | kSelectors[rule].entry : 0;
}
RecompReturn ActRaiserQuake_Select(CpuState *cpu, ArRegionalQuakeRule rule, uint32_t *target) {
  if (!ActRaiserQuake_SelectorEntry(cpu) || (unsigned)rule >= kArRegionalQuake_Count || !target)
    ActRaiserHleFatal("Invalid regional earthquake selector");
  cpu_mirrors_to_p(cpu);
  cpu_write_a8(cpu, 0xff); ActRaiserCpuHle_SetNegativeZero8(cpu, 0xff);
  /* Reuse the existing native random-class call site and RNG implementation.
   * No host random draw, branch on subtype, or extra resource callback. */
  RecompReturn result = ActRaiserNativeCall(cpu, bank_03_AF65_M1X0, 3, 0xa341, true);
  if (result != RECOMP_RETURN_NORMAL) return result;
  if (!ActRaiserQuake_SelectorEntry(cpu)) ActRaiserHleFatal("Earthquake RNG changed its ABI");
  const bool destroy = (uint8_t)cpu->A < 0x80;
  cpu->_flag_C = !destroy; cpu->P = (cpu->P & ~1u) | cpu->_flag_C;
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)(cpu->A - 0x80));
  /* Classes3/4 keep and class5 destroy begin at their intercepted entry.
   * Use class6's identical native action continuations instead of re-entering
   * the selector (which would draw again). Only houses have a feedback tail. */
  const uint16_t pc = destroy ? kSelectors[rule].destroy : kSelectors[rule].keep;
  *target = 0x030000u | pc;
  return RECOMP_RETURN_NORMAL;
}
