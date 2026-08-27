#ifndef ACTRAISER_LZSS_H
#define ACTRAISER_LZSS_H

#include "snesrecomp/game/cpu.h"

/* Whole-body HLE for the stock Quintet LZSS driver at $02:C5C9. */
RecompReturn ActRaiser_LzssDecompress(CpuState *cpu);

#endif /* ACTRAISER_LZSS_H */
