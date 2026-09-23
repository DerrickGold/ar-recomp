#include "actraiser_fishing.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_native_call.h"

extern RecompReturn bank_03_F4DF_M1X0(CpuState *);
extern RecompReturn bank_03_F4EA_M1X0(CpuState *);
extern RecompReturn bank_03_CA93_M1X0(CpuState *);

bool ActRaiserFishing_Entry(const CpuState *cpu) {
  return cpu && cpu->PB == 3 && cpu->DB == 0x7f && !cpu->D &&
      !cpu->emulation && !cpu->m_flag && !cpu->x_flag;
}

static void A8(CpuState *cpu, uint8_t value) {
  cpu_write_a8(cpu, value);
  ActRaiserCpuHle_SetNegativeZero8(cpu, value);
}

RecompReturn ActRaiserFishing_Prefix(CpuState *cpu, uint8_t target,
                                    bool reconcile, uint16_t *continuation) {
  if (!ActRaiserFishing_Entry(cpu) || !target || !continuation)
    ActRaiserHleFatal("Invalid Fillmore fishing prefix");
  cpu_mirrors_to_p(cpu);
  cpu->P |= 0x20; cpu->m_flag = 1;
  RecompReturn result;
#define CALL(leaf, caller) do { \
    result = ActRaiserNativeCall(cpu, leaf, 3, caller, false); \
    if (result != RECOMP_RETURN_NORMAL) goto nonlocal; \
    if (cpu->PB != 3 || cpu->DB != 0x7f || cpu->D || cpu->emulation || !cpu->m_flag || cpu->x_flag) \
      ActRaiserHleFatal("Fishing leaf returned outside its ABI"); \
  } while (0)
  A8(cpu, 25);
  CALL(bank_03_F4DF_M1X0, 0xe86b);
  if (cpu->_flag_Z) {
    A8(cpu, 25);
    CALL(bank_03_F4EA_M1X0, 0xe872);
    A8(cpu, 0); cpu_write8(cpu, 0x7f, 0x916e, 0);
    cpu->X = 0xe5f6; ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->X);
    CALL(bank_03_CA93_M1X0, 0xe87e);
  }
  /* Native Palace initialization still restarts an unfinished expedition.
   * Only an actual target change reconciles already-earned progress. Keeping
   * equality otherwise preserves native byte wrap for debug/imported states. */
  const uint8_t before = cpu_read8(cpu, 0x7f, 0x916e);
  const bool reached = reconcile && before >= target;
  const uint8_t progress = reached ? before : (uint8_t)(before + 1u);
  A8(cpu, progress); cpu_write8(cpu, 0x7f, 0x916e, progress);
  cpu->_flag_C = progress >= target;
  cpu->P = (cpu->P & ~1u) | cpu->_flag_C;
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)(progress - target));
  *continuation = reached || progress == target ? 0xe895 : 0xe88c;
  return RECOMP_RETURN_NORMAL;
nonlocal:
  return result; /* entry adapter propagates one native caller level */
#undef CALL
}
