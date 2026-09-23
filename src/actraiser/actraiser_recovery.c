#include "actraiser_recovery.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_native_call.h"

extern RecompReturn bank_03_AFBD_M1X0(CpuState *cpu);
extern RecompReturn bank_03_B482_M1X0(CpuState *cpu);
extern RecompReturn bank_03_B456_M1X0(CpuState *cpu);

static bool Shape(const CpuState *cpu) {
  return cpu && !cpu->D && !cpu->emulation;
}
bool ActRaiserRecovery_CycleEntry(const CpuState *cpu) {
  return Shape(cpu) && cpu->PB == 3 && cpu->DB == 0x7f && !cpu->m_flag && !cpu->x_flag;
}
bool ActRaiserRecovery_DrainEntry(const CpuState *cpu) {
  return Shape(cpu) && cpu->PB == 1 && cpu->DB == 1 && !cpu->m_flag && !cpu->x_flag;
}
bool ActRaiserRecovery_MotionEntry(const CpuState *cpu) {
  return Shape(cpu) && cpu->PB == 1 && cpu->DB == 1 && cpu->m_flag && cpu->x_flag;
}
static bool Valid(const ArRegionalRecoverySnapshot *snapshot) {
  return snapshot && (!snapshot->angel_calls || snapshot->angel_calls == 60);
}
static void Width(CpuState *cpu, bool narrow) {
  cpu->m_flag = narrow;
  cpu->P = (cpu->P & ~0x20u) | (narrow ? 0x20 : 0);
}
static void A16(CpuState *cpu, uint16_t value) {
  cpu->A = value;
  ActRaiserCpuHle_SetNegativeZero16(cpu, value);
}
static void A8(CpuState *cpu, uint8_t value) {
  cpu_write_a8(cpu, value);
  ActRaiserCpuHle_SetNegativeZero8(cpu, value);
}
static void Compare16(CpuState *cpu, uint16_t value) {
  cpu->_flag_C = cpu->A >= value;
  cpu->P = (cpu->P & ~1u) | cpu->_flag_C;
  ActRaiserCpuHle_SetNegativeZero16(cpu, (uint16_t)(cpu->A - value));
}

void ActRaiserRecovery_Reconcile(CpuState *cpu, unsigned changed) {
  if (!Shape(cpu) || (changed >> kArRegionalRecovery_Count))
    ActRaiserHleFatal("Invalid recovery reconciliation");
  if (changed & (1u << kArRegionalRecovery_SP)) cpu_write8(cpu, 0, 0x0b05, 0);
  if (changed & (1u << kArRegionalRecovery_Angel)) cpu_write8(cpu, 0, 0x0b04, 0);
}

#define CALL(leaf, caller, long_call, db) do { \
  result = ActRaiserNativeCall(cpu, leaf, 3, caller, long_call); \
  if (result != RECOMP_RETURN_NORMAL) return result; \
  if (!Shape(cpu) || !cpu->m_flag || cpu->x_flag || cpu->DB != db) \
    ActRaiserHleFatal("Recovery callee outside native ABI at %04x", caller); \
} while (0)

RecompReturn ActRaiserRecovery_Cycle(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot) {
  if (!ActRaiserRecovery_CycleEntry(cpu) || !Valid(snapshot))
    ActRaiserHleFatal("Invalid recovery cycle entry");
  cpu_mirrors_to_p(cpu);
  RecompReturn result;
  if (snapshot->cycle_sp) {
    A16(cpu, cpu_read16(cpu, 0, 0x0284));
    cpu->X = cpu->A;
    Width(cpu, true); A8(cpu, 10);
    CALL(bank_03_AFBD_M1X0, 0x827c, false, 0x7f);
    cpu_write8(cpu, 0, 0x0b05, (uint8_t)cpu->A);
    Width(cpu, false);
  }
  if (!snapshot->angel_calls) {
    A16(cpu, cpu_read16(cpu, 0, 0x0287));
    A16(cpu, cpu->A & 0xff); cpu->X = cpu->A;
    Width(cpu, true); A8(cpu, 4);
    CALL(bank_03_AFBD_M1X0, 0x8291, false, 0x7f);
    cpu_write8(cpu, 0, 0x0b04, (uint8_t)cpu->A);
    Width(cpu, false);
  }
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiserRecovery_Drain(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot) {
  if (!ActRaiserRecovery_DrainEntry(cpu) || !Valid(snapshot))
    ActRaiserHleFatal("Invalid recovery drain entry");
  cpu_mirrors_to_p(cpu); Width(cpu, true);
  RecompReturn result;
  if (!snapshot->angel_calls) {
    A8(cpu, cpu_read8(cpu, 0, 0x88)); A8(cpu, (uint8_t)cpu->A & 15);
    if (cpu->_flag_Z) {
      A8(cpu, cpu_read8(cpu, 1, 0x0b04));
      if (!cpu->_flag_Z) {
        const uint8_t next = (uint8_t)(cpu->A - 1);
        cpu_write8(cpu, 1, 0x0b04, next);
        ActRaiserCpuHle_SetNegativeZero8(cpu, next); A8(cpu, 1);
        CALL(bank_03_B482_M1X0, 0xb26c, true, 1);
      }
    }
  }
  if (snapshot->cycle_sp) {
    A8(cpu, cpu_read8(cpu, 0, 0x88)); A8(cpu, (uint8_t)cpu->A & 3);
    if (cpu->_flag_Z) {
      A8(cpu, cpu_read8(cpu, 1, 0x0b05));
      if (!cpu->_flag_Z) {
        const uint8_t next = (uint8_t)(cpu->A - 1);
        cpu_write8(cpu, 1, 0x0b05, next);
        ActRaiserCpuHle_SetNegativeZero8(cpu, next); A8(cpu, 1);
        CALL(bank_03_B456_M1X0, 0xb280, true, 1);
      }
    }
  }
  return RECOMP_RETURN_NORMAL;
}
#undef CALL

void ActRaiserRecovery_Motion(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot, bool stopped) {
  if (!ActRaiserRecovery_MotionEntry(cpu) || !Valid(snapshot))
    ActRaiserHleFatal("Invalid angel recovery entry");
  cpu_mirrors_to_p(cpu); Width(cpu, false);
  if (stopped) {
    cpu_write16(cpu, 1, 0x0afe, 0); cpu_write16(cpu, 1, 0x0b00, 0);
  }
  if (!snapshot->angel_calls) return;
  A16(cpu, cpu_read16(cpu, 1, 0x0af6)); A16(cpu, cpu->A & 15); Compare16(cpu, 4);
  if (cpu->_flag_Z) return;
  /* The JP word overlaps the independent US SP queue. Its bounded phase fits
   * one byte; never read/write the adjacent SP byte in a mixed policy. */
  A16(cpu, cpu_read8(cpu, 1, 0x0b04)); A16(cpu, cpu->A + 1);
  Compare16(cpu, snapshot->angel_calls);
  if (cpu->_flag_C) {
    Width(cpu, true);
    A8(cpu, cpu_read8(cpu, 1, 0x0286));
    const uint8_t maximum = cpu_read8(cpu, 1, 0x0287);
    cpu->_flag_C = (uint8_t)cpu->A >= maximum;
    cpu->P = (cpu->P & ~1u) | cpu->_flag_C;
    ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)(cpu->A - maximum));
    if (!cpu->_flag_Z) {
      /* Preserve the native equality test, including odd imported/debug HP. */
      const uint8_t hp = (uint8_t)(cpu->A + 1);
      cpu_write8(cpu, 1, 0x0286, hp); ActRaiserCpuHle_SetNegativeZero8(cpu, hp);
    }
    Width(cpu, false); A16(cpu, 0);
  }
  cpu_write8(cpu, 1, 0x0b04, (uint8_t)cpu->A);
}
