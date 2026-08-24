#ifndef ACTRAISER_CPU_HLE_INTERNAL_H
#define ACTRAISER_CPU_HLE_INTERNAL_H

#include <stdint.h>

#include "cpu_state.h"

typedef enum ActRaiserCpuHleEntryMode {
  kActRaiserCpuHleEntryMode_Native16BitIndexes = 0,
  kActRaiserCpuHleEntryMode_Native8BitAccumulator16BitIndexes,
  kActRaiserCpuHleEntryMode_Native16BitAccumulatorAndIndexes,
  kActRaiserCpuHleEntryMode_DbZeroNative8BitAccumulator16BitIndexes,
  kActRaiserCpuHleEntryMode_DirectPageAndDbZeroNative8BitAccumulator16BitIndexes,
} ActRaiserCpuHleEntryMode;

void ActRaiserCpuHle_RequireEntryMode(
    CpuState *cpu, const char *routine_name,
    ActRaiserCpuHleEntryMode required_mode);

uint16_t ActRaiserCpuHle_PushWordAt(
    CpuState *cpu, uint16_t stack_pointer, uint16_t value);
void ActRaiserCpuHle_PushWord(CpuState *cpu, uint16_t value);
uint16_t ActRaiserCpuHle_PopWord(CpuState *cpu);

void ActRaiserCpuHle_SetNegativeZero8(CpuState *cpu, uint8_t value);
void ActRaiserCpuHle_SetNegativeZero16(CpuState *cpu, uint16_t value);

#endif
