#ifndef ACTRAISER_RETRY_H
#define ACTRAISER_RETRY_H

#include "snesrecomp/game/cpu.h"

/* $00:981C..9825. Branch-only actor entry, not a JSR. Returns the native
 * continuation; allocation, actor setup and the rest of the frame stay native. */
bool ActRaiserRetry_Entry(const CpuState *cpu);
uint16_t ActRaiserRetry_Prefix(CpuState *cpu, bool clear_score);

#endif
