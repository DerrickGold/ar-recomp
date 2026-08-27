#include "actraiser/actraiser_cpu_hle_internal.h"

#include <stdbool.h>

#include "actraiser/actraiser_hle_fatal.h"
#include "snesrecomp/game/runtime_constants.h"

static bool EntryModeMatches(
    const CpuState *cpu, ActRaiserCpuHleEntryMode required_mode) {
  if (!cpu || cpu->emulation || cpu->x_flag) return false;

  switch (required_mode) {
    case kActRaiserCpuHleEntryMode_Native16BitIndexes:
      return true;
    case kActRaiserCpuHleEntryMode_Native8BitAccumulator16BitIndexes:
      return cpu->m_flag;
    case kActRaiserCpuHleEntryMode_Native16BitAccumulatorAndIndexes:
      return !cpu->m_flag;
    case kActRaiserCpuHleEntryMode_DbZeroNative8BitAccumulator16BitIndexes:
      return cpu->m_flag && cpu->DB == 0;
    case kActRaiserCpuHleEntryMode_DirectPageAndDbZeroNative8BitAccumulator16BitIndexes:
      return cpu->m_flag && cpu->D == 0 && cpu->DB == 0;
  }

  return false;
}

void ActRaiserCpuHle_RequireEntryMode(
    CpuState *cpu, const char *routine_name,
    ActRaiserCpuHleEntryMode required_mode) {
  if (EntryModeMatches(cpu, required_mode)) return;

  const char *requirement = "a valid declared CPU entry mode";
  switch (required_mode) {
    case kActRaiserCpuHleEntryMode_Native16BitIndexes:
      requirement = "native mode with 16-bit indexes";
      break;
    case kActRaiserCpuHleEntryMode_Native8BitAccumulator16BitIndexes:
      requirement = "native mode with 8-bit A and 16-bit X/Y";
      break;
    case kActRaiserCpuHleEntryMode_Native16BitAccumulatorAndIndexes:
      requirement = "native mode with 16-bit A/X/Y";
      break;
    case kActRaiserCpuHleEntryMode_DbZeroNative8BitAccumulator16BitIndexes:
      requirement = "DB=0 native mode with 8-bit A and 16-bit X/Y";
      break;
    case kActRaiserCpuHleEntryMode_DirectPageAndDbZeroNative8BitAccumulator16BitIndexes:
      requirement = "D/DB=0 native mode with 8-bit A and 16-bit X/Y";
      break;
  }
  ActRaiserHleFatal("%s HLE requires %s",
                    routine_name ? routine_name : "unnamed routine",
                    requirement);
}

uint16_t ActRaiserCpuHle_PushWordAt(
    CpuState *cpu, uint16_t stack_pointer, uint16_t value) {
  cpu_write8(cpu, k65816StackBank, stack_pointer,
             (uint8_t)(value >> 8));
  stack_pointer = (uint16_t)(stack_pointer - 1);
  cpu_write8(cpu, k65816StackBank, stack_pointer, (uint8_t)value);
  return (uint16_t)(stack_pointer - 1);
}

void ActRaiserCpuHle_PushWord(CpuState *cpu, uint16_t value) {
  cpu->S = ActRaiserCpuHle_PushWordAt(cpu, cpu->S, value);
}

uint16_t ActRaiserCpuHle_PopWord(CpuState *cpu) {
  cpu->S = (uint16_t)(cpu->S + 1);
  const uint16_t value = cpu_read16(cpu, k65816StackBank, cpu->S);
  cpu->S = (uint16_t)(cpu->S + 1);
  return value;
}

void ActRaiserCpuHle_SetNegativeZero8(CpuState *cpu, uint8_t value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & UINT8_C(0x80)) != 0;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N | CPU_P_Z)) |
                     (cpu->_flag_N ? CPU_P_N : 0) |
                     (cpu->_flag_Z ? CPU_P_Z : 0));
}

void ActRaiserCpuHle_SetNegativeZero16(CpuState *cpu, uint16_t value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & UINT16_C(0x8000)) != 0;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N | CPU_P_Z)) |
                     (cpu->_flag_N ? CPU_P_N : 0) |
                     (cpu->_flag_Z ? CPU_P_Z : 0));
}
