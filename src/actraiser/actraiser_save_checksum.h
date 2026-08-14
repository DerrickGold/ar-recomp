#ifndef ACTRAISER_SAVE_CHECKSUM_H
#define ACTRAISER_SAVE_CHECKSUM_H

#include "cpu_state.h"

/* Whole-body HLE for the stock SRAM checksum accumulator at $00:84F3. */
RecompReturn ActRaiser_SaveAccumulateChecksum(CpuState *cpu);

#endif /* ACTRAISER_SAVE_CHECKSUM_H */
